#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "frame_grouper.hpp"
#include "software_trigger_scheduler.hpp"

int main()
{
    std::array<FrameBuffer, 4> buffers;
    const std::array<CameraId, 4> ids = {
        CameraId::FRONT, CameraId::REAR, CameraId::LEFT, CameraId::RIGHT};
    for (std::size_t index = 0; index < buffers.size(); ++index) {
        auto packet = std::make_shared<FramePacket>();
        packet->camera_id = ids[index];
        packet->trigger_cycle = 42;
        buffers[index].push(std::move(packet));
    }

    FrameGrouper grouper({&buffers[0], &buffers[1], &buffers[2], &buffers[3]});
    const FrameGroup complete_group = grouper.find(42);
    assert(complete_group.complete());
    assert(complete_group.group_id == 42);
    assert(complete_group.trigger_cycle == 42);
    assert(!grouper.find(41).complete());

    const auto complete_groups = grouper.completeAfter(41);
    assert(complete_groups.size() == 1);
    assert(complete_groups.front().complete());
    assert(complete_groups.front().trigger_cycle == 42);
    assert(grouper.completeAfter(42).empty());

    std::mutex cycles_mutex;
    std::vector<std::uint64_t> cycles;
    SoftwareTriggerScheduler scheduler(
        [&](std::uint64_t cycle) {
            const std::lock_guard lock(cycles_mutex);
            cycles.push_back(cycle);
            return true;
        },
        100.0);
    assert(scheduler.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(65));
    scheduler.stop();

    const auto statistics = scheduler.statistics();
    assert(statistics.generated_cycles >= 5);
    assert(statistics.generated_cycles <= 9);
    assert(statistics.generated_cycles == statistics.fully_successful_cycles);
    assert(statistics.failed_cycles == 0);
    const std::lock_guard lock(cycles_mutex);
    for (std::size_t index = 0; index < cycles.size(); ++index) {
        assert(cycles[index] == index + 1);
    }
    return 0;
}
