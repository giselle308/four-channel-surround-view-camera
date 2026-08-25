#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "frame_buffer.hpp"

struct FrameGroup {
    // Phase 1 uses the already unique trigger cycle as the stable group id.
    // Keeping both fields makes the downstream wire protocol independent of
    // the trigger implementation when GPIO triggering is added later.
    std::uint64_t group_id = 0;
    std::uint64_t trigger_cycle = 0;
    // steady_clock nanoseconds.  The maximum member host timestamp is the
    // instant at which the complete group became available on the NX.
    std::int64_t group_timestamp = 0;
    std::array<std::shared_ptr<const FramePacket>, 4> frames;

    bool complete() const
    {
        for (const auto &frame : frames) {
            if (frame == nullptr) {
                return false;
            }
        }
        return true;
    }
};

class FrameGrouper {
public:
    explicit FrameGrouper(std::array<const FrameBuffer *, 4> buffers)
        : buffers_(buffers)
    {
    }

    FrameGroup find(std::uint64_t trigger_cycle) const
    {
        FrameGroup group;
        group.group_id = trigger_cycle;
        group.trigger_cycle = trigger_cycle;
        for (std::size_t camera_index = 0; camera_index < buffers_.size(); ++camera_index) {
            if (buffers_[camera_index] == nullptr) {
                continue;
            }
            for (const auto &packet : buffers_[camera_index]->snapshot()) {
                if (packet != nullptr && packet->trigger_cycle == trigger_cycle) {
                    group.frames[camera_index] = packet;
                    group.group_timestamp =
                        std::max(group.group_timestamp, packet->host_timestamp);
                    break;
                }
            }
        }
        return group;
    }

    // Returns every complete group still present in the four fixed-capacity
    // ring buffers.  No frame is retained by FrameGrouper itself, and callers
    // can advance last_trigger_cycle to prefer fresh groups after an overrun.
    std::vector<FrameGroup> completeAfter(std::uint64_t last_trigger_cycle) const
    {
        std::array<std::vector<std::shared_ptr<const FramePacket>>, 4> snapshots;
        for (std::size_t index = 0; index < buffers_.size(); ++index) {
            if (buffers_[index] != nullptr) {
                snapshots[index] = buffers_[index]->snapshot();
            }
        }

        std::vector<FrameGroup> groups;
        groups.reserve(FrameBuffer::kCapacity);
        for (const auto &front : snapshots[0]) {
            if (front == nullptr || front->trigger_cycle <= last_trigger_cycle) {
                continue;
            }

            FrameGroup group;
            group.group_id = front->trigger_cycle;
            group.trigger_cycle = front->trigger_cycle;
            group.frames[0] = front;
            group.group_timestamp = front->host_timestamp;

            for (std::size_t camera_index = 1; camera_index < snapshots.size(); ++camera_index) {
                const auto match = std::find_if(
                    snapshots[camera_index].begin(),
                    snapshots[camera_index].end(),
                    [&](const auto &packet) {
                        return packet != nullptr &&
                               packet->trigger_cycle == group.trigger_cycle;
                    });
                if (match == snapshots[camera_index].end()) {
                    break;
                }
                group.frames[camera_index] = *match;
                group.group_timestamp =
                    std::max(group.group_timestamp, (*match)->host_timestamp);
            }
            if (group.complete()) {
                groups.push_back(std::move(group));
            }
        }

        std::sort(groups.begin(), groups.end(), [](const FrameGroup &left, const FrameGroup &right) {
            return left.trigger_cycle < right.trigger_cycle;
        });
        groups.erase(
            std::unique(groups.begin(), groups.end(), [](const FrameGroup &left, const FrameGroup &right) {
                return left.trigger_cycle == right.trigger_cycle;
            }),
            groups.end());
        return groups;
    }

private:
    std::array<const FrameBuffer *, 4> buffers_;
};
