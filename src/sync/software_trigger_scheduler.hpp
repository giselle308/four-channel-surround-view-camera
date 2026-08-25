#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <thread>

#include "trigger_generator.hpp"

struct SoftwareTriggerStatistics {
    std::uint64_t generated_cycles = 0;
    std::uint64_t fully_successful_cycles = 0;
    std::uint64_t failed_cycles = 0;
    std::uint64_t scheduler_overruns = 0;
};

class SoftwareTriggerScheduler final : public TriggerGenerator {
public:
    using TriggerCallback = std::function<bool(std::uint64_t)>;

    explicit SoftwareTriggerScheduler(TriggerCallback callback, double frequency_hz = 100.0);
    ~SoftwareTriggerScheduler() override;

    bool start() override;
    void stop() override;
    bool isRunning() const override { return running_.load(std::memory_order_acquire); }
    std::uint64_t currentCycle() const override { return current_cycle_.load(std::memory_order_relaxed); }
    SoftwareTriggerStatistics statistics() const;

private:
    void run(std::stop_token stop_token);

    TriggerCallback callback_;
    std::chrono::nanoseconds period_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> current_cycle_{0};
    std::atomic<std::uint64_t> successful_cycles_{0};
    std::atomic<std::uint64_t> failed_cycles_{0};
    std::atomic<std::uint64_t> scheduler_overruns_{0};
};
