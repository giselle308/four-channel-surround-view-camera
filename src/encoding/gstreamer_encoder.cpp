#include "gstreamer_encoder.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace {

const char *CameraName(CameraId camera_id)
{
    switch (camera_id) {
        case CameraId::FRONT: return "Front";
        case CameraId::REAR: return "Rear";
        case CameraId::LEFT: return "Left";
        case CameraId::RIGHT: return "Right";
    }
    return "Unknown";
}

}  // namespace

const char *VideoCodecName(VideoCodec codec) noexcept
{
    switch (codec) {
        case VideoCodec::H264: return "h264";
        case VideoCodec::H265: return "h265";
        case VideoCodec::AV1: return "av1";
    }
    return "unknown";
}

VideoCodec ParseVideoCodec(const std::string &name)
{
    if (name == "h264") return VideoCodec::H264;
    if (name == "h265") return VideoCodec::H265;
    if (name == "av1") return VideoCodec::AV1;
    throw std::invalid_argument("unsupported codec: " + name);
}

GStreamerEncoder::GStreamerEncoder(CameraId camera_id,
                                   EncoderConfig config,
                                   OutputCallback output_callback)
    : camera_id_(camera_id),
      config_(std::move(config)),
      output_callback_(std::move(output_callback)),
      input_queue_(config_.queue_depth)
{
    if (config_.width == 0 || config_.height == 0 || config_.fps == 0 ||
        config_.bitrate == 0 || config_.gop == 0 || config_.queue_depth == 0) {
        throw std::invalid_argument("encoder dimensions, FPS, bitrate, GOP, and queue depth must be positive");
    }
    gst_init(nullptr, nullptr);
}

GStreamerEncoder::~GStreamerEncoder()
{
    stop();
}

bool GStreamerEncoder::buildPipeline(std::string *error)
{
    const bool av1 = config_.codec == VideoCodec::AV1;
    const char *encoder = config_.codec == VideoCodec::H264
                              ? "nvv4l2h264enc"
                              : (config_.codec == VideoCodec::H265
                                     ? "nvv4l2h265enc"
                                     : "nvv4l2av1enc");
    const char *parser = config_.codec == VideoCodec::H264
                             ? "h264parse"
                             : (config_.codec == VideoCodec::H265 ? "h265parse" : "av1parse");
    const char *encoded_caps = config_.codec == VideoCodec::H264
                                   ? "video/x-h264"
                                   : (config_.codec == VideoCodec::H265
                                          ? "video/x-h265"
                                          : "video/x-av1");

    std::ostringstream description;
    description
        << "appsrc name=input is-live=true format=time do-timestamp=false block=false "
        << "caps=video/x-raw,format=NV12,width=" << config_.width
        << ",height=" << config_.height << ",framerate=" << config_.fps << "/1 "
        << "! queue max-size-buffers=" << config_.queue_depth
        << " max-size-bytes=0 max-size-time=0 leaky=downstream "
        << "! nvvidconv ! video/x-raw(memory:NVMM),format=NV12 "
        << "! " << encoder
        << " bitrate=" << config_.bitrate
        << " iframeinterval=" << config_.gop
        << " idrinterval=" << config_.gop
        << (av1 ? " insert-seq-hdr=true" : " insert-sps-pps=true")
        << " copy-timestamp=true maxperf-enable=true control-rate=1 "
        << (av1 ? "" : "num-B-Frames=0 ")
        << "! " << parser << (av1 ? " " : " config-interval=-1 ")
        << "! " << encoded_caps
        << (av1 ? ",stream-format=obu-stream,alignment=tu "
                : ",stream-format=byte-stream,alignment=au ")
        << "! appsink name=output emit-signals=false sync=false max-buffers="
        << config_.queue_depth << " drop=true";

    GError *gst_error = nullptr;
    pipeline_ = gst_parse_launch(description.str().c_str(), &gst_error);
    if (pipeline_ == nullptr || gst_error != nullptr) {
        if (error != nullptr) {
            *error = gst_error != nullptr ? gst_error->message : "gst_parse_launch returned null";
        }
        if (gst_error != nullptr) g_error_free(gst_error);
        if (pipeline_ != nullptr) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
        return false;
    }

    app_src_ = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline_), "input"));
    app_sink_ = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline_), "output"));
    bus_ = gst_element_get_bus(pipeline_);
    if (app_src_ == nullptr || app_sink_ == nullptr || bus_ == nullptr) {
        if (error != nullptr) *error = "failed to resolve appsrc/appsink/bus in encoder pipeline";
        return false;
    }
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = &GStreamerEncoder::HandleNewSample;
    gst_app_sink_set_callbacks(app_sink_, &callbacks, this, nullptr);
    return true;
}

bool GStreamerEncoder::start(std::string *error)
{
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!buildPipeline(error)) {
        stop();
        return false;
    }
    const GstStateChangeReturn state_result =
        gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (state_result == GST_STATE_CHANGE_FAILURE) {
        if (error != nullptr) *error = "failed to set encoder pipeline to PLAYING";
        stop();
        return false;
    }
    running_.store(true, std::memory_order_release);
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
    return true;
}

bool GStreamerEncoder::submit(std::shared_ptr<const Nv12Frame> frame)
{
    if (!running_.load(std::memory_order_acquire) || frame == nullptr) {
        return false;
    }
    submitted_frames_.fetch_add(1, std::memory_order_relaxed);
    const auto result = input_queue_.push(std::move(frame));
    if (result.dropped_oldest) {
        queue_drops_.fetch_add(1, std::memory_order_relaxed);
    }
    return result.accepted;
}

void GStreamerEncoder::run(std::stop_token stop_token)
{
    const std::size_t expected_size =
        static_cast<std::size_t>(config_.width) * config_.height * 3 / 2;
    const GstClockTime duration = gst_util_uint64_scale_int(1, GST_SECOND, config_.fps);
    std::shared_ptr<const Nv12Frame> frame;
    while (input_queue_.waitPop(frame, stop_token)) {
        if (frame == nullptr || frame->image_data == nullptr ||
            frame->image_data->size() < expected_size) {
            errors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, expected_size, nullptr);
        if (buffer == nullptr) {
            errors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        GstMapInfo map{};
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            gst_buffer_unref(buffer);
            errors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        std::memcpy(map.data, frame->image_data->data(), expected_size);
        gst_buffer_unmap(buffer, &map);
        const std::uint64_t pts = static_cast<std::uint64_t>(frame->group_timestamp);
        GST_BUFFER_PTS(buffer) = pts;
        GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DURATION(buffer) = duration;

        {
            const std::lock_guard lock(pending_mutex_);
            if (pending_frames_.size() == kPendingCapacity) {
                pending_frames_.pop_front();
                encoder_drops_.fetch_add(1, std::memory_order_relaxed);
            }
            pending_frames_.push_back({pts, std::chrono::steady_clock::now(), frame});
        }
        const GstFlowReturn push_result = gst_app_src_push_buffer(app_src_, buffer);
        if (push_result != GST_FLOW_OK) {
            errors_.fetch_add(1, std::memory_order_relaxed);
            spdlog::error("{} encoder appsrc push failed: {}",
                          CameraName(camera_id_),
                          static_cast<int>(push_result));
        }
        checkBus();
    }
}

GstFlowReturn GStreamerEncoder::HandleNewSample(GstAppSink *sink, gpointer user_data)
{
    return static_cast<GStreamerEncoder *>(user_data)->handleNewSample(sink);
}

GstFlowReturn GStreamerEncoder::handleNewSample(GstAppSink *sink)
{
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
        return GST_FLOW_EOS;
    }
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map{};
    if (buffer == nullptr || !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        errors_.fetch_add(1, std::memory_order_relaxed);
        return GST_FLOW_ERROR;
    }

    const std::uint64_t pts = GST_BUFFER_PTS_IS_VALID(buffer) ? GST_BUFFER_PTS(buffer) : 0;
    PendingFrame pending;
    bool matched = false;
    {
        const std::lock_guard lock(pending_mutex_);
        const auto match = std::find_if(
            pending_frames_.begin(), pending_frames_.end(),
            [pts](const PendingFrame &item) { return item.pts_ns == pts; });
        if (match != pending_frames_.end()) {
            encoder_drops_.fetch_add(
                static_cast<std::uint64_t>(std::distance(pending_frames_.begin(), match)),
                std::memory_order_relaxed);
            pending = std::move(*match);
            pending_frames_.erase(pending_frames_.begin(), std::next(match));
            matched = true;
        }
    }

    if (!matched || pending.frame == nullptr) {
        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);
        encoder_drops_.fetch_add(1, std::memory_order_relaxed);
        return GST_FLOW_OK;
    }

    auto bytes = std::make_shared<std::vector<std::uint8_t>>(map.size);
    std::memcpy(bytes->data(), map.data, map.size);
    const bool key_frame = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    const std::uint64_t encoded_duration = GST_BUFFER_DURATION_IS_VALID(buffer)
                                               ? GST_BUFFER_DURATION(buffer)
                                               : gst_util_uint64_scale_int(
                                                     1, GST_SECOND, config_.fps);
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    EncodedFrame encoded;
    encoded.camera_id = pending.frame->camera_id;
    encoded.codec = config_.codec;
    encoded.frame_number = pending.frame->frame_number;
    encoded.trigger_cycle = pending.frame->trigger_cycle;
    encoded.group_id = pending.frame->group_id;
    encoded.device_timestamp = pending.frame->device_timestamp;
    encoded.host_timestamp = pending.frame->host_timestamp;
    encoded.group_timestamp = pending.frame->group_timestamp;
    encoded.pts_ns = pts;
    encoded.duration_ns = encoded_duration;
    encoded.key_frame = key_frame;
    encoded.data = std::move(bytes);

    encoded_frames_.fetch_add(1, std::memory_order_relaxed);
    encoded_bytes_.fetch_add(encoded.data->size(), std::memory_order_relaxed);
    recordLatency(std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - pending.submitted_at)
                      .count());
    if (output_callback_) {
        try {
            output_callback_(std::move(encoded));
        } catch (const std::exception &exception) {
            errors_.fetch_add(1, std::memory_order_relaxed);
            spdlog::error("{} encoder output callback failed: {}",
                          CameraName(camera_id_), exception.what());
        }
    }
    return GST_FLOW_OK;
}

void GStreamerEncoder::checkBus()
{
    while (bus_ != nullptr) {
        GstMessage *message = gst_bus_pop_filtered(
            bus_, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
        if (message == nullptr) {
            break;
        }
        GError *error = nullptr;
        gchar *debug = nullptr;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            gst_message_parse_error(message, &error, &debug);
            errors_.fetch_add(1, std::memory_order_relaxed);
            spdlog::error("{} encoder GStreamer error: {} debug={}",
                          CameraName(camera_id_),
                          error != nullptr ? error->message : "unknown",
                          debug != nullptr ? debug : "");
        } else {
            gst_message_parse_warning(message, &error, &debug);
            spdlog::warn("{} encoder GStreamer warning: {} debug={}",
                         CameraName(camera_id_),
                         error != nullptr ? error->message : "unknown",
                         debug != nullptr ? debug : "");
        }
        if (error != nullptr) g_error_free(error);
        if (debug != nullptr) g_free(debug);
        gst_message_unref(message);
    }
}

void GStreamerEncoder::recordLatency(double latency_ms)
{
    const std::lock_guard lock(latency_mutex_);
    latency_samples_.push_back(latency_ms);
    latency_sum_ms_ += latency_ms;
    max_latency_ms_ = std::max(max_latency_ms_, latency_ms);
    if (latency_samples_.size() > kLatencyWindowSize) {
        latency_sum_ms_ -= latency_samples_.front();
        latency_samples_.pop_front();
    }
}

GStreamerEncoderStatistics GStreamerEncoder::statistics() const
{
    GStreamerEncoderStatistics result;
    result.submitted_frames = submitted_frames_.load(std::memory_order_relaxed);
    result.encoded_frames = encoded_frames_.load(std::memory_order_relaxed);
    result.encoded_bytes = encoded_bytes_.load(std::memory_order_relaxed);
    result.queue_drops = queue_drops_.load(std::memory_order_relaxed);
    result.encoder_drops = encoder_drops_.load(std::memory_order_relaxed);
    result.errors = errors_.load(std::memory_order_relaxed);
    result.queue_size = input_queue_.size();
    const std::lock_guard lock(latency_mutex_);
    if (!latency_samples_.empty()) {
        result.average_latency_ms = latency_sum_ms_ / latency_samples_.size();
        result.max_latency_ms = max_latency_ms_;
        std::vector<double> sorted(latency_samples_.begin(), latency_samples_.end());
        std::sort(sorted.begin(), sorted.end());
        result.p95_latency_ms = sorted[static_cast<std::size_t>(
            0.95 * static_cast<double>(sorted.size() - 1))];
    }
    return result;
}

void GStreamerEncoder::stop()
{
    running_.store(false, std::memory_order_release);
    input_queue_.close();
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    if (app_src_ != nullptr) {
        gst_app_src_end_of_stream(app_src_);
    }
    if (pipeline_ != nullptr) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
    if (bus_ != nullptr) {
        gst_object_unref(bus_);
        bus_ = nullptr;
    }
    if (app_sink_ != nullptr) {
        gst_object_unref(app_sink_);
        app_sink_ = nullptr;
    }
    if (app_src_ != nullptr) {
        gst_object_unref(app_src_);
        app_src_ = nullptr;
    }
    if (pipeline_ != nullptr) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}
