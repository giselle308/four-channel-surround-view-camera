#include "cuda_npp_isp_backend.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

#include <cuda_runtime.h>
#include <nppcore.h>
#include <nppi_color_conversion.h>

namespace {

struct ColorParameters {
    float white_balance_red;
    float white_balance_green;
    float white_balance_blue;
    float gamma_inverse;
    float color_matrix[9];
};

std::string CudaError(const char *operation, cudaError_t error)
{
    return std::string(operation) + " failed: " + cudaGetErrorString(error);
}

std::string NppError(const char *operation, NppStatus status)
{
    std::ostringstream stream;
    stream << operation << " failed, NPP status=" << static_cast<int>(status);
    return stream.str();
}

__device__ unsigned char ClampByte(float value)
{
    value = fminf(255.0F, fmaxf(0.0F, value));
    return static_cast<unsigned char>(__float2int_rn(value));
}

__device__ void CorrectRgb(const unsigned char *rgb,
                           const ColorParameters &parameters,
                           float &red,
                           float &green,
                           float &blue)
{
    const float wb_red = static_cast<float>(rgb[0]) * parameters.white_balance_red;
    const float wb_green = static_cast<float>(rgb[1]) * parameters.white_balance_green;
    const float wb_blue = static_cast<float>(rgb[2]) * parameters.white_balance_blue;

    red = parameters.color_matrix[0] * wb_red +
          parameters.color_matrix[1] * wb_green +
          parameters.color_matrix[2] * wb_blue;
    green = parameters.color_matrix[3] * wb_red +
            parameters.color_matrix[4] * wb_green +
            parameters.color_matrix[5] * wb_blue;
    blue = parameters.color_matrix[6] * wb_red +
           parameters.color_matrix[7] * wb_green +
           parameters.color_matrix[8] * wb_blue;

    red = fminf(255.0F, fmaxf(0.0F, red));
    green = fminf(255.0F, fmaxf(0.0F, green));
    blue = fminf(255.0F, fmaxf(0.0F, blue));
    if (fabsf(parameters.gamma_inverse - 1.0F) > 0.0001F) {
        red = 255.0F * powf(red / 255.0F, parameters.gamma_inverse);
        green = 255.0F * powf(green / 255.0F, parameters.gamma_inverse);
        blue = 255.0F * powf(blue / 255.0F, parameters.gamma_inverse);
    }
}

__global__ void RgbToYKernel(const unsigned char *rgb,
                             unsigned char *y_plane,
                             int width,
                             int height,
                             ColorParameters parameters)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    CorrectRgb(rgb + (y * width + x) * 3, parameters, red, green, blue);
    y_plane[y * width + x] = ClampByte(16.0F +
        (65.738F * red + 129.057F * green + 25.064F * blue) / 256.0F);
}

__global__ void RgbToUvKernel(const unsigned char *rgb,
                              unsigned char *uv_plane,
                              int width,
                              int height,
                              ColorParameters parameters)
{
    const int chroma_x = blockIdx.x * blockDim.x + threadIdx.x;
    const int chroma_y = blockIdx.y * blockDim.y + threadIdx.y;
    if (chroma_x >= width / 2 || chroma_y >= height / 2) {
        return;
    }

    float red_sum = 0.0F;
    float green_sum = 0.0F;
    float blue_sum = 0.0F;
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            float red = 0.0F;
            float green = 0.0F;
            float blue = 0.0F;
            const int x = chroma_x * 2 + dx;
            const int y = chroma_y * 2 + dy;
            CorrectRgb(rgb + (y * width + x) * 3, parameters, red, green, blue);
            red_sum += red;
            green_sum += green;
            blue_sum += blue;
        }
    }
    const float red = red_sum * 0.25F;
    const float green = green_sum * 0.25F;
    const float blue = blue_sum * 0.25F;
    const int offset = chroma_y * width + chroma_x * 2;
    uv_plane[offset] = ClampByte(128.0F +
        (-37.945F * red - 74.494F * green + 112.439F * blue) / 256.0F);
    uv_plane[offset + 1] = ClampByte(128.0F +
        (112.439F * red - 94.154F * green - 18.285F * blue) / 256.0F);
}

}  // namespace

class CudaNppIspBackend::Impl {
public:
    explicit Impl(const IspConfig &config)
        : width(config.width),
          height(config.height),
          bayer_size(static_cast<std::size_t>(width) * height),
          rgb_size(bayer_size * 3),
          nv12_size(bayer_size * 3 / 2)
    {
        try {
            parameters.white_balance_red = config.white_balance_red;
            parameters.white_balance_green = config.white_balance_green;
            parameters.white_balance_blue = config.white_balance_blue;
            parameters.gamma_inverse = 1.0F / config.gamma;
            std::copy(config.color_matrix.begin(), config.color_matrix.end(), parameters.color_matrix);

            cudaError_t cuda_status = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
            if (cuda_status != cudaSuccess) throw std::runtime_error(CudaError("cudaStreamCreate", cuda_status));
            cuda_status = cudaEventCreate(&start_event);
            if (cuda_status != cudaSuccess) throw std::runtime_error(CudaError("cudaEventCreate(start)", cuda_status));
            cuda_status = cudaEventCreate(&stop_event);
            if (cuda_status != cudaSuccess) throw std::runtime_error(CudaError("cudaEventCreate(stop)", cuda_status));
            cuda_status = cudaMalloc(reinterpret_cast<void **>(&device_bayer), bayer_size);
            if (cuda_status != cudaSuccess) throw std::runtime_error(CudaError("cudaMalloc(Bayer)", cuda_status));
            cuda_status = cudaMalloc(reinterpret_cast<void **>(&device_rgb), rgb_size);
            if (cuda_status != cudaSuccess) throw std::runtime_error(CudaError("cudaMalloc(RGB)", cuda_status));
            cuda_status = cudaMalloc(reinterpret_cast<void **>(&device_nv12), nv12_size);
            if (cuda_status != cudaSuccess) throw std::runtime_error(CudaError("cudaMalloc(NV12)", cuda_status));

            NppStatus npp_status = nppGetStreamContext(&npp_context);
            if (npp_status != NPP_SUCCESS) throw std::runtime_error(NppError("nppGetStreamContext", npp_status));
            npp_context.hStream = stream;
            cuda_status = cudaStreamGetFlags(stream, &npp_context.nStreamFlags);
            if (cuda_status != cudaSuccess) throw std::runtime_error(CudaError("cudaStreamGetFlags", cuda_status));
        } catch (...) {
            release();
            throw;
        }
    }

    ~Impl()
    {
        release();
    }

    void release() noexcept
    {
        if (device_nv12 != nullptr) cudaFree(device_nv12);
        if (device_rgb != nullptr) cudaFree(device_rgb);
        if (device_bayer != nullptr) cudaFree(device_bayer);
        if (stop_event != nullptr) cudaEventDestroy(stop_event);
        if (start_event != nullptr) cudaEventDestroy(start_event);
        if (stream != nullptr) cudaStreamDestroy(stream);
        device_nv12 = nullptr;
        device_rgb = nullptr;
        device_bayer = nullptr;
        stop_event = nullptr;
        start_event = nullptr;
        stream = nullptr;
    }

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t bayer_size = 0;
    std::size_t rgb_size = 0;
    std::size_t nv12_size = 0;
    unsigned char *device_bayer = nullptr;
    unsigned char *device_rgb = nullptr;
    unsigned char *device_nv12 = nullptr;
    cudaStream_t stream = nullptr;
    cudaEvent_t start_event = nullptr;
    cudaEvent_t stop_event = nullptr;
    NppStreamContext npp_context{};
    ColorParameters parameters{};
};

CudaNppIspBackend::CudaNppIspBackend(const IspConfig &config)
    : impl_(std::make_unique<Impl>(config))
{
}

CudaNppIspBackend::~CudaNppIspBackend() = default;

bool CudaNppIspBackend::process(const FramePacket &input,
                                std::vector<std::uint8_t> &output_nv12,
                                double &latency_ms,
                                std::string *error)
{
    if (input.image_data == nullptr || input.image_data->size() < impl_->bayer_size) {
        if (error != nullptr) *error = "CUDA ISP input Bayer buffer is incomplete";
        return false;
    }
    output_nv12.resize(impl_->nv12_size);

    auto fail_cuda = [&](const char *operation, cudaError_t status) {
        if (error != nullptr) *error = CudaError(operation, status);
        return false;
    };
    cudaError_t cuda_status = cudaEventRecord(impl_->start_event, impl_->stream);
    if (cuda_status != cudaSuccess) return fail_cuda("cudaEventRecord(start)", cuda_status);
    cuda_status = cudaMemcpyAsync(impl_->device_bayer,
                                  input.image_data->data(),
                                  impl_->bayer_size,
                                  cudaMemcpyHostToDevice,
                                  impl_->stream);
    if (cuda_status != cudaSuccess) return fail_cuda("Bayer host-to-device copy", cuda_status);

    const NppiSize source_size = {
        static_cast<int>(impl_->width), static_cast<int>(impl_->height)};
    const NppiRect source_roi = {
        0, 0, static_cast<int>(impl_->width), static_cast<int>(impl_->height)};
    const NppStatus npp_status = nppiCFAToRGB_8u_C1C3R_Ctx(
        impl_->device_bayer,
        static_cast<int>(impl_->width),
        source_size,
        source_roi,
        impl_->device_rgb,
        static_cast<int>(impl_->width * 3),
        NPPI_BAYER_RGGB,
        NPPI_INTER_UNDEFINED,
        impl_->npp_context);
    if (npp_status != NPP_SUCCESS) {
        if (error != nullptr) *error = NppError("nppiCFAToRGB_8u_C1C3R_Ctx", npp_status);
        return false;
    }

    constexpr dim3 block(16, 16);
    const dim3 y_grid((impl_->width + block.x - 1) / block.x,
                      (impl_->height + block.y - 1) / block.y);
    RgbToYKernel<<<y_grid, block, 0, impl_->stream>>>(
        impl_->device_rgb,
        impl_->device_nv12,
        static_cast<int>(impl_->width),
        static_cast<int>(impl_->height),
        impl_->parameters);
    const dim3 uv_grid((impl_->width / 2 + block.x - 1) / block.x,
                       (impl_->height / 2 + block.y - 1) / block.y);
    RgbToUvKernel<<<uv_grid, block, 0, impl_->stream>>>(
        impl_->device_rgb,
        impl_->device_nv12 + impl_->bayer_size,
        static_cast<int>(impl_->width),
        static_cast<int>(impl_->height),
        impl_->parameters);
    cuda_status = cudaGetLastError();
    if (cuda_status != cudaSuccess) return fail_cuda("RGB-to-NV12 kernel launch", cuda_status);

    cuda_status = cudaMemcpyAsync(output_nv12.data(),
                                  impl_->device_nv12,
                                  impl_->nv12_size,
                                  cudaMemcpyDeviceToHost,
                                  impl_->stream);
    if (cuda_status != cudaSuccess) return fail_cuda("NV12 device-to-host copy", cuda_status);
    cuda_status = cudaEventRecord(impl_->stop_event, impl_->stream);
    if (cuda_status != cudaSuccess) return fail_cuda("cudaEventRecord(stop)", cuda_status);
    cuda_status = cudaEventSynchronize(impl_->stop_event);
    if (cuda_status != cudaSuccess) return fail_cuda("CUDA ISP synchronize", cuda_status);

    float elapsed_ms = 0.0F;
    cuda_status = cudaEventElapsedTime(&elapsed_ms, impl_->start_event, impl_->stop_event);
    if (cuda_status != cudaSuccess) return fail_cuda("cudaEventElapsedTime", cuda_status);
    latency_ms = elapsed_ms;
    return true;
}
