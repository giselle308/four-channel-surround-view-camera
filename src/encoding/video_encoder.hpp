#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "frame_packet.hpp"

enum class VideoCodec {
    H264,
    H265,
    AV1,
};

const char *VideoCodecName(VideoCodec codec) noexcept;
VideoCodec ParseVideoCodec(const std::string &name);

struct EncoderConfig {
    VideoCodec codec = VideoCodec::H264;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps = 80;
    std::uint32_t bitrate = 20'000'000;
    std::uint32_t gop = 40;
    bool low_latency = true;
    std::size_t queue_depth = 2;
};

struct EncodedFrame {
    CameraId camera_id = CameraId::FRONT;
    VideoCodec codec = VideoCodec::H264;
    std::uint64_t frame_number = 0;
    std::uint64_t trigger_cycle = 0;
    std::uint64_t group_id = 0;
    std::uint64_t device_timestamp = 0;
    std::int64_t host_timestamp = 0;
    std::int64_t group_timestamp = 0;
    std::uint64_t pts_ns = 0;
    std::uint64_t duration_ns = 0;
    bool key_frame = false;
    std::shared_ptr<const std::vector<std::uint8_t>> data;
};
