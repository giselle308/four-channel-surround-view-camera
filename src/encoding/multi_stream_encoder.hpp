#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "gstreamer_encoder.hpp"

struct MultiStreamEncoderConfig {
    EncoderConfig common;
    std::array<std::uint32_t, 4> bitrates = {
        20'000'000, 20'000'000, 20'000'000, 20'000'000};
    std::size_t active_camera_count = 4;
};

class MultiStreamEncoder {
public:
    using OutputCallback = GStreamerEncoder::OutputCallback;

    MultiStreamEncoder(MultiStreamEncoderConfig config,
                       OutputCallback output_callback = {});
    ~MultiStreamEncoder();

    bool start(std::string *error = nullptr);
    bool submit(ProcessedFrameGroup group);
    void stop();
    std::array<GStreamerEncoderStatistics, 4> statistics() const;
    const MultiStreamEncoderConfig &config() const noexcept { return config_; }

private:
    MultiStreamEncoderConfig config_;
    OutputCallback output_callback_;
    std::array<std::unique_ptr<GStreamerEncoder>, 4> encoders_;
};
