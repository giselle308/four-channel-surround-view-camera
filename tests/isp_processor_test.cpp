#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

#include "isp_processor.hpp"

namespace {

void Require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main()
{
    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 48;
    FrameGroup input;
    input.group_id = 7;
    input.trigger_cycle = 7;
    input.group_timestamp = 1'000'000;
    for (std::size_t index = 0; index < input.frames.size(); ++index) {
        auto packet = std::make_shared<FramePacket>();
        packet->camera_id = static_cast<CameraId>(index);
        packet->frame_number = 100 + index;
        packet->trigger_cycle = 7;
        packet->host_timestamp = input.group_timestamp - static_cast<std::int64_t>(index);
        packet->width = kWidth;
        packet->height = kHeight;
        auto bayer = std::make_shared<std::vector<std::uint8_t>>(kWidth * kHeight);
        for (std::uint32_t row = 0; row < kHeight; ++row) {
            for (std::uint32_t column = 0; column < kWidth; ++column) {
                // Known BayerRG8/RGGB field: R=200, G=100, B=50.
                const bool even_row = row % 2 == 0;
                const bool even_column = column % 2 == 0;
                (*bayer)[row * kWidth + column] =
                    even_row && even_column ? 200 :
                    (!even_row && !even_column ? 50 : 100);
            }
        }
        packet->image_data = std::move(bayer);
        input.frames[index] = std::move(packet);
    }

    IspConfig config;
    config.backend = IspBackend::OPENCV_CPU;
    config.width = kWidth;
    config.height = kHeight;
    IspProcessor processor(config);
    IspProcessResult output;
    std::string error;
    if (!processor.process(input, 4, output, &error)) {
        throw std::runtime_error(error);
    }
    Require(output.processed_frames == 4, "CPU ISP did not process four frames");
    Require(output.group.group_id == input.group_id, "CPU ISP lost group_id");
    for (std::size_t index = 0; index < output.group.frames.size(); ++index) {
        const auto &frame = output.group.frames[index];
        Require(frame != nullptr, "CPU ISP output frame is missing");
        Require(frame->frame_number == 100 + index, "CPU ISP lost frame_number");
        Require(frame->trigger_cycle == input.trigger_cycle, "CPU ISP lost trigger_cycle");
        Require(frame->image_data != nullptr, "CPU ISP NV12 buffer is missing");
        Require(frame->image_data->size() == kWidth * kHeight * 3 / 2,
                "CPU ISP NV12 buffer has the wrong size");
        Require(output.frame_latency_ms[index] >= 0.0, "CPU ISP latency is invalid");
    }

    const std::size_t y_index = (kHeight / 2) * kWidth + kWidth / 2;
    const std::size_t uv_index = static_cast<std::size_t>(kWidth) * kHeight +
                                 (kHeight / 4) * kWidth + kWidth / 2;
    const auto &cpu_nv12 = *output.group.frames[0]->image_data;
    std::cout << "CPU NV12 center YUV=" << static_cast<int>(cpu_nv12[y_index]) << ','
              << static_cast<int>(cpu_nv12[uv_index]) << ','
              << static_cast<int>(cpu_nv12[uv_index + 1]) << '\n';
    Require(cpu_nv12[y_index] > 110 && cpu_nv12[y_index] < 140,
            "CPU ISP Y value does not match the known RGGB field");
    Require(cpu_nv12[uv_index] > 75 && cpu_nv12[uv_index] < 110,
            "CPU ISP U value does not match the known RGGB field");
    Require(cpu_nv12[uv_index + 1] > 160 && cpu_nv12[uv_index + 1] < 195,
            "CPU ISP V value does not match the known RGGB field");

#if ISP_HAS_CUDA_NPP
    config.backend = IspBackend::CUDA_NPP;
    IspProcessor cuda_processor(config);
    IspProcessResult cuda_output;
    if (!cuda_processor.process(input, 1, cuda_output, &error)) {
        throw std::runtime_error(error);
    }
    const auto &cuda_nv12 = *cuda_output.group.frames[0]->image_data;
    std::cout << "CUDA NV12 center YUV=" << static_cast<int>(cuda_nv12[y_index]) << ','
              << static_cast<int>(cuda_nv12[uv_index]) << ','
              << static_cast<int>(cuda_nv12[uv_index + 1]) << '\n';
    Require(std::abs(static_cast<int>(cuda_nv12[y_index]) -
                     static_cast<int>(cpu_nv12[y_index])) <= 3,
            "CUDA ISP Y differs from CPU reference");
    Require(std::abs(static_cast<int>(cuda_nv12[uv_index]) -
                     static_cast<int>(cpu_nv12[uv_index])) <= 3,
            "CUDA ISP U differs from CPU reference");
    Require(std::abs(static_cast<int>(cuda_nv12[uv_index + 1]) -
                     static_cast<int>(cpu_nv12[uv_index + 1])) <= 3,
            "CUDA ISP V differs from CPU reference");
#endif
    return 0;
}
