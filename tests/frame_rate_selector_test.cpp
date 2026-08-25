#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>

#include "frame_rate_selector.hpp"

namespace {

FrameGroup MakeGroup(std::uint64_t cycle, std::int64_t timestamp)
{
    FrameGroup group;
    group.group_id = cycle;
    group.trigger_cycle = cycle;
    group.group_timestamp = timestamp;
    for (std::size_t index = 0; index < group.frames.size(); ++index) {
        auto packet = std::make_shared<FramePacket>();
        packet->camera_id = static_cast<CameraId>(index);
        packet->trigger_cycle = cycle;
        packet->host_timestamp = timestamp - static_cast<std::int64_t>(3 - index) * 1'000;
        group.frames[index] = std::move(packet);
    }
    return group;
}

}  // namespace

int main()
{
    FrameRateSelector selector(80.0);
    std::uint64_t emitted = 0;
    std::int64_t previous_output_timestamp = 0;
    constexpr std::uint64_t kInputGroups = 1'000;
    constexpr std::int64_t kStartTimestamp = 1'000'000'000;

    for (std::uint64_t cycle = 1; cycle <= kInputGroups; ++cycle) {
        // Deterministic +/-0.2 ms capture jitter verifies timestamp-driven
        // selection without assuming every fifth source frame is the drop.
        const std::int64_t jitter =
            static_cast<std::int64_t>(cycle % 5) * 100'000 - 200'000;
        const std::int64_t timestamp =
            kStartTimestamp + static_cast<std::int64_t>(cycle - 1) * 10'000'000 + jitter;
        auto output = selector.select(MakeGroup(cycle, timestamp));
        if (!output.has_value()) {
            continue;
        }
        assert(output->complete());
        assert(output->group_id == output->trigger_cycle);
        assert(output->group_timestamp > previous_output_timestamp);
        previous_output_timestamp = output->group_timestamp;
        ++emitted;
    }

    const FrameRateSelectorStatistics stats = selector.statistics();
    assert(stats.input_groups == kInputGroups);
    assert(stats.output_groups == emitted);
    assert(stats.invalid_groups == 0);
    assert(emitted >= 799 && emitted <= 801);
    assert(stats.dropped_groups + stats.output_groups +
               static_cast<std::uint64_t>(stats.has_pending_group) ==
           stats.input_groups);

    selector.reset();
    assert(selector.statistics().input_groups == 0);
    assert(selector.select(MakeGroup(1, kStartTimestamp)).has_value());
    auto invalid = MakeGroup(2, kStartTimestamp);
    assert(!selector.select(std::move(invalid)).has_value());
    assert(selector.statistics().invalid_groups == 1);
    return 0;
}
