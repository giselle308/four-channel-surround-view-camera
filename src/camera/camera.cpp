/*
 * 海康工业相机启动与取帧模块
 *
 * 讲解时可以按以下主线理解本文件：
 *   1. 从 YAML 读取相机参数；
 *   2. 通过海康 MVS SDK 枚举、打开并配置相机；
 *   3. 启动取流，把 SDK 返回的各种像素格式统一转成 BGR；
 *   4. 上层可以通过 HikCameraNode 获取图像，也可以单独运行本文件预览画面；
 *   5. 退出时按 SDK 要求停止取流并释放设备资源。
 *
 * 数据流：相机传感器 -> MVS SDK 原始帧 -> OpenCV cv::Mat(BGR) -> 检测/显示模块。
 */

#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

#include <MvCameraControl.h>
#include <opencv2/opencv.hpp>
#include <yaml-cpp/yaml.h>

#include "camera_node.hpp"
#include "memory_layout.hpp"
#include "logging.hpp"

#ifndef CAMERA_CONFIG_PATH
// CMake 可以在编译时传入绝对配置路径；未传入时使用此相对路径。
#define CAMERA_CONFIG_PATH "../config/config.yaml"
#endif

/*
 * CameraConfig 是 YAML 配置在 C++ 中的对应结构。
 * 将外部配置先集中读入该结构，后面的 init() 就只需关心如何将参数写入相机。
 * 成员初始值主要用来表达预期的基准配置；LoadCameraConfig()
 * 会从 YAML 覆盖这些值，其中 pixel_format 是可选项。
 */
struct CameraConfig {
    std::string interface = "USB3Vision";      // 接口类型；当前实现实际固定枚举 USB 设备。
    int width = 1440;                           // 期望的图像宽度，必须满足相机支持的取值范围和步进。
    int height = 1080;                          // 期望的图像高度。
    std::string pixel_format = "BayerRG8";     // 相机输出格式；Bayer 数据量小，但需要后续去马赛克。
    int frame_rate = 0;                         // 目标帧率，仅在 frame_rate_enable=true 时写入相机。
    bool frame_rate_enable = false;             // 是否由程序限制相机采集帧率。
    std::string trigger_mode = "off";           // off 表示连续采集；其他值表示触发采集。
    std::string trigger_source = "software";    // 触发源：software 为软触发，否则映射为外部线路触发。
    float exposure_time = 2000.0f;              // 手动曝光时间，MVS 节点 ExposureTime 的单位通常为微秒。
    bool exposure_auto = false;                 // true 使用相机自动曝光，false 使用 exposure_time。
    float gain = 0.0f;                          // 手动增益；提高可增亮画面，也会放大噪声。
    bool gain_auto = false;                     // true 由相机自动调节增益。
    std::string white_balance_mode = "manual"; // manual 使用下面的红、蓝通道比例。
    float white_balance_red = 1.0f;             // 红通道白平衡系数。
    float white_balance_blue = 1.0f;            // 蓝通道白平衡系数。
};

/*
 * 将字符串转为小写。
 * YAML 中可能写成 BGR8、bgr8 或 Bgr8，先归一化可以避免大小写导致匹配失败。
 * static 表示该工具函数只在本编译单元内可见，不对其他文件暴露。
 */
static std::string ToLower(std::string value) 
{
    for (char &ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

/*
 * 将 YAML 中便于人阅读的像素格式名称，转换为海康 SDK 要求的整数枚举。
 * BGR/RGB 每个像素已有三个颜色通道；Mono 是灰度图；Bayer 每个像素只保留一个颜色分量。
 * HB 是 SDK 定义的特定编码/封装格式。遇到未识别的名称时回退为 BayerRG8。
 */
static unsigned int GetPixelFormatEnum(const std::string &fmt) 
{
    const std::string f = ToLower(fmt);
    if (f == "bgr8" || f == "bgr8_packed" || f == "bgr 8" || f == "brg8") return PixelType_Gvsp_BGR8_Packed;
    if (f == "rgb8" || f == "rgb8_packed") return PixelType_Gvsp_RGB8_Packed;
    if (f == "mono8") return PixelType_Gvsp_Mono8;
    if (f == "bayerbg8") return PixelType_Gvsp_BayerBG8;
    if (f == "bayergb8") return PixelType_Gvsp_BayerGB8;
    if (f == "bayergr8") return PixelType_Gvsp_BayerGR8;
    if (f == "bayerrg8") return PixelType_Gvsp_BayerRG8;
    if (f == "hb_bgr8" || f == "hb_bgr8_packed") return PixelType_Gvsp_HB_BGR8_Packed;
    if (f == "hb_bayerbg8") return PixelType_Gvsp_HB_BayerBG8;
    if (f == "hb_bayergb8") return PixelType_Gvsp_HB_BayerGB8;
    if (f == "hb_bayergr8") return PixelType_Gvsp_HB_BayerGR8;
    if (f == "hb_bayerrg8") return PixelType_Gvsp_HB_BayerRG8;
    return PixelType_Gvsp_BayerRG8;
}

/*
 * GetPixelFormatEnum() 的反向映射：把 SDK 枚举转回可读文本。
 * 这些文本用在日志和错误信息中，让调试时不必只面对一个整数枚举值。
 */
static const char *PixelFormatName(unsigned int fmt)
{
    switch (fmt) {
        case PixelType_Gvsp_BayerBG8: return "BayerBG8";
        case PixelType_Gvsp_BayerGB8: return "BayerGB8";
        case PixelType_Gvsp_BayerGR8: return "BayerGR8";
        case PixelType_Gvsp_BayerRG8: return "BayerRG8";
        case PixelType_Gvsp_BGR8_Packed: return "BGR8_Packed";
        case PixelType_Gvsp_RGB8_Packed: return "RGB8_Packed";
        case PixelType_Gvsp_Mono8: return "Mono8";
        case PixelType_Gvsp_HB_BayerBG8: return "HB_BayerBG8";
        case PixelType_Gvsp_HB_BayerGB8: return "HB_BayerGB8";
        case PixelType_Gvsp_HB_BayerGR8: return "HB_BayerGR8";
        case PixelType_Gvsp_HB_BayerRG8: return "HB_BayerRG8";
        case PixelType_Gvsp_HB_BGR8_Packed: return "HB_BGR8_Packed";
        default: return "Unknown";
    }
}

/*
 * 判断当前帧是否为普通 8 位 Bayer 图像。
 * BG/GB/GR/RG 表示传感器左上角开始的颜色排列，排列不同时必须选择对应的转换码。
 */
static bool IsPlainBayer8(MvGvspPixelType fmt)
{
    return fmt == PixelType_Gvsp_BayerBG8 ||
           fmt == PixelType_Gvsp_BayerGB8 ||
           fmt == PixelType_Gvsp_BayerGR8 ||
           fmt == PixelType_Gvsp_BayerRG8;
}

static int OpenCvBayerToBgrCode(MvGvspPixelType fmt)
{
    /*
     * 根据相机报告的 Bayer 排列选择 OpenCV 去马赛克转换码。
     * 需特别注意：OpenCV 常量的命名按“等价 RGB 顺序”表示，
     * 因此相机报告 BayerRG 时，为了得到 BGR 输出反而要使用 COLOR_BayerBG2BGR；
     * BayerBG 与 BayerRG、BayerGB 与 BayerGR 也要成对交换。
     */
    switch (fmt) {
        case PixelType_Gvsp_BayerBG8: return cv::COLOR_BayerRG2BGR;
        case PixelType_Gvsp_BayerGB8: return cv::COLOR_BayerGR2BGR;
        case PixelType_Gvsp_BayerGR8: return cv::COLOR_BayerGB2BGR;
        case PixelType_Gvsp_BayerRG8: return cv::COLOR_BayerBG2BGR;
        default: return -1;
    }
}

static std::string HexRet(int ret)
{
    // SDK 错误码通常用十六进制查阅手册，这里统一格式化为 0xXXXXXXXX。
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << ret;
    return oss.str();
}

static std::vector<std::string> PixelFormatStringCandidates(const std::string &fmt)
{
    /*
     * 不同相机型号或 SDK 版本对同一格式可能使用不同字符串，
     * 例如 BGR8Packed、BGR8_Packed 和 BGR8 可能表示同一类数据。
     * 该函数列出候选别名，供后面的兼容性回退逻辑逐个尝试。
     */
    const std::string f = ToLower(fmt);
    if (f == "bgr8" || f == "bgr8_packed" || f == "bgr 8" || f == "brg8") {
        return {"BGR8Packed", "BGR8", "BGR8_Packed", "BGR 8"};
    }
    if (f == "rgb8" || f == "rgb8_packed" || f == "rgb 8") {
        return {"RGB8Packed", "RGB8", "RGB8_Packed", "RGB 8"};
    }
    if (f == "mono8" || f == "mono 8") {
        return {"Mono8", "Mono 8"};
    }
    if (f == "bayerrg8") return {"BayerRG8"};
    if (f == "bayerbg8") return {"BayerBG8"};
    if (f == "bayergb8") return {"BayerGB8"};
    if (f == "bayergr8") return {"BayerGR8"};
    return {fmt};
}

/*
 * 读取相机 PixelFormat 节点的当前值与支持列表，并组装成一行诊断文本。
 * MVCC_ENUMVALUE 同时包含当前枚举值、支持数量和支持值数组。
 * 日志最多列出 8 项，是为了保持输出简洁，不代表相机最多只支持 8 种。
 */
static std::string DescribePixelFormatSupport(void *handle)
{
    MVCC_ENUMVALUE enum_value{};
    const int ret = MV_CC_GetEnumValue(handle, "PixelFormat", &enum_value);
    if (ret != MV_OK) {
        return "unavailable";
    }

    std::string desc = "current=" + std::string(PixelFormatName(enum_value.nCurValue)) +
                       "(" + std::to_string(enum_value.nCurValue) + ")";
    if (enum_value.nSupportedNum > 0) {
        desc += " supported=";
        const unsigned int limit = std::min(enum_value.nSupportedNum, 8U);
        for (unsigned int i = 0; i < limit; ++i) {
            if (i > 0) {
                desc += ",";
            }
            const unsigned int value = enum_value.nSupportValue[i];
            desc += std::string(PixelFormatName(value)) + "(" + std::to_string(value) + ")";
        }
        if (enum_value.nSupportedNum > limit) {
            desc += ",...";
        }
    }
    return desc;
}

/*
 * 尽可能将相机切换到配置指定的像素格式。
 * 第一层：使用 MV_CC_SetEnumValue() 直接写入整数枚举，效率高且明确。
 * 第二层：如果枚举失败，用 MV_CC_SetEnumValueByString() 逐个尝试别名。
 * 所有方式都失败时返回 false，调用者会保留相机原有格式继续运行。
 */
static bool SetPixelFormatWithFallbacks(void *handle, const std::string &fmt)
{
    const unsigned int requested = GetPixelFormatEnum(fmt);
    int ret = MV_CC_SetEnumValue(handle, "PixelFormat", requested);
    if (ret == MV_OK) {
        return true;
    }

    spdlog::warn("Set PixelFormat={} by enum {} failed ret={}. {}",
                 fmt,
                 requested,
                 HexRet(ret),
                 DescribePixelFormatSupport(handle));

    for (const std::string &candidate : PixelFormatStringCandidates(fmt)) {
        ret = MV_CC_SetEnumValueByString(handle, "PixelFormat", candidate.c_str());
        if (ret == MV_OK) {
            spdlog::info("Set PixelFormat={} by string '{}' succeeded. {}",
                         fmt,
                         candidate,
                         DescribePixelFormatSupport(handle));
            return true;
        }
        spdlog::warn("Set PixelFormat={} by string '{}' failed ret={}.",
                     fmt,
                     candidate,
                     HexRet(ret));
    }
    return false;
}

/*
 * 定位并加载 YAML 配置文件。
 * 程序可能从 build 目录、工作区根目录或安装目录启动，当前工作目录不一定相同。
 * 因此按顺序尝试：编译期路径 -> build 旁的 config -> 工作区路径 -> 当前目录路径。
 * 找到后通过 usedPath 返回实际路径，方便启动日志明确显示使用了哪份配置。
 * 如果全部路径都不存在，抛出异常交由 LoadCameraConfig() 统一转成错误信息。
 */
static YAML::Node LoadConfigWithFallback(std::string &usedPath) {
    const std::vector<std::string> candidates = {
        CAMERA_CONFIG_PATH,
        "../config/config.yaml",
        "2027rm_ws/config/config.yaml",
        "config/config.yaml"
    };

    for (const auto &path : candidates) {
        std::ifstream fin(path);
        if (fin.good()) {
            usedPath = path;
            return YAML::LoadFile(path);
        }
    }

    throw std::runtime_error("bad file: config.yaml (tried absolute/relative fallback paths)");
}

/*
 * 将 YAML 中的 camera 节点解析为 CameraConfig。
 * 配置按功能分为四组：
 *   connection：设备连接方式；
 *   image：分辨率、像素格式和帧率；
 *   trigger：连续采集或触发采集；
 *   capture：曝光、增益和白平衡。
 * yaml-cpp 的 as<T>() 同时完成类型转换；节点缺失或类型错误时会抛出异常。
 * 这里捕获异常并写入 error，让上层可以用 bool 统一判断初始化是否成功。
 */
static bool LoadCameraConfig(CameraConfig &cfg, std::string &usedPath, std::string *error) {
    try {
        YAML::Node config = LoadConfigWithFallback(usedPath);
        const auto camera = config["camera"];
        cfg.interface = camera["connection"]["interface"].as<std::string>();
        cfg.width = camera["image"]["width"].as<int>();
        cfg.height = camera["image"]["height"].as<int>();
        if (camera["image"]["pixel_format"]) {
            cfg.pixel_format = camera["image"]["pixel_format"].as<std::string>();
        }
        cfg.frame_rate = camera["image"]["frame_rate"].as<int>();
        cfg.frame_rate_enable = camera["image"]["frame_rate_enable"].as<bool>();
        cfg.trigger_mode = camera["trigger"]["mode"].as<std::string>();
        cfg.trigger_source = camera["trigger"]["source"].as<std::string>();
        cfg.exposure_time = camera["capture"]["exposure_time"].as<float>();
        cfg.exposure_auto = camera["capture"]["exposure_auto"].as<bool>();
        cfg.gain = camera["capture"]["gain"].as<float>();
        cfg.gain_auto = camera["capture"]["gain_auto"].as<bool>();
        cfg.white_balance_mode = camera["capture"]["white_balance_mode"].as<std::string>();
        cfg.white_balance_red = camera["capture"]["white_balance_red"].as<float>();
        cfg.white_balance_blue = camera["capture"]["white_balance_blue"].as<float>();
        return true;
    } catch (const std::exception &e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

HikCameraNode::~HikCameraNode() {
    /*
     * 析构时自动关闭相机，这是 RAII 思想的体现：对象生命周期结束，资源也随之释放。
     * shutdown() 会检查句柄是否为空，因此即使 main() 中已经主动关闭，这里再调用也是安全的。
     */
    shutdown();
}

bool HikCameraNode::init(std::string *error) {
    /*
     * init() 负责完整的相机启动流程。
     * 约定：成功返回 true；失败返回 false，并尽可能将原因写入 error。
     * 开始前先 shutdown()，可以防止重复初始化时泄漏旧句柄，也使该节点具备重连能力。
     */
    shutdown();

    /*
     * 第 1 步：加载配置。
     * output_width_ / output_height_ 记录对外公布的图像尺寸，上层可据此预分配帧缓冲区。
     * alignment_logged_ 清零后，新的取流会在第一帧重新打印内存对齐信息。
     */
    CameraConfig cfg;
    std::string configPath;
    if (!LoadCameraConfig(cfg, configPath, error)) {
        return false;
    }
    output_width_ = cfg.width;
    output_height_ = cfg.height;
    alignment_logged_ = false;

    spdlog::info("Loaded camera config: {} | pixel_format={} | size={}x{} | exposure={}us",
                 configPath,
                 cfg.pixel_format,
                 cfg.width,
                 cfg.height,
                 cfg.exposure_time);
    spdlog::info("Pixel conversion backend=opencv");

    /*
     * 第 2 步：枚举设备。
     * MV_CC_EnumDevices() 会让 SDK 扫描指定传输层，这里传入 MV_USB_DEVICE，
     * 所以当前实现只查找 USB 相机，并未根据 cfg.interface 切换传输层。
     * deviceList 保存设备数量和每台设备的描述信息；当前默认选择第 0 台。
     * 如果同时连接多台相机，实际项目中可进一步按序列号精确选择。
     */
    MV_CC_DEVICE_INFO_LIST deviceList{};
    int nRet = MV_CC_EnumDevices(MV_USB_DEVICE, &deviceList);
    if (nRet != MV_OK) {
        if (error) *error = "枚举设备失败";
        return false;
    }
    if (deviceList.nDeviceNum == 0) {
        if (error) *error = "未发现相机";
        return false;
    }

    /*
     * 第 3 步：创建句柄并打开相机。
     * 句柄 handle_ 是后续所有 SDK 操作的设备上下文，可以理解为程序与这台相机的连接凭证。
     * CreateHandle 只创建控制对象，OpenDevice 才真正打开设备。
     * 一旦句柄已创建，后续任何失败分支都调用 shutdown() 统一清理。
     */
    nRet = MV_CC_CreateHandle(&handle_, deviceList.pDeviceInfo[0]);
    if (nRet != MV_OK || handle_ == nullptr) {
        if (error) *error = "创建句柄失败";
        return false;
    }

    nRet = MV_CC_OpenDevice(handle_);
    if (nRet != MV_OK) {
        if (error) *error = "打开设备失败";
        shutdown();
        return false;
    }

    /*
     * 第 4 步：配置输出图像。
     * 先打印相机的像素格式能力，再尝试设置 cfg.pixel_format。
     * 像素格式设置失败被视为“可降级错误”：程序记录警告，但会使用相机当前格式继续。
     * 宽高设置失败则是“不可恢复错误”，因为上层已按配置尺寸准备数据，
     * 如果实际尺寸不一致可能造成缓冲区或后处理错误，所以直接关闭相机并返回 false。
     */
    spdlog::info("Initial PixelFormat support: {}", DescribePixelFormatSupport(handle_));
    if (!SetPixelFormatWithFallbacks(handle_, cfg.pixel_format)) {
        spdlog::warn("Set PixelFormat={} failed with all methods, fallback to camera current format. {}",
                     cfg.pixel_format,
                     DescribePixelFormatSupport(handle_));
    }

    nRet = MV_CC_SetIntValue(handle_, "Width", cfg.width);
    if (nRet != MV_OK) {
        if (error) *error = "设置宽度失败";
        shutdown();
        return false;
    }
    nRet = MV_CC_SetIntValue(handle_, "Height", cfg.height);
    if (nRet != MV_OK) {
        if (error) *error = "设置高度失败";
        shutdown();
        return false;
    }

    /*
     * 第 5 步：配置触发方式。
     * TriggerMode=0：关闭触发模式，相机连续产生图像。
     * TriggerMode=1：开启触发模式，只在收到触发事件时曝光。
     * TriggerSource=7 映射软件触发；其他配置当前统一写入 0，通常对应外部 Line0。
     * 连续采集适合实时视觉，外部触发则适合与其他传感器做硬件同步。
     */
    if (cfg.trigger_mode == "off") {
        MV_CC_SetEnumValue(handle_, "TriggerMode", 0);
    } else {
        MV_CC_SetEnumValue(handle_, "TriggerMode", 1);
        if (cfg.trigger_source == "software") {
            MV_CC_SetEnumValue(handle_, "TriggerSource", 7);
        } else {
            MV_CC_SetEnumValue(handle_, "TriggerSource", 0);
        }
    }

    /*
     * 第 6 步：配置成像参数。
     * 帧率：启用 AcquisitionFrameRateEnable 后，AcquisitionFrameRate 才会成为主动限制。
     * 曝光：ExposureAuto=2 交给相机自动调节；=0 关闭自动后才写入 ExposureTime。
     * 增益：GainAuto 的处理与曝光相同；手动增益越高，画面越亮，噪声通常也越大。
     * 白平衡：手动模式先关闭自动白平衡，再用 Selector 分别选中红、蓝通道写入比例。
     * 配置中的浮点比例乘 1000 后写入 SDK 整数节点，例如 1.2 会写成 1200。
     */
    if (cfg.frame_rate_enable) {
        MV_CC_SetBoolValue(handle_, "AcquisitionFrameRateEnable", true);
        MV_CC_SetFloatValue(handle_, "AcquisitionFrameRate", static_cast<float>(cfg.frame_rate));
    } else {
        MV_CC_SetBoolValue(handle_, "AcquisitionFrameRateEnable", false);
    }

    if (cfg.exposure_auto) {
        MV_CC_SetEnumValue(handle_, "ExposureAuto", 2);
    } else {
        MV_CC_SetEnumValue(handle_, "ExposureAuto", 0);
        MV_CC_SetFloatValue(handle_, "ExposureTime", cfg.exposure_time);
    }

    if (cfg.gain_auto) {
        MV_CC_SetEnumValue(handle_, "GainAuto", 2);
    } else {
        MV_CC_SetEnumValue(handle_, "GainAuto", 0);
        MV_CC_SetFloatValue(handle_, "Gain", cfg.gain);
    }

    if (cfg.white_balance_mode == "manual") {
        MV_CC_SetEnumValue(handle_, "BalanceWhiteAuto", 0);
        MV_CC_SetEnumValue(handle_, "BalanceRatioSelector", 0);
        MV_CC_SetIntValue(handle_, "BalanceRatio", static_cast<int>(cfg.white_balance_red * 1000));
        MV_CC_SetEnumValue(handle_, "BalanceRatioSelector", 2);
        MV_CC_SetIntValue(handle_, "BalanceRatio", static_cast<int>(cfg.white_balance_blue * 1000));
    } else {
        MV_CC_SetEnumValue(handle_, "BalanceWhiteAuto", 1);
    }

    /*
     * 第 7 步：选择取帧策略并启动数据流。
     * LatestImagesOnly 表示应用处理跟不上相机时，丢弃过期帧并优先保留最新画面。
     * 对自瞄这类实时系统，“帧新”通常比“每帧都处理”更重要，
     * 因为队列中积压的旧帧会直接增加检测到控制之间的延迟。
     */
    nRet = MV_CC_SetGrabStrategy(handle_, MV_GrabStrategy_LatestImagesOnly);
    if (nRet != MV_OK) {
        if (error) *error = "设置抓取策略失败";
        shutdown();
        return false;
    }

    // MV_CC_StartGrabbing() 成功后，SDK 才开始接收相机图像，grab() 此时才能取到帧。
    nRet = MV_CC_StartGrabbing(handle_);
    if (nRet != MV_OK) {
        if (error) *error = "开始取流失败";
        shutdown();
        return false;
    }

    spdlog::info("Camera stream started.");
    return true;
}

bool HikCameraNode::grab(cv::Mat &bgr,
                         std::chrono::steady_clock::time_point *capture_tp,
                         std::string *error) {
    /*
     * grab() 完成“取出一帧 -> 转换为 BGR -> 归还 SDK 缓冲区”的完整过程。
     * bgr          ：输出参数，成功时得到 CV_8UC3 格式的 OpenCV 图像。
     * capture_tp   ：可选的采集时刻输出，上层可用它计算算法延迟或做时间同步。
     * error        ：可选的错误文本输出；传 nullptr 表示调用者只关心成功/失败。
     * 函数本身不保留 SDK 原始帧，因此返回后 bgr 不依赖 SDK 缓冲区的生命周期。
     */
    if (handle_ == nullptr) {
        if (error) *error = "camera not initialized";
        return false;
    }

    /*
     * MV_CC_GetImageBuffer() 从 SDK 取出一个图像缓冲区，最后一个参数 1000 是毫秒超时。
     * frame.pBufAddr 指向原始像素数据，frame.stFrameInfo 保存宽、高、像素格式等元数据。
     * 取帧成功后立即使用 steady_clock 记录时刻。steady_clock 不会因系统时间校准而跳变，
     * 所以比 system_clock 更适合统计耗时。
     */
    MV_FRAME_OUT frame = {};
    const int nRet = MV_CC_GetImageBuffer(handle_, &frame, 1000);
    if (nRet != MV_OK) {
        if (error) *error = "获取图像失败";
        return false;
    }
    if (capture_tp) {
        *capture_tp = std::chrono::steady_clock::now();
    }

    const int h = static_cast<int>(frame.stFrameInfo.nHeight);
    const int w = static_cast<int>(frame.stFrameInfo.nWidth);
    const auto src_pixel_type = static_cast<MvGvspPixelType>(frame.stFrameInfo.enPixelType);

    /*
     * 上层算法统一使用 BGR，因此这里根据相机的实际像素格式分支处理：
     *   BGR8       ：数据排列已符合要求，直接 memcpy 到输出图像；
     *   RGB8       ：交换 R/B 通道转成 BGR；
     *   Mono8      ：把单通道灰度值复制到 B、G、R 三个通道；
     *   Bayer8     ：根据传感器颜色排列执行去马赛克，还原三通道彩色图像；
     *   其他格式 ：OpenCV 路径暂不支持，必须先归还缓冲区，再返回错误。
     *
     * bgr.create() 会确保尺寸为 h×w、类型为 CV_8UC3。如果传入 Mat 的尺寸和类型已正确，
     * OpenCV 通常会复用现有内存，避免每帧重复申请。
     */
    bgr.create(h, w, CV_8UC3);
    if (src_pixel_type == PixelType_Gvsp_BGR8_Packed || src_pixel_type == PixelType_Gvsp_HB_BGR8_Packed) {
        std::memcpy(bgr.data, frame.pBufAddr, static_cast<std::size_t>(h) * static_cast<std::size_t>(w) * 3U);
    } else if (src_pixel_type == PixelType_Gvsp_RGB8_Packed) {
        const cv::Mat rgb(h, w, CV_8UC3, frame.pBufAddr);
        cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    } else if (src_pixel_type == PixelType_Gvsp_Mono8) {
        const cv::Mat mono(h, w, CV_8UC1, frame.pBufAddr);
        cv::cvtColor(mono, bgr, cv::COLOR_GRAY2BGR);
    } else if (IsPlainBayer8(src_pixel_type)) {
        const cv::Mat raw(h, w, CV_8UC1, frame.pBufAddr);
        cv::cvtColor(raw, bgr, OpenCvBayerToBgrCode(src_pixel_type));
    } else {
        MV_CC_FreeImageBuffer(handle_, &frame);
        if (error) {
            *error = std::string("unsupported pixel format for OpenCV conversion: ") +
                     PixelFormatName(static_cast<unsigned int>(src_pixel_type));
        }
        return false;
    }

    /*
     * 每次启动取流后只在首帧打印一次内存布局，避免每帧写日志影响实时性。
     * sdk_raw_64B / output_64B 表示原始和输出首地址是否按 64 字节对齐，良好对齐有利于 SIMD 向量化。
     * output_step 是相邻两行数据的字节距离；continuous 表示整幅图像在内存中是否连续。
     * Bayer 路径的 cvtColor() 直接写入预先分配的 bgr，不再创建一张中间彩色图。
     */
    if (!alignment_logged_) {
        spdlog::info("Frame buffer alignment: sdk_raw_64B={} output_64B={} output_step={} continuous={} pixel_format={}",
                     app::memory::IsAligned(frame.pBufAddr),
                     app::memory::IsAligned(bgr.data),
                     bgr.step,
                     bgr.isContinuous(),
                     PixelFormatName(static_cast<unsigned int>(src_pixel_type)));
        if (IsPlainBayer8(src_pixel_type)) {
            spdlog::info("Bayer path writes cvtColor output directly into the preallocated frame pool buffer.");
        }
        alignment_logged_ = true;
    }

    /*
     * frame.pBufAddr 的所有权属于 SDK，应用只是临时借用。
     * 必须在转换完成后调用 MV_CC_FreeImageBuffer() 归还，否则 SDK 可用缓冲区会逐渐耗尽，
     * 最终表现为取帧超时或停止出图。bgr 中的数据已经拷贝/转换完成，归还后仍然有效。
     */
    MV_CC_FreeImageBuffer(handle_, &frame);
    return true;
}

void HikCameraNode::shutdown() {
    /*
     * 按 SDK 要求的相反顺序释放资源：
     *   1. StopGrabbing：停止接收图像；
     *   2. CloseDevice ：断开与硬件的连接；
     *   3. DestroyHandle：销毁 SDK 上下文。
     * 销毁后立即将 handle_ 置空，既防止悬空指针，也让本函数可以安全重复调用。
     * 最后重置对外尺寸和首帧日志标志，使对象恢复到“未初始化”状态。
     */
    if (handle_ != nullptr) {
        MV_CC_StopGrabbing(handle_);
        MV_CC_CloseDevice(handle_);
        MV_CC_DestroyHandle(handle_);
        handle_ = nullptr;
    }
    output_width_ = 0;
    output_height_ = 0;
    alignment_logged_ = false;
}

#ifndef CAMERA_NODE_LIBRARY
/*
 * 本文件有两种编译用途：
 *   - 作为完整视觉系统的相机节点时，CMake 定义 CAMERA_NODE_LIBRARY，排除下面的 main()；
 *   - 作为 camera_app 单独编译时，保留 main()，可独立验证相机连接、配置和画面。
 * 这种设计使相机问题可以脱离推理、PnP 等后续模块单独排查。
 */
int main() {
    // 异步日志放在最前面初始化，保证相机启动期间的成功信息和错误都能被记录。
    app::logging::InitAsyncLogging();

    // 创建相机节点并执行前面讲解的完整 init() 流程；初始化失败时不进入显示循环。
    HikCameraNode camera;
    std::string error;
    if (!camera.init(&error)) {
        spdlog::error("Camera init failed: {}", error);
        return -1;
    }

    const std::string windowName = "Camera";
    cv::Mat img;
    int frameCount = 0;
    double fps = 0.0;
    auto fpsStart = std::chrono::steady_clock::now();

    /*
     * 主循环每次完成三件事：取帧、统计 FPS、显示画面。
     * 取帧失败时记录错误后 continue，允许短暂超时后继续尝试，不会立即退出程序。
     * FPS 使用“统计窗口内帧数 / 实际经过时间”计算，每累计至少 1 秒更新一次，
     * 比用单帧耗时计算更稳定。结果四舍五入后显示在 OpenCV 窗口标题中。
     * cv::waitKey(1) 既让 GUI 处理窗口事件，也读取键盘；27 是 Esc 键的 ASCII 码。
     */
    while (true) {
        if (!camera.grab(img, nullptr, &error)) {
            spdlog::error("Grab failed: {}", error);
            continue;
        }

        ++frameCount;
        const auto now = std::chrono::steady_clock::now();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - fpsStart).count();
        if (elapsedMs >= 1000) {
            fps = frameCount * 1000.0 / static_cast<double>(elapsedMs);
            frameCount = 0;
            fpsStart = now;
        }
        if (fps > 0.0) {
            cv::setWindowTitle(windowName, windowName + " | FPS: " + std::to_string(static_cast<int>(fps + 0.5)));
        }

        cv::imshow(windowName, img);
        if (cv::waitKey(1) == 27) {
            break;
        }
    }

    /*
     * 退出循环后主动关闭相机和所有 OpenCV 窗口。
     * 虽然 camera 的析构函数也会调用 shutdown()，这里显式调用能更清楚地表达程序的收尾顺序。
     */
    camera.shutdown();
    cv::destroyAllWindows();
    return 0;
}
#endif
