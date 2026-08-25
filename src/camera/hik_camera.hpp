#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <MvCameraControl.h>

enum class TriggerMode {
    FREE_RUN,
    SOFTWARE_TRIGGER,
    HARDWARE_TRIGGER,
};

struct HikCameraParameters {
    std::uint32_t width = 1440;
    std::uint32_t height = 1080;
    float frame_rate = 100.0F;
    MvGvspPixelType pixel_format = PixelType_Gvsp_BayerRG8;
    bool exposure_auto = true;
    float exposure_time_us = 10000.0F;
    float auto_exposure_upper_limit_us = 8000.0F;
    std::uint32_t auto_brightness_target = 160;
    bool gain_auto = true;
    float white_balance_red = 1.8F;
    float white_balance_blue = 1.6F;
    bool trigger_cache = true;
    TriggerMode trigger_mode = TriggerMode::SOFTWARE_TRIGGER;
};

struct HikDeviceInfo {
    std::string serial;
    std::string model;
    std::string user_defined_name;
    MV_CC_DEVICE_INFO native_info{};
};

struct HikFrameMetadata {
    std::uint64_t frame_number = 0;
    std::uint64_t trigger_index = 0;
    std::uint64_t device_timestamp = 0;
    std::int64_t sdk_host_timestamp = 0;
    std::uint32_t lost_packets = 0;
    float exposure_time_us = 0.0F;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    MvGvspPixelType pixel_format = PixelType_Gvsp_Undefined;
    std::size_t data_size = 0;
};

enum class HikGrabResult {
    FRAME,
    TIMEOUT,
    ERROR,
};

class HikCamera {
public:
    class FrameLease {
    public:
        FrameLease() = default;
        ~FrameLease();

        FrameLease(const FrameLease &) = delete;
        FrameLease &operator=(const FrameLease &) = delete;
        FrameLease(FrameLease &&other) noexcept;
        FrameLease &operator=(FrameLease &&other) noexcept;

        const unsigned char *data() const { return frame_.pBufAddr; }
        std::size_t size() const { return metadata_.data_size; }
        const HikFrameMetadata &metadata() const { return metadata_; }
        explicit operator bool() const { return owner_ != nullptr; }
        void reset();

    private:
        friend class HikCamera;

        HikCamera *owner_ = nullptr;
        MV_FRAME_OUT frame_{};
        HikFrameMetadata metadata_{};
    };

    HikCamera() = default;
    ~HikCamera();

    HikCamera(const HikCamera &) = delete;
    HikCamera &operator=(const HikCamera &) = delete;

    static bool enumerateUsbDevices(std::vector<HikDeviceInfo> &devices,
                                    std::string *error = nullptr);

    bool openBySerial(const std::string &serial,
                      const HikCameraParameters &parameters,
                      std::string *error = nullptr);
    bool open(const HikDeviceInfo &device,
              const HikCameraParameters &parameters,
              std::string *error = nullptr);
    bool startGrabbing(std::string *error = nullptr);
    HikGrabResult getImageBuffer(FrameLease &frame,
                                 unsigned int timeout_ms,
                                 std::string *error = nullptr);
    bool softwareTrigger(std::string *error = nullptr);
    void close();

    bool isOpen() const { return handle_ != nullptr; }
    bool isGrabbing() const { return grabbing_; }
    const std::string &serial() const { return serial_; }

private:
    void releaseImageBuffer(MV_FRAME_OUT &frame) noexcept;

    void *handle_ = nullptr;
    std::string serial_;
    HikCameraParameters parameters_{};
    bool grabbing_ = false;
};
