#pragma once

#include <atomic>
#include <stop_token>
#include <thread>

#include "multi_camera_manager.hpp"

class FourCameraPreview {
public:
    explicit FourCameraPreview(const MultiCameraManager &manager);
    ~FourCameraPreview();

    FourCameraPreview(const FourCameraPreview &) = delete;
    FourCameraPreview &operator=(const FourCameraPreview &) = delete;

    bool start();
    void stop();
    bool closeRequested() const noexcept
    {
        return close_requested_.load(std::memory_order_acquire);
    }

private:
    void run(std::stop_token stop_token);

    const MultiCameraManager &manager_;
    std::jthread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> close_requested_{false};
};
