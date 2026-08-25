#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

#include "bounded_latest_queue.hpp"
#include "isp_processor.hpp"

struct IspPipelineConfig {
    IspConfig processor;
    std::size_t active_camera_count = 4;
    std::size_t queue_depth = 2;
};

struct IspPipelineStatistics {
    std::uint64_t submitted_groups = 0;
    std::uint64_t processed_groups = 0;
    std::uint64_t processed_frames = 0;
    std::uint64_t queue_drops = 0;
    std::uint64_t processing_errors = 0;
    std::size_t queue_size = 0;
    double average_frame_latency_ms = 0.0;
    double p95_frame_latency_ms = 0.0;
    double max_frame_latency_ms = 0.0;
};

class IspPipeline {
public:
    using OutputCallback = std::function<void(ProcessedFrameGroup)>;

    IspPipeline(IspPipelineConfig config, OutputCallback output_callback = {});
    ~IspPipeline();

    IspPipeline(const IspPipeline &) = delete;
    IspPipeline &operator=(const IspPipeline &) = delete;

    bool start();
    bool submit(FrameGroup group);
    void stop();
    IspPipelineStatistics statistics() const;
    const IspPipelineConfig &config() const noexcept { return config_; }

private:
    void run(std::stop_token stop_token);
    void recordLatency(double latency_ms);

    static constexpr std::size_t kLatencyWindowSize = 4096;

    IspPipelineConfig config_;
    OutputCallback output_callback_;
    IspProcessor processor_;
    BoundedLatestQueue<FrameGroup> input_queue_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> submitted_groups_{0};
    std::atomic<std::uint64_t> processed_groups_{0};
    std::atomic<std::uint64_t> processed_frames_{0};
    std::atomic<std::uint64_t> queue_drops_{0};
    std::atomic<std::uint64_t> processing_errors_{0};
    mutable std::mutex latency_mutex_;
    std::deque<double> latency_samples_;
    double latency_sum_ms_ = 0.0;
    double max_latency_ms_ = 0.0;
};
