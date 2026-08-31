#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "video_encoder.hpp"

enum class OutputMode {
    NULL_OUTPUT,
    FILE_OUTPUT,
    RTP_UDP,
    SRT,
};

const char *OutputModeName(OutputMode mode) noexcept;
OutputMode ParseOutputMode(const std::string &name);

struct StreamerConfig {
    OutputMode mode = OutputMode::NULL_OUTPUT;
    VideoCodec codec = VideoCodec::H264;
    std::string server_ip = "127.0.0.1";
    std::uint16_t port = 0;
    std::uint8_t payload_type = 96;
    std::uint32_t mtu = 1400;
    std::uint32_t srt_latency_ms = 120;
    std::size_t queue_depth = 2;
    std::string file_path;
};

struct StreamerStatistics {
    std::uint64_t submitted_frames = 0;
    std::uint64_t sent_frames = 0;
    std::uint64_t sent_bytes = 0;
    std::uint64_t queue_drops = 0;
    std::uint64_t errors = 0;
    std::size_t queue_size = 0;
    double average_end_to_end_latency_ms = 0.0;
    double p95_end_to_end_latency_ms = 0.0;
    double max_end_to_end_latency_ms = 0.0;
    std::string transport_stats;
};

class IStreamer {
public:
    virtual ~IStreamer() = default;
    virtual bool start(std::string *error = nullptr) = 0;
    virtual bool send(EncodedFrame frame) = 0;
    virtual void stop() = 0;
    virtual StreamerStatistics statistics() const = 0;
};

std::unique_ptr<IStreamer> CreateStreamer(StreamerConfig config);

struct MultiStreamOutputConfig {
    OutputMode mode = OutputMode::NULL_OUTPUT;
    VideoCodec codec = VideoCodec::H264;
    std::string server_ip = "127.0.0.1";
    std::array<std::uint16_t, 4> ports = {5000, 5002, 5004, 5006};
    std::uint8_t payload_type = 96;
    std::uint32_t mtu = 1400;
    std::uint32_t srt_latency_ms = 120;
    std::size_t queue_depth = 2;
    std::size_t active_camera_count = 4;
    std::string file_directory = "encoded";
};

class MultiStreamOutput {
public:
    explicit MultiStreamOutput(MultiStreamOutputConfig config);
    ~MultiStreamOutput();

    bool start(std::string *error = nullptr);
    bool send(EncodedFrame frame);
    void stop();
    std::array<StreamerStatistics, 4> statistics() const;
    const MultiStreamOutputConfig &config() const noexcept { return config_; }

private:
    MultiStreamOutputConfig config_;
    std::array<std::unique_ptr<IStreamer>, 4> streamers_;
};
