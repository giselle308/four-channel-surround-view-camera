#include "frame_rate_selector.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace {

constexpr double kNanosecondsPerSecond = 1'000'000'000.0;

}  // namespace

FrameRateSelector::FrameRateSelector(double output_fps) : output_fps_(output_fps)
{
    if (!std::isfinite(output_fps_) || output_fps_ <= 0.0) {
        throw std::invalid_argument("output_fps must be finite and positive");
    }
    period_ns_ = static_cast<std::int64_t>(
        std::llround(kNanosecondsPerSecond / output_fps_));
    if (period_ns_ <= 0) {
        throw std::invalid_argument("output_fps produces an invalid period");
    }
}

std::optional<FrameGroup> FrameRateSelector::select(FrameGroup group)
{
    ++statistics_.input_groups;
    const std::int64_t timestamp = group.group_timestamp;
    if (!group.complete() || timestamp <= 0 ||
        (last_input_timestamp_ != 0 && timestamp <= last_input_timestamp_)) {
        ++statistics_.invalid_groups;
        ++statistics_.dropped_groups;
        return std::nullopt;
    }
    last_input_timestamp_ = timestamp;

    if (next_output_timestamp_ == 0) {
        next_output_timestamp_ = timestamp + period_ns_;
        ++statistics_.output_groups;
        return group;
    }

    if (timestamp < next_output_timestamp_) {
        if (pending_group_.has_value()) {
            // The newer pre-deadline group is always closer to the target.
            ++statistics_.dropped_groups;
        }
        pending_group_ = std::move(group);
        return std::nullopt;
    }

    std::optional<FrameGroup> selected;
    if (pending_group_.has_value()) {
        const std::int64_t before_delta =
            next_output_timestamp_ - pending_group_->group_timestamp;
        const std::int64_t after_delta = timestamp - next_output_timestamp_;
        if (after_delta <= before_delta) {
            // Prefer the fresher frame when both are equally close.
            ++statistics_.dropped_groups;
            selected = std::move(group);
            pending_group_.reset();
        } else {
            selected = std::move(pending_group_);
            pending_group_ = std::move(group);
        }
    } else {
        selected = std::move(group);
    }

    ++statistics_.output_groups;
    advanceOutputTimestamp(timestamp);
    return selected;
}

FrameRateSelectorStatistics FrameRateSelector::statistics() const noexcept
{
    FrameRateSelectorStatistics result = statistics_;
    result.has_pending_group = pending_group_.has_value();
    return result;
}

void FrameRateSelector::reset() noexcept
{
    next_output_timestamp_ = 0;
    last_input_timestamp_ = 0;
    pending_group_.reset();
    statistics_ = {};
}

void FrameRateSelector::advanceOutputTimestamp(std::int64_t input_timestamp) noexcept
{
    do {
        next_output_timestamp_ += period_ns_;
    } while (next_output_timestamp_ <= input_timestamp);
}
