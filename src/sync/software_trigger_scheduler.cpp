#include "software_trigger_scheduler.hpp"

#include <cmath>
#include <utility>

SoftwareTriggerScheduler::SoftwareTriggerScheduler(TriggerCallback callback, double frequency_hz)
    : callback_(std::move(callback))
{
    if (std::isfinite(frequency_hz) && frequency_hz > 0.0) {
        period_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(1.0 / frequency_hz));
    }
}

SoftwareTriggerScheduler::~SoftwareTriggerScheduler()
{
    stop();
}

bool SoftwareTriggerScheduler::start()
{
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!callback_ || period_.count() <= 0) {
        return false;
    }

    current_cycle_.store(0, std::memory_order_relaxed);
    successful_cycles_.store(0, std::memory_order_relaxed);
    failed_cycles_.store(0, std::memory_order_relaxed);
    scheduler_overruns_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
    return true;
}

void SoftwareTriggerScheduler::stop()
{
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

void SoftwareTriggerScheduler::run(std::stop_token stop_token)
{
    auto next_wakeup = std::chrono::steady_clock::now();
    while (!stop_token.stop_requested()) {
        const std::uint64_t cycle = current_cycle_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (callback_(cycle)) {
            successful_cycles_.fetch_add(1, std::memory_order_relaxed);
        } else {
            failed_cycles_.fetch_add(1, std::memory_order_relaxed);
        }

        next_wakeup += period_;
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_wakeup) {
            scheduler_overruns_.fetch_add(1, std::memory_order_relaxed);
            next_wakeup = now + period_;
        }
        std::this_thread::sleep_until(next_wakeup);
    }
}

SoftwareTriggerStatistics SoftwareTriggerScheduler::statistics() const
{
    SoftwareTriggerStatistics result;
    result.generated_cycles = current_cycle_.load(std::memory_order_relaxed);
    result.fully_successful_cycles = successful_cycles_.load(std::memory_order_relaxed);
    result.failed_cycles = failed_cycles_.load(std::memory_order_relaxed);
    result.scheduler_overruns = scheduler_overruns_.load(std::memory_order_relaxed);
    return result;
}
