#include "isp_processor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "cuda_npp_isp_backend.hpp"

namespace {

std::uint8_t ClampByte(float value)
{
    return static_cast<std::uint8_t>(
        std::clamp(std::lround(value), 0L, 255L));
}

bool ProcessOpenCvCpu(const IspConfig &config,
                      const FramePacket &input,
                      std::vector<std::uint8_t> &output,
                      double &latency_ms,
                      std::string *error)
{
    const auto start = std::chrono::steady_clock::now();
    const std::size_t bayer_size =
        static_cast<std::size_t>(config.width) * config.height;
    if (input.image_data == nullptr || input.image_data->size() < bayer_size) {
        if (error != nullptr) *error = "OpenCV ISP input Bayer buffer is incomplete";
        return false;
    }

    try {
        cv::Mat bayer(static_cast<int>(config.height),
                      static_cast<int>(config.width),
                      CV_8UC1,
                      const_cast<std::uint8_t *>(input.image_data->data()));
        cv::Mat bgr;
        // Matches the verified preview conversion for MVS BayerRG8.
        cv::cvtColor(bayer, bgr, cv::COLOR_BayerBG2BGR);

        const float gamma_inverse = 1.0F / config.gamma;
        for (int row = 0; row < bgr.rows; ++row) {
            auto *pixels = bgr.ptr<cv::Vec3b>(row);
            for (int column = 0; column < bgr.cols; ++column) {
                const float red = pixels[column][2] * config.white_balance_red;
                const float green = pixels[column][1] * config.white_balance_green;
                const float blue = pixels[column][0] * config.white_balance_blue;
                float corrected_red = config.color_matrix[0] * red +
                                      config.color_matrix[1] * green +
                                      config.color_matrix[2] * blue;
                float corrected_green = config.color_matrix[3] * red +
                                        config.color_matrix[4] * green +
                                        config.color_matrix[5] * blue;
                float corrected_blue = config.color_matrix[6] * red +
                                       config.color_matrix[7] * green +
                                       config.color_matrix[8] * blue;
                if (std::abs(gamma_inverse - 1.0F) > 0.0001F) {
                    corrected_red = 255.0F * std::pow(
                        std::clamp(corrected_red, 0.0F, 255.0F) / 255.0F, gamma_inverse);
                    corrected_green = 255.0F * std::pow(
                        std::clamp(corrected_green, 0.0F, 255.0F) / 255.0F, gamma_inverse);
                    corrected_blue = 255.0F * std::pow(
                        std::clamp(corrected_blue, 0.0F, 255.0F) / 255.0F, gamma_inverse);
                }
                pixels[column] = cv::Vec3b(
                    ClampByte(corrected_blue),
                    ClampByte(corrected_green),
                    ClampByte(corrected_red));
            }
        }

        cv::Mat i420;
        cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);
        const std::size_t y_size = bayer_size;
        const std::size_t chroma_size = bayer_size / 4;
        output.resize(bayer_size * 3 / 2);
        const auto *i420_data = i420.ptr<std::uint8_t>();
        std::memcpy(output.data(), i420_data, y_size);
        const auto *u_plane = i420_data + y_size;
        const auto *v_plane = u_plane + chroma_size;
        auto *uv_plane = output.data() + y_size;
        for (std::size_t index = 0; index < chroma_size; ++index) {
            uv_plane[index * 2] = u_plane[index];
            uv_plane[index * 2 + 1] = v_plane[index];
        }
    } catch (const cv::Exception &exception) {
        if (error != nullptr) *error = std::string("OpenCV ISP failed: ") + exception.what();
        return false;
    }

    latency_ms = std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - start)
                     .count();
    return true;
}

}  // namespace

const char *IspBackendName(IspBackend backend) noexcept
{
    switch (backend) {
        case IspBackend::CUDA_NPP: return "cuda_npp";
        case IspBackend::OPENCV_CPU: return "opencv_cpu";
    }
    return "unknown";
}

IspBackend ParseIspBackend(const std::string &name)
{
    if (name == "cuda_npp") return IspBackend::CUDA_NPP;
    if (name == "opencv_cpu") return IspBackend::OPENCV_CPU;
    throw std::invalid_argument("unsupported ISP backend: " + name);
}

class IspProcessor::Impl {
public:
    explicit Impl(IspConfig requested_config) : config(std::move(requested_config))
    {
        if (config.width == 0 || config.height == 0 ||
            config.width % 2 != 0 || config.height % 2 != 0) {
            throw std::invalid_argument("ISP width and height must be positive even values");
        }
        if (!std::isfinite(config.gamma) || config.gamma <= 0.0F ||
            !std::isfinite(config.white_balance_red) || config.white_balance_red <= 0.0F ||
            !std::isfinite(config.white_balance_green) || config.white_balance_green <= 0.0F ||
            !std::isfinite(config.white_balance_blue) || config.white_balance_blue <= 0.0F) {
            throw std::invalid_argument("ISP white balance and gamma must be finite and positive");
        }
        if (config.output_buffers_per_camera == 0) {
            throw std::invalid_argument("ISP output buffer pool must not be empty");
        }

        const std::size_t output_size =
            static_cast<std::size_t>(config.width) * config.height * 3 / 2;
        for (std::size_t camera_index = 0; camera_index < output_pool.size(); ++camera_index) {
            output_pool[camera_index].reserve(config.output_buffers_per_camera);
            for (std::size_t index = 0; index < config.output_buffers_per_camera; ++index) {
                output_pool[camera_index].push_back(
                    std::make_shared<std::vector<std::uint8_t>>(output_size));
            }
            if (config.backend == IspBackend::CUDA_NPP) {
                cuda_backends[camera_index] = std::make_unique<CudaNppIspBackend>(config);
            }
        }
    }

    std::shared_ptr<std::vector<std::uint8_t>> acquireOutputBuffer(std::size_t camera_index)
    {
        for (const auto &buffer : output_pool[camera_index]) {
            if (buffer.use_count() == 1) {
                return buffer;
            }
        }
        return {};
    }

    IspConfig config;
    std::array<std::unique_ptr<CudaNppIspBackend>, 4> cuda_backends;
    std::array<std::vector<std::shared_ptr<std::vector<std::uint8_t>>>, 4> output_pool;
};

IspProcessor::IspProcessor(IspConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

IspProcessor::~IspProcessor() = default;

bool IspProcessor::process(const FrameGroup &input,
                           std::size_t active_camera_count,
                           IspProcessResult &output,
                           std::string *error)
{
    output = {};
    if (!input.complete()) {
        if (error != nullptr) *error = "ISP requires a complete four-camera FrameGroup";
        return false;
    }
    if (active_camera_count == 0 || active_camera_count > input.frames.size()) {
        if (error != nullptr) *error = "active ISP camera count must be 1, 2, 3, or 4";
        return false;
    }

    output.group.group_id = input.group_id;
    output.group.trigger_cycle = input.trigger_cycle;
    output.group.group_timestamp = input.group_timestamp;
    for (std::size_t camera_index = 0; camera_index < active_camera_count; ++camera_index) {
        const auto &input_frame = input.frames[camera_index];
        if (input_frame->width != impl_->config.width ||
            input_frame->height != impl_->config.height) {
            if (error != nullptr) *error = "ISP input dimensions do not match configuration";
            return false;
        }
        auto output_buffer = impl_->acquireOutputBuffer(camera_index);
        if (output_buffer == nullptr) {
            if (error != nullptr) *error = "ISP NV12 output buffer pool exhausted";
            return false;
        }

        double latency_ms = 0.0;
        bool succeeded = false;
        if (impl_->config.backend == IspBackend::CUDA_NPP) {
            succeeded = impl_->cuda_backends[camera_index]->process(
                *input_frame, *output_buffer, latency_ms, error);
        } else {
            succeeded = ProcessOpenCvCpu(
                impl_->config, *input_frame, *output_buffer, latency_ms, error);
        }
        if (!succeeded) {
            return false;
        }

        auto processed = std::make_shared<Nv12Frame>();
        processed->camera_id = input_frame->camera_id;
        processed->frame_number = input_frame->frame_number;
        processed->trigger_cycle = input_frame->trigger_cycle;
        processed->group_id = input.group_id;
        processed->device_timestamp = input_frame->device_timestamp;
        processed->host_timestamp = input_frame->host_timestamp;
        processed->group_timestamp = input.group_timestamp;
        processed->width = input_frame->width;
        processed->height = input_frame->height;
        processed->image_data = std::move(output_buffer);
        output.group.frames[camera_index] = std::move(processed);
        output.frame_latency_ms[camera_index] = latency_ms;
        ++output.processed_frames;
    }
    return true;
}

const IspConfig &IspProcessor::config() const noexcept
{
    return impl_->config;
}
