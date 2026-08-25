#pragma once

#include <cstdint>
#include <optional>

#include "frame_grouper.hpp"

struct FrameRateSelectorStatistics {
    std::uint64_t input_groups = 0;
    std::uint64_t output_groups = 0;
    std::uint64_t dropped_groups = 0;
    std::uint64_t invalid_groups = 0;
    bool has_pending_group = false;
};

// Timestamp-driven streaming frame-rate conversion.  It never sleeps and
// retains at most one complete FrameGroup while choosing the input frame
// nearest to the next output timestamp.
class FrameRateSelector {
public:
    explicit FrameRateSelector(double output_fps);

    std::optional<FrameGroup> select(FrameGroup group);
    FrameRateSelectorStatistics statistics() const noexcept;
    void reset() noexcept;

    double outputFps() const noexcept { return output_fps_; }

private:
    void advanceOutputTimestamp(std::int64_t input_timestamp) noexcept;

    double output_fps_ = 0.0;
    std::int64_t period_ns_ = 0;
    std::int64_t next_output_timestamp_ = 0;
    std::int64_t last_input_timestamp_ = 0;
    std::optional<FrameGroup> pending_group_;
    FrameRateSelectorStatistics statistics_;
};
