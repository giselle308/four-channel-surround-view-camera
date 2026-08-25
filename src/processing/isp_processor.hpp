#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "frame_grouper.hpp"

enum class IspBackend {
    CUDA_NPP,
    OPENCV_CPU,
};

const char *IspBackendName(IspBackend backend) noexcept;
IspBackend ParseIspBackend(const std::string &name);

struct IspConfig {
    IspBackend backend = IspBackend::CUDA_NPP;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float white_balance_red = 1.0F;
    float white_balance_green = 1.0F;
    float white_balance_blue = 1.0F;
    float gamma = 1.0F;
    std::array<float, 9> color_matrix = {
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F,
    };
    std::size_t output_buffers_per_camera = 4;
};

struct Nv12Frame {
    CameraId camera_id = CameraId::FRONT;
    std::uint64_t frame_number = 0;
    std::uint64_t trigger_cycle = 0;
    std::uint64_t group_id = 0;
    std::uint64_t device_timestamp = 0;
    std::int64_t host_timestamp = 0;
    std::int64_t group_timestamp = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> image_data;
};

struct ProcessedFrameGroup {
    std::uint64_t group_id = 0;
    std::uint64_t trigger_cycle = 0;
    std::int64_t group_timestamp = 0;
    std::array<std::shared_ptr<const Nv12Frame>, 4> frames;
};

struct IspProcessResult {
    ProcessedFrameGroup group;
    std::array<double, 4> frame_latency_ms{};
    std::size_t processed_frames = 0;
};

class IspProcessor {
public:
    explicit IspProcessor(IspConfig config);
    ~IspProcessor();

    IspProcessor(const IspProcessor &) = delete;
    IspProcessor &operator=(const IspProcessor &) = delete;

    bool process(const FrameGroup &input,
                 std::size_t active_camera_count,
                 IspProcessResult &output,
                 std::string *error = nullptr);
    const IspConfig &config() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
