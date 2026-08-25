#include <cstdint>
#include <memory>
#include <stdexcept>

#include "metadata_sender.hpp"

namespace {

std::uint64_t ReadU64(const std::vector<std::uint8_t> &data, std::size_t offset)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8) | data[offset + index];
    }
    return value;
}

void Require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main()
{
    ProcessedFrameGroup group;
    group.group_id = 0x0102030405060708ULL;
    group.trigger_cycle = 42;
    group.group_timestamp = 12'500'000;
    for (std::size_t index = 0; index < group.frames.size(); ++index) {
        auto frame = std::make_shared<Nv12Frame>();
        frame->frame_number = 100 + index;
        frame->device_timestamp = 200 + index;
        frame->host_timestamp = 300 + static_cast<std::int64_t>(index);
        group.frames[index] = std::move(frame);
    }
    const auto packet = SerializeFrameGroupMetadata(group);
    Require(packet.size() == kFrameGroupMetadataPacketSize, "wire size mismatch");
    Require(packet[0] == 'S' && packet[1] == 'V' && packet[2] == 'M' && packet[3] == 'D',
            "wire magic mismatch");
    Require(ReadU64(packet, 8) == group.group_id, "wire group_id mismatch");
    Require(ReadU64(packet, 16) == group.trigger_cycle, "wire trigger_cycle mismatch");
    Require(ReadU64(packet, 40) == 100, "wire front frame number mismatch");
    Require(RtpTimestampFromNanoseconds(group.group_timestamp) == 1125,
            "90 kHz RTP timestamp conversion mismatch");
    return 0;
}
