#pragma once

#include <cstdint>
#include <memory>
#include <vector>

enum class CameraId : std::uint8_t {
    FRONT,
    REAR,
    LEFT,
    RIGHT,
};

struct FramePacket {
    CameraId camera_id = CameraId::FRONT;
    std::uint64_t frame_number = 0;
    std::uint64_t trigger_cycle = 0;
    std::uint64_t device_timestamp = 0;
    std::int64_t host_timestamp = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pixel_format = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> image_data;
};
