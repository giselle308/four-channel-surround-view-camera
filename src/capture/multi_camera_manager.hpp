#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "frame_buffer.hpp"
#include "hik_camera.hpp"

struct CameraBinding {
    CameraId camera_id;
    std::string serial;
};

struct MultiCameraManagerConfig {
    std::array<CameraBinding, 4> cameras;
    HikCameraParameters camera_parameters;
    unsigned int grab_timeout_ms = 100;
};

struct CameraStatistics {
    std::uint64_t received_frames = 0;
    std::uint64_t frame_number_drops = 0;
    std::uint64_t grab_timeouts = 0;
    std::uint64_t grab_errors = 0;
    std::uint64_t invalid_frames = 0;
    std::uint64_t buffer_overwrites = 0;
    std::uint64_t buffer_pool_exhausted = 0;
    std::uint64_t trigger_failures = 0;
    std::uint64_t unmatched_trigger_frames = 0;
    std::uint64_t last_frame_number = 0;
    std::uint64_t last_device_timestamp = 0;
};

class MultiCameraManager {
public:
    MultiCameraManager();
    ~MultiCameraManager();

    MultiCameraManager(const MultiCameraManager &) = delete;
    MultiCameraManager &operator=(const MultiCameraManager &) = delete;

    bool openAll(const MultiCameraManagerConfig &config, std::string *error = nullptr);
    bool startCapture(std::string *error = nullptr);
    bool softwareTriggerAll(std::uint64_t trigger_cycle);
    void stop();

    bool isOpen() const { return opened_; }
    bool isRunning() const { return running_.load(std::memory_order_acquire); }
    const FrameBuffer &frameBuffer(CameraId camera_id) const;
    CameraStatistics statistics(CameraId camera_id) const;

private:
    static constexpr std::size_t kCameraCount = 4;
    static constexpr std::size_t kImageBuffersPerCamera = FrameBuffer::kCapacity + 1;
    static constexpr std::size_t kTriggerQueueCapacity = 64;

    class TriggerCycleQueue {
    public:
        bool push(std::uint64_t cycle) noexcept;
        bool pop(std::uint64_t &cycle) noexcept;
        bool cancelLastPush() noexcept;
        void clear() noexcept;

    private:
        std::array<std::uint64_t, kTriggerQueueCapacity> values_{};
        std::atomic<std::uint64_t> head_{0};
        std::atomic<std::uint64_t> tail_{0};
    };

    struct AtomicStatistics {
        std::atomic<std::uint64_t> received_frames{0};
        std::atomic<std::uint64_t> frame_number_drops{0};
        std::atomic<std::uint64_t> grab_timeouts{0};
        std::atomic<std::uint64_t> grab_errors{0};
        std::atomic<std::uint64_t> invalid_frames{0};
        std::atomic<std::uint64_t> buffer_pool_exhausted{0};
        std::atomic<std::uint64_t> trigger_failures{0};
        std::atomic<std::uint64_t> unmatched_trigger_frames{0};
        std::atomic<std::uint64_t> last_frame_number{0};
        std::atomic<std::uint64_t> last_device_timestamp{0};
    };

    struct CameraSlot {
        CameraId camera_id = CameraId::FRONT;
        std::string serial;
        HikCamera camera;
        FrameBuffer frame_buffer;
        TriggerCycleQueue trigger_cycles;
        std::array<std::shared_ptr<std::vector<std::uint8_t>>, kImageBuffersPerCamera> image_pool;
        std::jthread capture_thread;
        AtomicStatistics statistics;
    };

    static std::size_t indexOf(CameraId camera_id);
    static const char *cameraIdName(CameraId camera_id);
    std::shared_ptr<std::vector<std::uint8_t>> acquireImageBuffer(CameraSlot &slot);
    void captureLoop(CameraSlot &slot, std::stop_token stop_token);

    std::array<CameraSlot, kCameraCount> slots_;
    MultiCameraManagerConfig config_{};
    std::atomic<bool> running_{false};
    bool opened_ = false;
};
