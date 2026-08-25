#include "multi_camera_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace {

std::int64_t SteadyTimestampNanoseconds()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

MultiCameraManager::MultiCameraManager()
{
    slots_[0].camera_id = CameraId::FRONT;
    slots_[1].camera_id = CameraId::REAR;
    slots_[2].camera_id = CameraId::LEFT;
    slots_[3].camera_id = CameraId::RIGHT;
}

MultiCameraManager::~MultiCameraManager()
{
    stop();
}

bool MultiCameraManager::TriggerCycleQueue::push(std::uint64_t cycle) noexcept
{
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    if (tail - head >= kTriggerQueueCapacity) {
        return false;
    }
    values_[tail % kTriggerQueueCapacity] = cycle;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
}

bool MultiCameraManager::TriggerCycleQueue::pop(std::uint64_t &cycle) noexcept
{
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    if (head == tail) {
        return false;
    }
    cycle = values_[head % kTriggerQueueCapacity];
    head_.store(head + 1, std::memory_order_release);
    return true;
}

bool MultiCameraManager::TriggerCycleQueue::cancelLastPush() noexcept
{
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    if (tail == head) {
        return false;
    }
    tail_.store(tail - 1, std::memory_order_release);
    return true;
}

void MultiCameraManager::TriggerCycleQueue::clear() noexcept
{
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
}

std::size_t MultiCameraManager::indexOf(CameraId camera_id)
{
    switch (camera_id) {
        case CameraId::FRONT: return 0;
        case CameraId::REAR: return 1;
        case CameraId::LEFT: return 2;
        case CameraId::RIGHT: return 3;
    }
    throw std::out_of_range("invalid CameraId");
}

const char *MultiCameraManager::cameraIdName(CameraId camera_id)
{
    switch (camera_id) {
        case CameraId::FRONT: return "Front";
        case CameraId::REAR: return "Rear";
        case CameraId::LEFT: return "Left";
        case CameraId::RIGHT: return "Right";
    }
    return "Unknown";
}

bool MultiCameraManager::openAll(const MultiCameraManagerConfig &config, std::string *error)
{
    stop();
    config_ = config;

    for (CameraSlot &slot : slots_) {
        slot.frame_buffer.clear();
        slot.trigger_cycles.clear();
        slot.statistics.received_frames.store(0, std::memory_order_relaxed);
        slot.statistics.frame_number_drops.store(0, std::memory_order_relaxed);
        slot.statistics.grab_timeouts.store(0, std::memory_order_relaxed);
        slot.statistics.grab_errors.store(0, std::memory_order_relaxed);
        slot.statistics.invalid_frames.store(0, std::memory_order_relaxed);
        slot.statistics.buffer_pool_exhausted.store(0, std::memory_order_relaxed);
        slot.statistics.trigger_failures.store(0, std::memory_order_relaxed);
        slot.statistics.unmatched_trigger_frames.store(0, std::memory_order_relaxed);
        slot.statistics.last_frame_number.store(0, std::memory_order_relaxed);
        slot.statistics.last_device_timestamp.store(0, std::memory_order_relaxed);
    }

    std::set<std::string> requested_serials;
    std::array<bool, kCameraCount> ids_seen{};
    for (const CameraBinding &binding : config_.cameras) {
        const std::size_t index = indexOf(binding.camera_id);
        if (ids_seen[index]) {
            if (error != nullptr) *error = "duplicate logical camera id";
            return false;
        }
        if (binding.serial.empty() || !requested_serials.insert(binding.serial).second) {
            if (error != nullptr) *error = "camera serial is empty or duplicated: " + binding.serial;
            return false;
        }
        ids_seen[index] = true;
        slots_[index].serial = binding.serial;
    }

    if (std::any_of(ids_seen.begin(), ids_seen.end(), [](bool seen) { return !seen; })) {
        if (error != nullptr) *error = "Front/Rear/Left/Right bindings are incomplete";
        return false;
    }

    std::vector<HikDeviceInfo> devices;
    if (!HikCamera::enumerateUsbDevices(devices, error)) {
        return false;
    }
    for (const HikDeviceInfo &device : devices) {
        spdlog::info("Enumerated USB camera: serial={} model={} user_name={}",
                     device.serial,
                     device.model,
                     device.user_defined_name);
    }

    for (CameraSlot &slot : slots_) {
        const auto match = std::find_if(devices.begin(), devices.end(), [&](const HikDeviceInfo &device) {
            return device.serial == slot.serial;
        });
        if (match == devices.end()) {
            if (error != nullptr) {
                *error = std::string(cameraIdName(slot.camera_id)) +
                         " camera serial not found: " + slot.serial;
            }
            stop();
            return false;
        }
        if (!slot.camera.open(*match, config_.camera_parameters, error)) {
            stop();
            return false;
        }
        spdlog::info("Bound logical camera {} to serial={}", cameraIdName(slot.camera_id), slot.serial);
    }

    const std::size_t image_size =
        static_cast<std::size_t>(config_.camera_parameters.width) * config_.camera_parameters.height;
    for (CameraSlot &slot : slots_) {
        for (auto &buffer : slot.image_pool) {
            buffer = std::make_shared<std::vector<std::uint8_t>>(image_size);
        }
    }

    opened_ = true;
    return true;
}

bool MultiCameraManager::startCapture(std::string *error)
{
    if (!opened_) {
        if (error != nullptr) *error = "cameras are not open";
        return false;
    }
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }

    for (CameraSlot &slot : slots_) {
        if (!slot.camera.startGrabbing(error)) {
            stop();
            return false;
        }
    }

    running_.store(true, std::memory_order_release);
    for (CameraSlot &slot : slots_) {
        slot.capture_thread = std::jthread([this, &slot](std::stop_token token) {
            captureLoop(slot, token);
        });
    }
    return true;
}

bool MultiCameraManager::softwareTriggerAll(std::uint64_t trigger_cycle)
{
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    bool all_succeeded = true;
    for (CameraSlot &slot : slots_) {
        if (!slot.trigger_cycles.push(trigger_cycle)) {
            slot.statistics.trigger_failures.fetch_add(1, std::memory_order_relaxed);
            all_succeeded = false;
            continue;
        }

        std::string error;
        if (!slot.camera.softwareTrigger(&error)) {
            slot.trigger_cycles.cancelLastPush();
            slot.statistics.trigger_failures.fetch_add(1, std::memory_order_relaxed);
            spdlog::error("Software trigger failed for {} serial={}: {}",
                          cameraIdName(slot.camera_id),
                          slot.serial,
                          error);
            all_succeeded = false;
        }

    }
    return all_succeeded;
}

std::shared_ptr<std::vector<std::uint8_t>> MultiCameraManager::acquireImageBuffer(CameraSlot &slot)
{
    for (const auto &buffer : slot.image_pool) {
        if (buffer.use_count() == 1) {
            return buffer;
        }
    }
    return {};
}

void MultiCameraManager::captureLoop(CameraSlot &slot, std::stop_token stop_token)
{
    std::uint64_t previous_frame_number = 0;
    std::uint64_t invalid_frames_logged = 0;
    while (!stop_token.stop_requested()) {
        HikCamera::FrameLease lease;
        std::string error;
        const HikGrabResult result =
            slot.camera.getImageBuffer(lease, config_.grab_timeout_ms, &error);
        if (result == HikGrabResult::TIMEOUT) {
            slot.statistics.grab_timeouts.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (result == HikGrabResult::ERROR) {
            slot.statistics.grab_errors.fetch_add(1, std::memory_order_relaxed);
            spdlog::error("Grab failed for {} serial={}: {}",
                          cameraIdName(slot.camera_id),
                          slot.serial,
                          error);
            continue;
        }

        const HikFrameMetadata metadata = lease.metadata();
        const std::int64_t host_timestamp = SteadyTimestampNanoseconds();
        const std::size_t expected_size =
            static_cast<std::size_t>(metadata.width) * metadata.height;

        std::uint64_t dropped = 0;
        if (previous_frame_number != 0 && metadata.frame_number > previous_frame_number + 1) {
            dropped = metadata.frame_number - previous_frame_number - 1;
            slot.statistics.frame_number_drops.fetch_add(dropped, std::memory_order_relaxed);
        }
        previous_frame_number = metadata.frame_number;

        std::uint64_t discarded_cycle = 0;
        for (std::uint64_t index = 0; index < dropped; ++index) {
            if (!slot.trigger_cycles.pop(discarded_cycle)) {
                break;
            }
        }

        std::uint64_t trigger_cycle = 0;
        if (config_.camera_parameters.trigger_mode == TriggerMode::SOFTWARE_TRIGGER &&
            !slot.trigger_cycles.pop(trigger_cycle)) {
            slot.statistics.unmatched_trigger_frames.fetch_add(1, std::memory_order_relaxed);
        }

        if (metadata.width != config_.camera_parameters.width ||
            metadata.height != config_.camera_parameters.height ||
            metadata.pixel_format != PixelType_Gvsp_BayerRG8 ||
            metadata.data_size < expected_size ||
            lease.data() == nullptr) {
            slot.statistics.invalid_frames.fetch_add(1, std::memory_order_relaxed);
            if (invalid_frames_logged < 5) {
                ++invalid_frames_logged;
                spdlog::warn(
                    "Invalid frame for {} serial={}: frame_no={} size={}x{} expected={}x{} "
                    "pixel_format={} expected_pixel_format={} data_size={} expected_data_size={} "
                    "lost_packets={} trigger_index={} exposure_us={:.1f} data_null={}",
                    cameraIdName(slot.camera_id),
                    slot.serial,
                    metadata.frame_number,
                    metadata.width,
                    metadata.height,
                    config_.camera_parameters.width,
                    config_.camera_parameters.height,
                    static_cast<std::uint32_t>(metadata.pixel_format),
                    static_cast<std::uint32_t>(PixelType_Gvsp_BayerRG8),
                    metadata.data_size,
                    expected_size,
                    metadata.lost_packets,
                    metadata.trigger_index,
                    metadata.exposure_time_us,
                    lease.data() == nullptr);
            }
            continue;
        }

        auto image = acquireImageBuffer(slot);
        if (image == nullptr) {
            slot.statistics.buffer_pool_exhausted.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        std::memcpy(image->data(), lease.data(), expected_size);

        auto packet = std::make_shared<FramePacket>();
        packet->camera_id = slot.camera_id;
        packet->frame_number = metadata.frame_number;
        packet->trigger_cycle = trigger_cycle;
        packet->device_timestamp = metadata.device_timestamp;
        packet->host_timestamp = host_timestamp;
        packet->width = metadata.width;
        packet->height = metadata.height;
        packet->pixel_format = static_cast<std::uint32_t>(metadata.pixel_format);
        packet->image_data = std::move(image);
        slot.frame_buffer.push(std::move(packet));

        slot.statistics.received_frames.fetch_add(1, std::memory_order_relaxed);
        slot.statistics.last_frame_number.store(metadata.frame_number, std::memory_order_relaxed);
        slot.statistics.last_device_timestamp.store(metadata.device_timestamp, std::memory_order_relaxed);
    }
}

void MultiCameraManager::stop()
{
    running_.store(false, std::memory_order_release);
    for (CameraSlot &slot : slots_) {
        if (slot.capture_thread.joinable()) {
            slot.capture_thread.request_stop();
        }
    }
    for (CameraSlot &slot : slots_) {
        if (slot.capture_thread.joinable()) {
            slot.capture_thread.join();
        }
    }
    for (CameraSlot &slot : slots_) {
        slot.camera.close();
        for (auto &buffer : slot.image_pool) {
            buffer.reset();
        }
    }
    opened_ = false;
}

const FrameBuffer &MultiCameraManager::frameBuffer(CameraId camera_id) const
{
    return slots_[indexOf(camera_id)].frame_buffer;
}

CameraStatistics MultiCameraManager::statistics(CameraId camera_id) const
{
    const CameraSlot &slot = slots_[indexOf(camera_id)];
    CameraStatistics result;
    result.received_frames = slot.statistics.received_frames.load(std::memory_order_relaxed);
    result.frame_number_drops = slot.statistics.frame_number_drops.load(std::memory_order_relaxed);
    result.grab_timeouts = slot.statistics.grab_timeouts.load(std::memory_order_relaxed);
    result.grab_errors = slot.statistics.grab_errors.load(std::memory_order_relaxed);
    result.invalid_frames = slot.statistics.invalid_frames.load(std::memory_order_relaxed);
    result.buffer_overwrites = slot.frame_buffer.overwrittenCount();
    result.buffer_pool_exhausted = slot.statistics.buffer_pool_exhausted.load(std::memory_order_relaxed);
    result.trigger_failures = slot.statistics.trigger_failures.load(std::memory_order_relaxed);
    result.unmatched_trigger_frames = slot.statistics.unmatched_trigger_frames.load(std::memory_order_relaxed);
    result.last_frame_number = slot.statistics.last_frame_number.load(std::memory_order_relaxed);
    result.last_device_timestamp = slot.statistics.last_device_timestamp.load(std::memory_order_relaxed);
    return result;
}
