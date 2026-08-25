#include "hik_camera.hpp"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>

#include <spdlog/spdlog.h>

#include <MvObsoleteInterfaces.h>

namespace {

std::string HexRet(int ret)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << static_cast<unsigned int>(ret);
    return stream.str();
}

std::string UsbString(const unsigned char *value, std::size_t capacity)
{
    if (value == nullptr || capacity == 0) {
        return {};
    }
    const auto *begin = reinterpret_cast<const char *>(value);
    const void *terminator = std::memchr(begin, '\0', capacity);
    const auto length = terminator == nullptr
                            ? capacity
                            : static_cast<std::size_t>(static_cast<const char *>(terminator) - begin);
    return std::string(begin, length);
}

bool SetEnum(void *handle, const char *name, unsigned int value, std::string *error)
{
    const int ret = MV_CC_SetEnumValue(handle, name, value);
    if (ret == MV_OK) {
        return true;
    }
    if (error != nullptr) {
        *error = std::string("set ") + name + " failed, ret=" + HexRet(ret);
    }
    return false;
}

bool SetInt(void *handle, const char *name, unsigned int value, std::string *error)
{
    const int ret = MV_CC_SetIntValue(handle, name, value);
    if (ret == MV_OK) {
        return true;
    }
    if (error != nullptr) {
        *error = std::string("set ") + name + " failed, ret=" + HexRet(ret);
    }
    return false;
}

bool SetFloat(void *handle, const char *name, float value, std::string *error)
{
    const int ret = MV_CC_SetFloatValue(handle, name, value);
    if (ret == MV_OK) {
        return true;
    }
    if (error != nullptr) {
        *error = std::string("set ") + name + " failed, ret=" + HexRet(ret);
    }
    return false;
}

bool SetBool(void *handle, const char *name, bool value, std::string *error)
{
    const int ret = MV_CC_SetBoolValue(handle, name, value);
    if (ret == MV_OK) {
        return true;
    }
    if (error != nullptr) {
        *error = std::string("set ") + name + " failed, ret=" + HexRet(ret);
    }
    return false;
}

bool ConfigureTrigger(void *handle, TriggerMode mode, std::string *error)
{
    if (mode == TriggerMode::FREE_RUN) {
        return SetEnum(handle, "TriggerMode", 0, error);
    }

    if (!SetEnum(handle, "TriggerMode", 1, error)) {
        return false;
    }
    const unsigned int source = mode == TriggerMode::SOFTWARE_TRIGGER ? 7U : 0U;
    return SetEnum(handle, "TriggerSource", source, error);
}

bool ConfigureCamera(void *handle, const HikCameraParameters &parameters, std::string *error)
{
    if (!SetEnum(handle, "PixelFormat", static_cast<unsigned int>(parameters.pixel_format), error) ||
        !SetInt(handle, "Width", parameters.width, error) ||
        !SetInt(handle, "Height", parameters.height, error) ||
        !SetBool(handle, "AcquisitionFrameRateEnable", true, error) ||
        !SetFloat(handle, "AcquisitionFrameRate", parameters.frame_rate, error) ||
        !SetEnum(handle, "ExposureAuto", parameters.exposure_auto ? 2U : 0U, error)) {
        return false;
    }

    if (parameters.exposure_auto) {
        const int ret = MV_CC_SetAutoExposureTimeUpper(
            handle, static_cast<unsigned int>(parameters.auto_exposure_upper_limit_us));
        if (ret != MV_OK) {
            if (error != nullptr) {
                *error = "set auto exposure upper limit failed, ret=" + HexRet(ret);
            }
            return false;
        }
    } else if (!SetFloat(handle, "ExposureTime", parameters.exposure_time_us, error)) {
        return false;
    }

    if (!SetEnum(handle, "GainAuto", parameters.gain_auto ? 2U : 0U, error)) {
        return false;
    }
    if (!parameters.gain_auto) {
        MVCC_FLOATVALUE gain{};
        const int ret = MV_CC_GetFloatValue(handle, "Gain", &gain);
        if (ret != MV_OK) {
            if (error != nullptr) {
                *error = "read Gain range failed, ret=" + HexRet(ret);
            }
            return false;
        }
        if (!SetFloat(handle, "Gain", gain.fMax, error)) {
            return false;
        }
    }

    if (parameters.exposure_auto || parameters.gain_auto) {
        const int ret = MV_CC_SetBrightness(handle, parameters.auto_brightness_target);
        if (ret != MV_OK) {
            if (error != nullptr) {
                *error = "set auto brightness target failed, ret=" + HexRet(ret);
            }
            return false;
        }
    }

    if (!SetEnum(handle, "BalanceWhiteAuto", 0, error) ||
        !SetEnum(handle, "BalanceRatioSelector", 0, error) ||
        !SetInt(handle, "BalanceRatio", static_cast<unsigned int>(parameters.white_balance_red * 1000.0F), error) ||
        !SetEnum(handle, "BalanceRatioSelector", 2, error) ||
        !SetInt(handle, "BalanceRatio", static_cast<unsigned int>(parameters.white_balance_blue * 1000.0F), error) ||
        !ConfigureTrigger(handle, parameters.trigger_mode, error) ||
        (parameters.trigger_mode != TriggerMode::FREE_RUN &&
         !SetBool(handle, "TriggerCacheEnable", parameters.trigger_cache, error))) {
        return false;
    }

    MVCC_ENUMVALUE pixel_format{};
    int ret = MV_CC_GetEnumValue(handle, "PixelFormat", &pixel_format);
    if (ret != MV_OK || pixel_format.nCurValue != static_cast<unsigned int>(parameters.pixel_format)) {
        if (error != nullptr) {
            *error = "PixelFormat readback mismatch, requested BayerRG8";
        }
        return false;
    }

    MVCC_INTVALUE width{};
    MVCC_INTVALUE height{};
    ret = MV_CC_GetIntValue(handle, "Width", &width);
    if (ret != MV_OK) {
        if (error != nullptr) *error = "read Width failed, ret=" + HexRet(ret);
        return false;
    }
    ret = MV_CC_GetIntValue(handle, "Height", &height);
    if (ret != MV_OK) {
        if (error != nullptr) *error = "read Height failed, ret=" + HexRet(ret);
        return false;
    }
    if (width.nCurValue != parameters.width || height.nCurValue != parameters.height) {
        if (error != nullptr) {
            *error = "image size readback mismatch, actual=" + std::to_string(width.nCurValue) +
                     "x" + std::to_string(height.nCurValue);
        }
        return false;
    }

    return true;
}

void LogTransportReadback(void *handle, const std::string &serial)
{
    unsigned int transfer_size = 0;
    unsigned int transfer_ways = 0;
    const int size_ret = MV_USB_GetTransferSize(handle, &transfer_size);
    const int ways_ret = MV_USB_GetTransferWays(handle, &transfer_ways);

    MVCC_INTVALUE payload{};
    MVCC_INTVALUE throughput{};
    MVCC_INTVALUE exposure_upper{};
    MVCC_INTVALUE brightness{};
    MVCC_FLOATVALUE exposure{};
    MVCC_FLOATVALUE gain{};
    MVCC_FLOATVALUE acquisition_rate{};
    MVCC_FLOATVALUE resulting_rate{};
    const int payload_ret = MV_CC_GetIntValue(handle, "PayloadSize", &payload);
    const int throughput_ret =
        MV_CC_GetIntValue(handle, "DeviceLinkThroughputLimit", &throughput);
    const int exposure_ret = MV_CC_GetFloatValue(handle, "ExposureTime", &exposure);
    const int exposure_upper_ret =
        MV_CC_GetAutoExposureTimeUpper(handle, &exposure_upper);
    const int brightness_ret = MV_CC_GetBrightness(handle, &brightness);
    const int gain_ret = MV_CC_GetFloatValue(handle, "Gain", &gain);
    const int acquisition_rate_ret =
        MV_CC_GetFloatValue(handle, "AcquisitionFrameRate", &acquisition_rate);
    const int resulting_rate_ret =
        MV_CC_GetFloatValue(handle, "ResultingFrameRate", &resulting_rate);
    spdlog::info(
        "Camera transport serial={} payload={} throughput_limit={} exposure_us={:.1f} "
        "exposure_upper_us={} brightness={} brightness_range=[{},{}] "
        "gain={:.2f} gain_range=[{:.2f},{:.2f}] "
        "acquisition_fps={:.2f} resulting_fps={:.2f} "
        "usb_transfer_size={} usb_transfer_ways={} "
        "readback_ret=[{},{},{},{},{},{},{},{},{},{}]",
        serial,
        payload_ret == MV_OK ? payload.nCurValue : 0U,
        throughput_ret == MV_OK ? throughput.nCurValue : 0U,
        exposure_ret == MV_OK ? exposure.fCurValue : 0.0F,
        exposure_upper_ret == MV_OK ? exposure_upper.nCurValue : 0U,
        brightness_ret == MV_OK ? brightness.nCurValue : 0U,
        brightness_ret == MV_OK ? brightness.nMin : 0U,
        brightness_ret == MV_OK ? brightness.nMax : 0U,
        gain_ret == MV_OK ? gain.fCurValue : 0.0F,
        gain_ret == MV_OK ? gain.fMin : 0.0F,
        gain_ret == MV_OK ? gain.fMax : 0.0F,
        acquisition_rate_ret == MV_OK ? acquisition_rate.fCurValue : 0.0F,
        resulting_rate_ret == MV_OK ? resulting_rate.fCurValue : 0.0F,
        size_ret == MV_OK ? transfer_size : 0U,
        ways_ret == MV_OK ? transfer_ways : 0U,
        HexRet(payload_ret),
        HexRet(throughput_ret),
        HexRet(exposure_ret),
        HexRet(exposure_upper_ret),
        HexRet(brightness_ret),
        HexRet(gain_ret),
        HexRet(acquisition_rate_ret),
        HexRet(resulting_rate_ret),
        HexRet(size_ret),
        HexRet(ways_ret));
}

}  // namespace

HikCamera::FrameLease::~FrameLease()
{
    reset();
}

HikCamera::FrameLease::FrameLease(FrameLease &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      frame_(other.frame_),
      metadata_(other.metadata_)
{
    other.frame_ = {};
    other.metadata_ = {};
}

HikCamera::FrameLease &HikCamera::FrameLease::operator=(FrameLease &&other) noexcept
{
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        frame_ = other.frame_;
        metadata_ = other.metadata_;
        other.frame_ = {};
        other.metadata_ = {};
    }
    return *this;
}

void HikCamera::FrameLease::reset()
{
    if (owner_ != nullptr) {
        owner_->releaseImageBuffer(frame_);
        owner_ = nullptr;
    }
    frame_ = {};
    metadata_ = {};
}

HikCamera::~HikCamera()
{
    close();
}

bool HikCamera::enumerateUsbDevices(std::vector<HikDeviceInfo> &devices, std::string *error)
{
    devices.clear();
    MV_CC_DEVICE_INFO_LIST list{};
    // EnumDevices() also probes virtual USB transports. The ARM64 Runtime SDK
    // intentionally omits that optional producer, so enumerate physical USB
    // cameras only. Sorting improves logs but is never used for camera binding.
    const int ret = MV_CC_EnumDevicesEx2(
        MV_USB_DEVICE, &list, nullptr, SortMethod_SerialNumber);
    if (ret != MV_OK) {
        if (error != nullptr) *error = "enumerate USB cameras failed, ret=" + HexRet(ret);
        return false;
    }

    devices.reserve(list.nDeviceNum);
    for (unsigned int index = 0; index < list.nDeviceNum; ++index) {
        const MV_CC_DEVICE_INFO *native = list.pDeviceInfo[index];
        if (native == nullptr || (native->nTLayerType & MV_USB_DEVICE) == 0) {
            continue;
        }

        HikDeviceInfo device;
        device.native_info = *native;
        const auto &usb = native->SpecialInfo.stUsb3VInfo;
        device.serial = UsbString(usb.chSerialNumber, sizeof(usb.chSerialNumber));
        device.model = UsbString(usb.chModelName, sizeof(usb.chModelName));
        device.user_defined_name = UsbString(usb.chUserDefinedName, sizeof(usb.chUserDefinedName));
        devices.push_back(std::move(device));
    }
    return true;
}

bool HikCamera::openBySerial(const std::string &serial,
                             const HikCameraParameters &parameters,
                             std::string *error)
{
    std::vector<HikDeviceInfo> devices;
    if (!enumerateUsbDevices(devices, error)) {
        return false;
    }

    const auto match = std::find_if(devices.begin(), devices.end(), [&](const HikDeviceInfo &device) {
        return device.serial == serial;
    });
    if (match == devices.end()) {
        if (error != nullptr) *error = "USB camera serial not found: " + serial;
        return false;
    }
    return open(*match, parameters, error);
}

bool HikCamera::open(const HikDeviceInfo &device,
                     const HikCameraParameters &parameters,
                     std::string *error)
{
    close();
    if (device.serial.empty()) {
        if (error != nullptr) *error = "camera serial is empty";
        return false;
    }

    MV_CC_DEVICE_INFO native = device.native_info;
    int ret = MV_CC_CreateHandle(&handle_, &native);
    if (ret != MV_OK || handle_ == nullptr) {
        handle_ = nullptr;
        if (error != nullptr) *error = "create handle failed for serial " + device.serial + ", ret=" + HexRet(ret);
        return false;
    }

    ret = MV_CC_OpenDevice(handle_);
    if (ret != MV_OK) {
        if (error != nullptr) *error = "open camera failed for serial " + device.serial + ", ret=" + HexRet(ret);
        close();
        return false;
    }

    serial_ = device.serial;
    parameters_ = parameters;
    if (!ConfigureCamera(handle_, parameters_, error)) {
        close();
        return false;
    }
    LogTransportReadback(handle_, serial_);

    ret = MV_CC_SetGrabStrategy(handle_, MV_GrabStrategy_LatestImagesOnly);
    if (ret != MV_OK) {
        if (error != nullptr) *error = "set latest-image grab strategy failed, ret=" + HexRet(ret);
        close();
        return false;
    }

    spdlog::info("Opened camera serial={} model={} size={}x{} pixel_format=BayerRG8 trigger_mode={}",
                 serial_,
                 device.model,
                 parameters_.width,
                 parameters_.height,
                 static_cast<int>(parameters_.trigger_mode));
    return true;
}

bool HikCamera::startGrabbing(std::string *error)
{
    if (handle_ == nullptr) {
        if (error != nullptr) *error = "camera is not open";
        return false;
    }
    if (grabbing_) {
        return true;
    }

    const int ret = MV_CC_StartGrabbing(handle_);
    if (ret != MV_OK) {
        if (error != nullptr) *error = "start grabbing failed, ret=" + HexRet(ret);
        return false;
    }
    grabbing_ = true;
    return true;
}

HikGrabResult HikCamera::getImageBuffer(FrameLease &frame,
                                        unsigned int timeout_ms,
                                        std::string *error)
{
    frame.reset();
    if (handle_ == nullptr || !grabbing_) {
        if (error != nullptr) *error = "camera is not grabbing";
        return HikGrabResult::ERROR;
    }

    MV_FRAME_OUT native{};
    const int ret = MV_CC_GetImageBuffer(handle_, &native, timeout_ms);
    if (static_cast<unsigned int>(ret) == MV_E_NODATA) {
        return HikGrabResult::TIMEOUT;
    }
    if (ret != MV_OK) {
        if (error != nullptr) *error = "GetImageBuffer failed, ret=" + HexRet(ret);
        return HikGrabResult::ERROR;
    }

    frame.owner_ = this;
    frame.frame_ = native;
    frame.metadata_.frame_number = native.stFrameInfo.nFrameNum;
    frame.metadata_.trigger_index = native.stFrameInfo.nTriggerIndex;
    frame.metadata_.device_timestamp =
        (static_cast<std::uint64_t>(native.stFrameInfo.nDevTimeStampHigh) << 32U) |
        native.stFrameInfo.nDevTimeStampLow;
    frame.metadata_.sdk_host_timestamp = native.stFrameInfo.nHostTimeStamp;
    frame.metadata_.lost_packets = native.stFrameInfo.nLostPacket;
    frame.metadata_.exposure_time_us = native.stFrameInfo.fExposureTime;
    frame.metadata_.width = native.stFrameInfo.nWidth;
    frame.metadata_.height = native.stFrameInfo.nHeight;
    frame.metadata_.pixel_format = native.stFrameInfo.enPixelType;
    frame.metadata_.data_size = native.stFrameInfo.nFrameLen;
    return HikGrabResult::FRAME;
}

bool HikCamera::softwareTrigger(std::string *error)
{
    if (handle_ == nullptr || !grabbing_) {
        if (error != nullptr) *error = "camera is not grabbing";
        return false;
    }
    if (parameters_.trigger_mode != TriggerMode::SOFTWARE_TRIGGER) {
        if (error != nullptr) *error = "camera is not configured for software trigger";
        return false;
    }

    const int ret = MV_CC_SetCommandValue(handle_, "TriggerSoftware");
    if (ret != MV_OK) {
        if (error != nullptr) *error = "software trigger failed, ret=" + HexRet(ret);
        return false;
    }
    return true;
}

void HikCamera::releaseImageBuffer(MV_FRAME_OUT &frame) noexcept
{
    if (handle_ != nullptr && frame.pBufAddr != nullptr) {
        const int ret = MV_CC_FreeImageBuffer(handle_, &frame);
        if (ret != MV_OK) {
            spdlog::error("FreeImageBuffer failed for serial={}, ret={}", serial_, HexRet(ret));
        }
    }
    frame = {};
}

void HikCamera::close()
{
    if (handle_ != nullptr) {
        if (grabbing_) {
            MV_CC_StopGrabbing(handle_);
            grabbing_ = false;
        }
        MV_CC_CloseDevice(handle_);
        MV_CC_DestroyHandle(handle_);
        handle_ = nullptr;
    }
    serial_.clear();
}
