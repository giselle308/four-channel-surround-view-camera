#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "bounded_latest_queue.hpp"
#include "isp_processor.hpp"

struct MetadataSenderConfig {
    std::string server_ip = "127.0.0.1";
    std::uint16_t port = 5100;
    std::size_t queue_depth = 4;
};

struct MetadataSenderStatistics {
    std::uint64_t submitted_packets = 0;
    std::uint64_t sent_packets = 0;
    std::uint64_t queue_drops = 0;
    std::uint64_t errors = 0;
    std::size_t queue_size = 0;
};

inline constexpr std::uint32_t kFrameGroupMetadataMagic = 0x53564D44U;  // SVMD
inline constexpr std::uint16_t kFrameGroupMetadataVersion = 1;
inline constexpr std::size_t kFrameGroupMetadataPacketSize = 136;

std::uint32_t RtpTimestampFromNanoseconds(std::int64_t timestamp_ns) noexcept;
std::vector<std::uint8_t> SerializeFrameGroupMetadata(const ProcessedFrameGroup &group);

class MetadataSender {
public:
    explicit MetadataSender(MetadataSenderConfig config);
    ~MetadataSender();

    bool start(std::string *error = nullptr);
    bool send(const ProcessedFrameGroup &group);
    void stop();
    MetadataSenderStatistics statistics() const;

private:
    void run(std::stop_token stop_token);

    MetadataSenderConfig config_;
    BoundedLatestQueue<std::vector<std::uint8_t>> queue_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    int socket_fd_ = -1;
    std::unique_ptr<class MetadataSocketAddress> address_;
    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> drops_{0};
    std::atomic<std::uint64_t> errors_{0};
};
