#include "metadata_sender.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

class MetadataSocketAddress {
public:
    sockaddr_in value{};
};

namespace {

void PutU16(std::vector<std::uint8_t> &packet, std::uint16_t value)
{
    packet.push_back(static_cast<std::uint8_t>(value >> 8));
    packet.push_back(static_cast<std::uint8_t>(value));
}

void PutU32(std::vector<std::uint8_t> &packet, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        packet.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void PutU64(std::vector<std::uint8_t> &packet, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        packet.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

}  // namespace

std::uint32_t RtpTimestampFromNanoseconds(std::int64_t timestamp_ns) noexcept
{
    if (timestamp_ns <= 0) return 0;
    constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
    constexpr std::uint64_t kRtpClockRate = 90'000ULL;
    const std::uint64_t timestamp = static_cast<std::uint64_t>(timestamp_ns);
    const std::uint64_t seconds = timestamp / kNanosecondsPerSecond;
    const std::uint64_t remainder = timestamp % kNanosecondsPerSecond;
    return static_cast<std::uint32_t>(
        seconds * kRtpClockRate + remainder * kRtpClockRate / kNanosecondsPerSecond);
}

std::vector<std::uint8_t> SerializeFrameGroupMetadata(const ProcessedFrameGroup &group)
{
    std::vector<std::uint8_t> packet;
    packet.reserve(kFrameGroupMetadataPacketSize);
    PutU32(packet, kFrameGroupMetadataMagic);
    PutU16(packet, kFrameGroupMetadataVersion);
    PutU16(packet, static_cast<std::uint16_t>(kFrameGroupMetadataPacketSize));
    PutU64(packet, group.group_id);
    PutU64(packet, group.trigger_cycle);
    PutU64(packet, static_cast<std::uint64_t>(group.group_timestamp));
    PutU32(packet, RtpTimestampFromNanoseconds(group.group_timestamp));
    PutU32(packet, 0);
    for (const auto &frame : group.frames) {
        PutU64(packet, frame != nullptr ? frame->frame_number : 0);
        PutU64(packet, frame != nullptr ? frame->device_timestamp : 0);
        PutU64(packet, frame != nullptr
                           ? static_cast<std::uint64_t>(frame->host_timestamp)
                           : 0);
    }
    if (packet.size() != kFrameGroupMetadataPacketSize) {
        throw std::logic_error("metadata wire packet size mismatch");
    }
    return packet;
}

MetadataSender::MetadataSender(MetadataSenderConfig config)
    : config_(std::move(config)),
      queue_(config_.queue_depth),
      address_(std::make_unique<MetadataSocketAddress>())
{
    if (config_.port == 0 || config_.queue_depth == 0) {
        throw std::invalid_argument("metadata port and queue depth must be positive");
    }
}

MetadataSender::~MetadataSender()
{
    stop();
}

bool MetadataSender::start(std::string *error)
{
    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_fd_ < 0) {
        if (error != nullptr) *error = "failed to create metadata UDP socket";
        return false;
    }
    address_->value.sin_family = AF_INET;
    address_->value.sin_port = htons(config_.port);
    if (inet_pton(AF_INET, config_.server_ip.c_str(), &address_->value.sin_addr) != 1) {
        if (error != nullptr) *error = "metadata server_ip must be an IPv4 address";
        stop();
        return false;
    }
    running_.store(true, std::memory_order_release);
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
    return true;
}

bool MetadataSender::send(const ProcessedFrameGroup &group)
{
    if (!running_.load(std::memory_order_acquire)) return false;
    submitted_.fetch_add(1, std::memory_order_relaxed);
    const auto result = queue_.push(SerializeFrameGroupMetadata(group));
    if (result.dropped_oldest) drops_.fetch_add(1, std::memory_order_relaxed);
    return result.accepted;
}

void MetadataSender::run(std::stop_token stop_token)
{
    std::vector<std::uint8_t> packet;
    while (queue_.waitPop(packet, stop_token)) {
        const ssize_t bytes_sent = ::sendto(
            socket_fd_,
            packet.data(),
            packet.size(),
            0,
            reinterpret_cast<const sockaddr *>(&address_->value),
            sizeof(address_->value));
        if (bytes_sent != static_cast<ssize_t>(packet.size())) {
            errors_.fetch_add(1, std::memory_order_relaxed);
        } else {
            sent_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void MetadataSender::stop()
{
    running_.store(false, std::memory_order_release);
    queue_.close();
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
}

MetadataSenderStatistics MetadataSender::statistics() const
{
    MetadataSenderStatistics result;
    result.submitted_packets = submitted_.load(std::memory_order_relaxed);
    result.sent_packets = sent_.load(std::memory_order_relaxed);
    result.queue_drops = drops_.load(std::memory_order_relaxed);
    result.errors = errors_.load(std::memory_order_relaxed);
    result.queue_size = queue_.size();
    return result;
}
