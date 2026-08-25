#include "multi_stream_encoder.hpp"

#include <stdexcept>
#include <utility>

MultiStreamEncoder::MultiStreamEncoder(MultiStreamEncoderConfig config,
                                       OutputCallback output_callback)
    : config_(std::move(config)), output_callback_(std::move(output_callback))
{
    if (config_.active_camera_count == 0 || config_.active_camera_count > 4) {
        throw std::invalid_argument("encoder active_camera_count must be between 1 and 4");
    }
    const std::array<CameraId, 4> camera_ids = {
        CameraId::FRONT, CameraId::REAR, CameraId::LEFT, CameraId::RIGHT};
    for (std::size_t index = 0; index < config_.active_camera_count; ++index) {
        EncoderConfig stream_config = config_.common;
        stream_config.bitrate = config_.bitrates[index];
        encoders_[index] = std::make_unique<GStreamerEncoder>(
            camera_ids[index], stream_config, output_callback_);
    }
}

MultiStreamEncoder::~MultiStreamEncoder()
{
    stop();
}

bool MultiStreamEncoder::start(std::string *error)
{
    for (std::size_t index = 0; index < config_.active_camera_count; ++index) {
        if (!encoders_[index]->start(error)) {
            stop();
            return false;
        }
    }
    return true;
}

bool MultiStreamEncoder::submit(ProcessedFrameGroup group)
{
    bool accepted = true;
    for (std::size_t index = 0; index < config_.active_camera_count; ++index) {
        if (group.frames[index] == nullptr ||
            !encoders_[index]->submit(std::move(group.frames[index]))) {
            accepted = false;
        }
    }
    return accepted;
}

void MultiStreamEncoder::stop()
{
    for (auto &encoder : encoders_) {
        if (encoder != nullptr) {
            encoder->stop();
        }
    }
}

std::array<GStreamerEncoderStatistics, 4> MultiStreamEncoder::statistics() const
{
    std::array<GStreamerEncoderStatistics, 4> result;
    for (std::size_t index = 0; index < encoders_.size(); ++index) {
        if (encoders_[index] != nullptr) {
            result[index] = encoders_[index]->statistics();
        }
    }
    return result;
}
