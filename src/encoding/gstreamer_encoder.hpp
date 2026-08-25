#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "bounded_latest_queue.hpp"
#include "isp_processor.hpp"
#include "video_encoder.hpp"

struct GStreamerEncoderStatistics {
    std::uint64_t submitted_frames = 0;
    std::uint64_t encoded_frames = 0;
    std::uint64_t encoded_bytes = 0;
    std::uint64_t queue_drops = 0;
    std::uint64_t encoder_drops = 0;
    std::uint64_t errors = 0;
    std::size_t queue_size = 0;
    double average_latency_ms = 0.0;
    double p95_latency_ms = 0.0;
    double max_latency_ms = 0.0;
};

class GStreamerEncoder {
public:
    using OutputCallback = std::function<void(EncodedFrame)>;

    GStreamerEncoder(CameraId camera_id,
                     EncoderConfig config,
                     OutputCallback output_callback = {});
    ~GStreamerEncoder();

    GStreamerEncoder(const GStreamerEncoder &) = delete;
    GStreamerEncoder &operator=(const GStreamerEncoder &) = delete;

    bool start(std::string *error = nullptr);
    bool submit(std::shared_ptr<const Nv12Frame> frame);
    void stop();
    GStreamerEncoderStatistics statistics() const;
    const EncoderConfig &config() const noexcept { return config_; }

private:
    struct PendingFrame {
        std::uint64_t pts_ns = 0;
        std::chrono::steady_clock::time_point submitted_at;
        std::shared_ptr<const Nv12Frame> frame;
    };

    static GstFlowReturn HandleNewSample(GstAppSink *sink, gpointer user_data);
    GstFlowReturn handleNewSample(GstAppSink *sink);
    void run(std::stop_token stop_token);
    bool buildPipeline(std::string *error);
    void checkBus();
    void recordLatency(double latency_ms);

    static constexpr std::size_t kLatencyWindowSize = 4096;
    static constexpr std::size_t kPendingCapacity = 16;

    CameraId camera_id_;
    EncoderConfig config_;
    OutputCallback output_callback_;
    BoundedLatestQueue<std::shared_ptr<const Nv12Frame>> input_queue_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    GstElement *pipeline_ = nullptr;
    GstAppSrc *app_src_ = nullptr;
    GstAppSink *app_sink_ = nullptr;
    GstBus *bus_ = nullptr;
    std::mutex pending_mutex_;
    std::deque<PendingFrame> pending_frames_;
    std::atomic<std::uint64_t> submitted_frames_{0};
    std::atomic<std::uint64_t> encoded_frames_{0};
    std::atomic<std::uint64_t> encoded_bytes_{0};
    std::atomic<std::uint64_t> queue_drops_{0};
    std::atomic<std::uint64_t> encoder_drops_{0};
    std::atomic<std::uint64_t> errors_{0};
    mutable std::mutex latency_mutex_;
    std::deque<double> latency_samples_;
    double latency_sum_ms_ = 0.0;
    double max_latency_ms_ = 0.0;
};
