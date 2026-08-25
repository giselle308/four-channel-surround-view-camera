#include "isp_pipeline.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

IspPipeline::IspPipeline(IspPipelineConfig config, OutputCallback output_callback)
    : config_(std::move(config)),
      output_callback_(std::move(output_callback)),
      processor_(config_.processor),
      input_queue_(config_.queue_depth)
{
    if (config_.active_camera_count == 0 || config_.active_camera_count > 4) {
        throw std::invalid_argument("ISP active_camera_count must be between 1 and 4");
    }
    if (config_.queue_depth == 0) {
        throw std::invalid_argument("ISP queue_depth must be positive");
    }
}

IspPipeline::~IspPipeline()
{
    stop();
}

bool IspPipeline::start()
{
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
    return true;
}

bool IspPipeline::submit(FrameGroup group)
{
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    submitted_groups_.fetch_add(1, std::memory_order_relaxed);
    const auto result = input_queue_.push(std::move(group));
    if (result.dropped_oldest) {
        queue_drops_.fetch_add(1, std::memory_order_relaxed);
    }
    return result.accepted;
}

void IspPipeline::stop()
{
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    input_queue_.close();
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

void IspPipeline::run(std::stop_token stop_token)
{
    std::uint64_t logged_errors = 0;
    FrameGroup input;
    while (input_queue_.waitPop(input, stop_token)) {
        IspProcessResult output;
        std::string error;
        if (!processor_.process(
                input, config_.active_camera_count, output, &error)) {
            processing_errors_.fetch_add(1, std::memory_order_relaxed);
            if (logged_errors < 5) {
                ++logged_errors;
                spdlog::error("ISP processing failed for cycle={}: {}",
                              input.trigger_cycle,
                              error);
            }
            continue;
        }

        processed_groups_.fetch_add(1, std::memory_order_relaxed);
        processed_frames_.fetch_add(output.processed_frames, std::memory_order_relaxed);
        for (std::size_t index = 0; index < output.processed_frames; ++index) {
            recordLatency(output.frame_latency_ms[index]);
        }
        if (output_callback_) {
            try {
                output_callback_(std::move(output.group));
            } catch (const std::exception &exception) {
                processing_errors_.fetch_add(1, std::memory_order_relaxed);
                spdlog::error("ISP output callback failed: {}", exception.what());
            }
        }
    }
}

void IspPipeline::recordLatency(double latency_ms)
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

IspPipelineStatistics IspPipeline::statistics() const
{
    IspPipelineStatistics result;
    result.submitted_groups = submitted_groups_.load(std::memory_order_relaxed);
    result.processed_groups = processed_groups_.load(std::memory_order_relaxed);
    result.processed_frames = processed_frames_.load(std::memory_order_relaxed);
    result.queue_drops = queue_drops_.load(std::memory_order_relaxed);
    result.processing_errors = processing_errors_.load(std::memory_order_relaxed);
    result.queue_size = input_queue_.size();

    const std::lock_guard lock(latency_mutex_);
    if (!latency_samples_.empty()) {
        result.average_frame_latency_ms =
            latency_sum_ms_ / static_cast<double>(latency_samples_.size());
        result.max_frame_latency_ms = max_latency_ms_;
        std::vector<double> sorted(latency_samples_.begin(), latency_samples_.end());
        std::sort(sorted.begin(), sorted.end());
        const std::size_t p95_index =
            static_cast<std::size_t>(0.95 * static_cast<double>(sorted.size() - 1));
        result.p95_frame_latency_ms = sorted[p95_index];
    }
    return result;
}
