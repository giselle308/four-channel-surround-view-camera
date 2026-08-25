#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <MvCameraControl.h>
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include "hik_camera.hpp"
#include "four_camera_preview.hpp"
#include "frame_grouper.hpp"
#include "frame_rate_selector.hpp"
#include "isp_pipeline.hpp"
#include "logging.hpp"
#include "metadata_sender.hpp"
#include "multi_stream_encoder.hpp"
#include "multi_camera_manager.hpp"
#include "software_trigger_scheduler.hpp"
#include "streamer.hpp"

#ifndef FOUR_CAMERA_CONFIG_PATH
#define FOUR_CAMERA_CONFIG_PATH "config/config.yaml"
#endif

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void HandleSignal(int)
{
    g_stop_requested = 1;
}

const char *CameraName(CameraId camera_id)
{
    switch (camera_id) {
        case CameraId::FRONT: return "Front";
        case CameraId::REAR: return "Rear";
        case CameraId::LEFT: return "Left";
        case CameraId::RIGHT: return "Right";
    }
    return "Unknown";
}

const char *TriggerModeName(TriggerMode mode)
{
    switch (mode) {
        case TriggerMode::FREE_RUN: return "FREE_RUN";
        case TriggerMode::SOFTWARE_TRIGGER: return "SOFTWARE_TRIGGER";
        case TriggerMode::HARDWARE_TRIGGER: return "HARDWARE_TRIGGER";
    }
    return "UNKNOWN";
}

TriggerMode ParseTriggerMode(const std::string &value)
{
    if (value == "FREE_RUN") return TriggerMode::FREE_RUN;
    if (value == "SOFTWARE_TRIGGER") return TriggerMode::SOFTWARE_TRIGGER;
    if (value == "HARDWARE_TRIGGER") return TriggerMode::HARDWARE_TRIGGER;
    throw std::runtime_error("invalid trigger mode: " + value);
}

struct AppConfig {
    MultiCameraManagerConfig manager;
    double software_frequency_hz = 100.0;
    double output_fps = 80.0;
    bool isp_enabled = true;
    IspPipelineConfig isp;
    bool encoder_enabled = true;
    MultiStreamEncoderConfig encoder;
    MultiStreamOutputConfig output;
    bool metadata_enabled = true;
    MetadataSenderConfig metadata;
};

AppConfig LoadConfig(const std::string &path)
{
    const YAML::Node root = YAML::LoadFile(path);
    const YAML::Node config = root["four_camera_capture"];
    if (!config) {
        throw std::runtime_error("missing four_camera_capture configuration");
    }

    AppConfig result;
    const YAML::Node cameras = config["cameras"];
    result.manager.cameras = {{
        {CameraId::FRONT, cameras["Front_SERIAL"].as<std::string>()},
        {CameraId::REAR, cameras["Rear_SERIAL"].as<std::string>()},
        {CameraId::LEFT, cameras["Left_SERIAL"].as<std::string>()},
        {CameraId::RIGHT, cameras["Right_SERIAL"].as<std::string>()},
    }};

    std::set<std::string> serials;
    for (const CameraBinding &binding : result.manager.cameras) {
        if (binding.serial.empty() || !serials.insert(binding.serial).second) {
            throw std::runtime_error("camera serials must be non-empty and unique");
        }
    }

    const YAML::Node image = config["image"];
    result.manager.camera_parameters.width = image["width"].as<std::uint32_t>();
    result.manager.camera_parameters.height = image["height"].as<std::uint32_t>();
    result.manager.camera_parameters.frame_rate = image["frame_rate"].as<float>();
    const std::string pixel_format = image["pixel_format"].as<std::string>();
    if (pixel_format != "BayerRG8") {
        throw std::runtime_error("capture pixel_format must be BayerRG8");
    }
    result.manager.camera_parameters.pixel_format = PixelType_Gvsp_BayerRG8;

    const YAML::Node capture = config["capture"];
    result.manager.camera_parameters.exposure_auto = capture["exposure_auto"].as<bool>();
    result.manager.camera_parameters.exposure_time_us = capture["exposure_time_us"].as<float>();
    result.manager.camera_parameters.auto_exposure_upper_limit_us =
        capture["auto_exposure_upper_limit_us"].as<float>();
    result.manager.camera_parameters.auto_brightness_target =
        capture["auto_brightness_target"].as<std::uint32_t>();
    result.manager.camera_parameters.gain_auto = capture["gain_auto"].as<bool>();
    if (result.manager.camera_parameters.exposure_auto &&
        (result.manager.camera_parameters.auto_exposure_upper_limit_us <= 0.0F ||
         result.manager.camera_parameters.auto_exposure_upper_limit_us >= 10000.0F)) {
        throw std::runtime_error(
            "100 FPS requires auto_exposure_upper_limit_us between 0 and 10000");
    }
    if (!result.manager.camera_parameters.exposure_auto &&
        (result.manager.camera_parameters.exposure_time_us <= 0.0F ||
         result.manager.camera_parameters.exposure_time_us > 10000.0F)) {
        throw std::runtime_error(
            "100 FPS requires exposure_time_us between 0 and 10000");
    }
    if (result.manager.camera_parameters.auto_brightness_target > 255U) {
        throw std::runtime_error("auto_brightness_target must be between 0 and 255");
    }
    if (capture["white_balance_mode"].as<std::string>() != "fixed") {
        throw std::runtime_error("first-stage capture requires fixed white balance");
    }
    result.manager.camera_parameters.white_balance_red = capture["white_balance_red"].as<float>();
    result.manager.camera_parameters.white_balance_blue = capture["white_balance_blue"].as<float>();

    const YAML::Node trigger = config["trigger"];
    result.manager.camera_parameters.trigger_mode =
        ParseTriggerMode(trigger["mode"].as<std::string>());
    result.manager.camera_parameters.trigger_cache = trigger["trigger_cache"].as<bool>();
    result.software_frequency_hz = trigger["software_frequency_hz"].as<double>();
    if (result.software_frequency_hz <= 0.0) {
        throw std::runtime_error("software_frequency_hz must be positive");
    }

    const YAML::Node buffering = config["buffering"];
    if (buffering["frames_per_camera"].as<std::size_t>() != FrameBuffer::kCapacity) {
        throw std::runtime_error("frames_per_camera must be 4");
    }
    result.manager.grab_timeout_ms = buffering["grab_timeout_ms"].as<unsigned int>();
    if (result.manager.grab_timeout_ms == 0) {
        throw std::runtime_error("grab_timeout_ms must be positive");
    }

    const YAML::Node processing = config["processing"];
    if (processing && processing["output_fps"]) {
        result.output_fps = processing["output_fps"].as<double>();
    }
    if (result.output_fps <= 0.0 ||
        result.output_fps > result.manager.camera_parameters.frame_rate) {
        throw std::runtime_error("output_fps must be positive and no greater than capture FPS");
    }

    const YAML::Node isp = config["isp"];
    if (!isp) {
        throw std::runtime_error("missing four_camera_capture.isp configuration");
    }
    result.isp_enabled = isp["enabled"].as<bool>();
    result.isp.processor.backend = ParseIspBackend(isp["backend"].as<std::string>());
    result.isp.processor.width = result.manager.camera_parameters.width;
    result.isp.processor.height = result.manager.camera_parameters.height;
    result.isp.active_camera_count = isp["active_cameras"].as<std::size_t>();
    result.isp.queue_depth = isp["queue_depth"].as<std::size_t>();
    result.isp.processor.output_buffers_per_camera =
        isp["output_buffers_per_camera"].as<std::size_t>();
    result.isp.processor.white_balance_red = isp["white_balance_red"].as<float>();
    result.isp.processor.white_balance_green = isp["white_balance_green"].as<float>();
    result.isp.processor.white_balance_blue = isp["white_balance_blue"].as<float>();
    result.isp.processor.gamma = isp["gamma"].as<float>();
    const std::vector<float> color_matrix =
        isp["color_matrix"].as<std::vector<float>>();
    if (color_matrix.size() != result.isp.processor.color_matrix.size()) {
        throw std::runtime_error("ISP color_matrix must contain exactly 9 values");
    }
    std::copy(color_matrix.begin(), color_matrix.end(),
              result.isp.processor.color_matrix.begin());
    if (result.isp.active_camera_count == 0 || result.isp.active_camera_count > 4 ||
        result.isp.queue_depth == 0) {
        throw std::runtime_error("ISP active_cameras must be 1..4 and queue_depth must be positive");
    }

    const YAML::Node encoder = config["encoder"];
    if (!encoder) {
        throw std::runtime_error("missing four_camera_capture.encoder configuration");
    }
    result.encoder_enabled = encoder["enabled"].as<bool>();
    result.encoder.common.codec = ParseVideoCodec(encoder["codec"].as<std::string>());
    result.encoder.common.width = result.manager.camera_parameters.width;
    result.encoder.common.height = result.manager.camera_parameters.height;
    result.encoder.common.fps = encoder["fps"].as<std::uint32_t>();
    result.encoder.common.gop = encoder["gop"].as<std::uint32_t>();
    result.encoder.common.low_latency = encoder["low_latency"].as<bool>();
    result.encoder.common.queue_depth = encoder["queue_depth"].as<std::size_t>();
    result.encoder.active_camera_count = encoder["active_cameras"].as<std::size_t>();
    result.encoder.bitrates = {
        encoder["bitrate_front"].as<std::uint32_t>(),
        encoder["bitrate_rear"].as<std::uint32_t>(),
        encoder["bitrate_left"].as<std::uint32_t>(),
        encoder["bitrate_right"].as<std::uint32_t>(),
    };
    if (result.encoder.common.fps != static_cast<std::uint32_t>(result.output_fps) ||
        result.encoder.active_camera_count == 0 || result.encoder.active_camera_count > 4 ||
        result.encoder.common.queue_depth == 0 ||
        result.encoder.active_camera_count > result.isp.active_camera_count) {
        throw std::runtime_error(
            "encoder FPS must equal output_fps, camera count must be 1..ISP count, "
            "and queue_depth must be positive");
    }

    const YAML::Node output = config["output"];
    const YAML::Node network = config["network"];
    const YAML::Node streams = config["streams"];
    if (!output || !network || !streams) {
        throw std::runtime_error("missing output/network/streams configuration");
    }
    result.output.mode = ParseOutputMode(output["mode"].as<std::string>());
    result.output.codec = result.encoder.common.codec;
    result.output.file_directory = output["file_directory"].as<std::string>();
    result.output.queue_depth = output["queue_depth"].as<std::size_t>();
    result.output.active_camera_count = result.encoder.active_camera_count;
    result.output.server_ip = network["server_ip"].as<std::string>();
    result.output.payload_type = network["payload_type"].as<std::uint8_t>();
    result.output.mtu = network["mtu"].as<std::uint32_t>();
    result.output.ports = {
        streams["front_port"].as<std::uint16_t>(),
        streams["rear_port"].as<std::uint16_t>(),
        streams["left_port"].as<std::uint16_t>(),
        streams["right_port"].as<std::uint16_t>(),
    };
    result.metadata_enabled = network["metadata_enabled"].as<bool>();
    result.metadata.server_ip = result.output.server_ip;
    result.metadata.port = network["metadata_port"].as<std::uint16_t>();
    result.metadata.queue_depth = network["metadata_queue_depth"].as<std::size_t>();
    if (result.output.queue_depth == 0 || result.output.mtu < 256 ||
        result.output.payload_type > 127 || result.metadata.queue_depth == 0) {
        throw std::runtime_error("invalid output/network queue, MTU, or RTP payload configuration");
    }

    if (result.manager.camera_parameters.width == 0 ||
        result.manager.camera_parameters.height == 0 ||
        result.manager.camera_parameters.width > 1440 ||
        result.manager.camera_parameters.height > 1080 ||
        result.manager.camera_parameters.frame_rate != 100.0F) {
        throw std::runtime_error(
            "image size must be within 1440x1080 and frame rate must be 100 FPS");
    }
    return result;
}

enum class RunMode {
    CAPTURE,
    VALIDATE_CONFIG,
    ENUMERATE_ONLY,
    OPEN_ALL_ONLY,
    OPEN_SINGLE_ONLY,
};

struct CommandLine {
    RunMode mode = RunMode::CAPTURE;
    std::string config_path = FOUR_CAMERA_CONFIG_PATH;
    std::string single_serial;
    double duration_seconds = 0.0;
    bool preview = false;
    bool disable_isp = false;
    bool disable_encoder = false;
    std::size_t isp_camera_count = 0;
    std::size_t encoder_camera_count = 0;
    std::string isp_dump_directory;
    std::string output_mode;
    bool show_help = false;
};

void PrintUsage(const char *program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --validate-config       Parse and validate configuration without a camera\n"
        << "  --enumerate-only        Step1: enumerate USB cameras and print serials\n"
        << "  --single SERIAL         Step2: open one camera by exact serial; add --duration to capture\n"
        << "  --open-all-only         Step3: open four configured cameras, then close\n"
        << "  --duration SECONDS      Run four-thread capture for a fixed duration\n"
        << "  --preview               Show a four-camera preview; press Q or Esc to stop\n"
        << "  --disable-isp           Stop after the 80 FPS FrameRateSelector\n"
        << "  --isp-cameras N         Process 1, 2, 3, or 4 cameras for benchmarking\n"
        << "  --isp-dump-dir PATH     Save the first processed group as raw NV12 files\n"
        << "  --disable-encoder       Stop after ISP\n"
        << "  --encoder-cameras N     Encode 1, 2, 3, or 4 streams; also sets ISP count\n"
        << "  --output MODE           null, file, or rtp (RTP/UDP)\n"
        << "  --config PATH           Use an alternate YAML configuration\n"
        << "  --help                  Show this message\n";
}

CommandLine ParseCommandLine(int argc, char **argv)
{
    CommandLine options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--validate-config") {
            options.mode = RunMode::VALIDATE_CONFIG;
        } else if (argument == "--enumerate-only") {
            options.mode = RunMode::ENUMERATE_ONLY;
        } else if (argument == "--open-all-only") {
            options.mode = RunMode::OPEN_ALL_ONLY;
        } else if (argument == "--single" && index + 1 < argc) {
            options.mode = RunMode::OPEN_SINGLE_ONLY;
            options.single_serial = argv[++index];
        } else if (argument == "--duration" && index + 1 < argc) {
            options.duration_seconds = std::stod(argv[++index]);
        } else if (argument == "--preview") {
            options.preview = true;
        } else if (argument == "--disable-isp") {
            options.disable_isp = true;
        } else if (argument == "--isp-cameras" && index + 1 < argc) {
            options.isp_camera_count = std::stoul(argv[++index]);
            if (options.isp_camera_count == 0 || options.isp_camera_count > 4) {
                throw std::runtime_error("--isp-cameras must be between 1 and 4");
            }
        } else if (argument == "--isp-dump-dir" && index + 1 < argc) {
            options.isp_dump_directory = argv[++index];
        } else if (argument == "--disable-encoder") {
            options.disable_encoder = true;
        } else if (argument == "--encoder-cameras" && index + 1 < argc) {
            options.encoder_camera_count = std::stoul(argv[++index]);
            if (options.encoder_camera_count == 0 || options.encoder_camera_count > 4) {
                throw std::runtime_error("--encoder-cameras must be between 1 and 4");
            }
        } else if (argument == "--output" && index + 1 < argc) {
            options.output_mode = argv[++index];
            ParseOutputMode(options.output_mode);
        } else if (argument == "--config" && index + 1 < argc) {
            options.config_path = argv[++index];
        } else if (argument == "--help" || argument == "-h") {
            options.show_help = true;
        } else {
            throw std::runtime_error("unknown or incomplete argument: " + argument);
        }
    }
    if (options.duration_seconds < 0.0) {
        throw std::runtime_error("duration must not be negative");
    }
    return options;
}

bool PrintEnumeratedCameras()
{
    std::vector<HikDeviceInfo> devices;
    std::string error;
    if (!HikCamera::enumerateUsbDevices(devices, &error)) {
        spdlog::error("Enumeration failed: {}", error);
        return false;
    }
    spdlog::info("USB cameras found: {}", devices.size());
    for (const HikDeviceInfo &device : devices) {
        spdlog::info("serial={} model={} user_name={}",
                     device.serial,
                     device.model,
                     device.user_defined_name);
    }
    return true;
}

void DumpFirstIspGroup(const ProcessedFrameGroup &group,
                       const std::string &directory)
{
    if (directory.empty()) {
        return;
    }
    std::filesystem::create_directories(directory);
    const std::array<const char *, 4> names = {"front", "rear", "left", "right"};
    for (std::size_t index = 0; index < group.frames.size(); ++index) {
        const auto &frame = group.frames[index];
        if (frame == nullptr || frame->image_data == nullptr) {
            continue;
        }
        const std::filesystem::path path =
            std::filesystem::path(directory) /
            (std::string(names[index]) + "_" + std::to_string(group.group_id) + ".nv12");
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(frame->image_data->data()),
                     static_cast<std::streamsize>(frame->image_data->size()));
        if (!output) {
            throw std::runtime_error("failed to write ISP dump: " + path.string());
        }
        spdlog::info("Wrote ISP NV12 dump: {}", path.string());
    }
}

int RunSingleCameraDiagnostic(const std::string &serial,
                              const AppConfig &config,
                              double duration_seconds)
{
    HikCamera camera;
    std::string error;
    if (!camera.openBySerial(serial, config.manager.camera_parameters, &error)) {
        spdlog::error("Single-camera open failed for serial={}: {}", serial, error);
        return 1;
    }
    if (duration_seconds <= 0.0) {
        spdlog::info("Single-camera serial open passed: {}", serial);
        return 0;
    }
    if (!camera.startGrabbing(&error)) {
        spdlog::error("Single-camera grabbing start failed for serial={}: {}", serial, error);
        return 1;
    }

    SoftwareTriggerScheduler scheduler(
        [&camera](std::uint64_t) { return camera.softwareTrigger(); },
        config.software_frequency_hz);
    if (!scheduler.start()) {
        spdlog::error("Single-camera software trigger scheduler failed to start.");
        return 1;
    }

    const auto start = std::chrono::steady_clock::now();
    std::uint64_t complete_frames = 0;
    std::uint64_t incomplete_frames = 0;
    std::uint64_t timeouts = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() <
           duration_seconds) {
        HikCamera::FrameLease frame;
        const HikGrabResult result =
            camera.getImageBuffer(frame, config.manager.grab_timeout_ms, &error);
        if (result == HikGrabResult::TIMEOUT) {
            ++timeouts;
            continue;
        }
        if (result == HikGrabResult::ERROR) {
            scheduler.stop();
            spdlog::error("Single-camera grab failed for serial={}: {}", serial, error);
            return 1;
        }
        const HikFrameMetadata metadata = frame.metadata();
        const std::size_t expected_size =
            static_cast<std::size_t>(metadata.width) * metadata.height;
        if (metadata.width == config.manager.camera_parameters.width &&
            metadata.height == config.manager.camera_parameters.height &&
            metadata.pixel_format == PixelType_Gvsp_BayerRG8 &&
            metadata.data_size >= expected_size && frame.data() != nullptr) {
            ++complete_frames;
        } else {
            ++incomplete_frames;
            if (incomplete_frames <= 5) {
                spdlog::warn(
                    "Single-camera incomplete frame: frame_no={} data_size={} expected={} "
                    "lost_packets={} trigger_index={} exposure_us={:.1f}",
                    metadata.frame_number,
                    metadata.data_size,
                    expected_size,
                    metadata.lost_packets,
                    metadata.trigger_index,
                    metadata.exposure_time_us);
            }
        }
    }
    scheduler.stop();
    const SoftwareTriggerStatistics trigger_stats = scheduler.statistics();
    spdlog::info(
        "Single-camera diagnostic serial={} duration={:.2f}s complete={} incomplete={} "
        "timeouts={} trigger_cycles={} trigger_failed={}",
        serial,
        duration_seconds,
        complete_frames,
        incomplete_frames,
        timeouts,
        trigger_stats.generated_cycles,
        trigger_stats.failed_cycles);
    return incomplete_frames == 0 && complete_frames > 0 ? 0 : 2;
}

}  // namespace

class FourCameraCapture {
public:
    int run(const CommandLine &options)
    {
        if (options.mode == RunMode::ENUMERATE_ONLY) {
            return PrintEnumeratedCameras() ? 0 : 1;
        }

        AppConfig config;
        try {
            config = LoadConfig(options.config_path);
        } catch (const std::exception &exception) {
            spdlog::error("Config validation failed: {}", exception.what());
            return 1;
        }
        spdlog::info("Config OK: path={} size={}x{} capture_fps={} output_fps={} "
                     "pixel=BayerRG8 trigger={}",
                     options.config_path,
                     config.manager.camera_parameters.width,
                     config.manager.camera_parameters.height,
                     config.manager.camera_parameters.frame_rate,
                     config.output_fps,
                     TriggerModeName(config.manager.camera_parameters.trigger_mode));

        if (options.mode == RunMode::VALIDATE_CONFIG) {
            for (const CameraBinding &binding : config.manager.cameras) {
                spdlog::info("{} serial={}", CameraName(binding.camera_id), binding.serial);
            }
            return 0;
        }

        if (options.mode == RunMode::OPEN_SINGLE_ONLY) {
            return RunSingleCameraDiagnostic(
                options.single_serial, config, options.duration_seconds);
        }

        if (options.disable_isp) {
            config.isp_enabled = false;
        }
        if (options.isp_camera_count != 0) {
            config.isp.active_camera_count = options.isp_camera_count;
        }
        if (options.disable_encoder) {
            config.encoder_enabled = false;
        }
        if (options.encoder_camera_count != 0) {
            config.encoder.active_camera_count = options.encoder_camera_count;
            config.isp.active_camera_count = options.encoder_camera_count;
        }
        if (!options.output_mode.empty()) {
            config.output.mode = ParseOutputMode(options.output_mode);
        }
        config.output.active_camera_count = config.encoder.active_camera_count;
        if (config.encoder_enabled && !config.isp_enabled) {
            spdlog::error("Encoder requires ISP NV12 output; remove --disable-isp or disable encoder.");
            return 1;
        }

        std::unique_ptr<MultiStreamOutput> stream_output;
        std::unique_ptr<MultiStreamEncoder> stream_encoder;
        if (config.encoder_enabled) {
            try {
                std::string error;
                stream_output = std::make_unique<MultiStreamOutput>(config.output);
                if (!stream_output->start(&error)) {
                    spdlog::error("Output startup failed: {}", error);
                    return 1;
                }
                stream_encoder = std::make_unique<MultiStreamEncoder>(
                    config.encoder,
                    [output = stream_output.get()](EncodedFrame frame) {
                        output->send(std::move(frame));
                    });
                if (!stream_encoder->start(&error)) {
                    spdlog::error("Hardware encoder startup failed: {}", error);
                    return 1;
                }
                spdlog::info("Hardware encoders started: codec={} cameras={} fps={} gop={} output={}",
                             VideoCodecName(config.encoder.common.codec),
                             config.encoder.active_camera_count,
                             config.encoder.common.fps,
                             config.encoder.common.gop,
                             OutputModeName(config.output.mode));
            } catch (const std::exception &exception) {
                spdlog::error("Hardware encoder startup failed: {}", exception.what());
                return 1;
            }
        }

        std::unique_ptr<MetadataSender> metadata_sender;
        if (config.encoder_enabled && config.output.mode == OutputMode::RTP_UDP &&
            config.metadata_enabled) {
            std::string error;
            metadata_sender = std::make_unique<MetadataSender>(config.metadata);
            if (!metadata_sender->start(&error)) {
                spdlog::error("Metadata sender startup failed: {}", error);
                return 1;
            }
            spdlog::info("Metadata UDP started: {}:{} packet_size={}",
                         config.metadata.server_ip,
                         config.metadata.port,
                         kFrameGroupMetadataPacketSize);
        }

        std::unique_ptr<IspPipeline> isp_pipeline;
        std::shared_ptr<std::atomic<bool>> isp_dumped;
        if (config.isp_enabled) {
            try {
                IspPipeline::OutputCallback output_callback;
                if (!options.isp_dump_directory.empty() || stream_encoder != nullptr) {
                    isp_dumped = std::make_shared<std::atomic<bool>>(false);
                    output_callback = [directory = options.isp_dump_directory,
                                       isp_dumped,
                                       encoder = stream_encoder.get(),
                                       metadata = metadata_sender.get()](ProcessedFrameGroup group) {
                        if (!directory.empty() &&
                            !isp_dumped->exchange(true, std::memory_order_acq_rel)) {
                            DumpFirstIspGroup(group, directory);
                        }
                        if (metadata != nullptr) {
                            metadata->send(group);
                        }
                        if (encoder != nullptr) {
                            encoder->submit(std::move(group));
                        }
                    };
                }
                isp_pipeline = std::make_unique<IspPipeline>(
                    config.isp, std::move(output_callback));
                isp_pipeline->start();
                spdlog::info("ISP started: backend={} cameras={} queue_depth={} output=NV12",
                             IspBackendName(config.isp.processor.backend),
                             config.isp.active_camera_count,
                             config.isp.queue_depth);
            } catch (const std::exception &exception) {
                spdlog::error("ISP startup failed: {}", exception.what());
                return 1;
            }
        }

        MultiCameraManager manager;
        std::string error;
        if (!manager.openAll(config.manager, &error)) {
            spdlog::error("Four-camera open failed: {}", error);
            return 1;
        }
        if (options.mode == RunMode::OPEN_ALL_ONLY) {
            spdlog::info("Four-camera open test passed.");
            return 0;
        }
        if (!manager.startCapture(&error)) {
            spdlog::error("Four-thread capture start failed: {}", error);
            return 1;
        }

        std::unique_ptr<SoftwareTriggerScheduler> software_scheduler;
        if (config.manager.camera_parameters.trigger_mode == TriggerMode::SOFTWARE_TRIGGER) {
            software_scheduler = std::make_unique<SoftwareTriggerScheduler>(
                [&manager](std::uint64_t cycle) { return manager.softwareTriggerAll(cycle); },
                config.software_frequency_hz);
            if (!software_scheduler->start()) {
                spdlog::error("Software trigger scheduler failed to start.");
                manager.stop();
                return 1;
            }
        }

        std::unique_ptr<FourCameraPreview> preview;
        if (options.preview) {
            preview = std::make_unique<FourCameraPreview>(manager);
            preview->start();
            spdlog::info("Preview enabled. Press Q/Esc or Ctrl+C to stop.");
        }

        std::signal(SIGINT, HandleSignal);
        std::signal(SIGTERM, HandleSignal);
        g_stop_requested = 0;
        FrameGrouper frame_grouper({
            &manager.frameBuffer(CameraId::FRONT),
            &manager.frameBuffer(CameraId::REAR),
            &manager.frameBuffer(CameraId::LEFT),
            &manager.frameBuffer(CameraId::RIGHT),
        });
        FrameRateSelector frame_rate_selector(config.output_fps);
        std::uint64_t last_group_cycle = 0;
        std::uint64_t last_selected_cycle = 0;
        std::int64_t last_selected_timestamp = 0;
        std::uint64_t sync_cycle_gaps = 0;
        const auto run_start = std::chrono::steady_clock::now();
        auto stats_time = run_start;
        std::array<std::uint64_t, 4> previous_received{};
        FrameRateSelectorStatistics previous_selector_stats;
        std::array<std::uint64_t, 4> previous_encoded{};
        std::array<std::uint64_t, 4> previous_encoded_bytes{};
        const std::array<CameraId, 4> camera_ids = {
            CameraId::FRONT, CameraId::REAR, CameraId::LEFT, CameraId::RIGHT};

        spdlog::info("Four-camera capture running. Send SIGINT/SIGTERM to stop.");
        while (g_stop_requested == 0) {
            if (preview != nullptr && preview->closeRequested()) {
                break;
            }

            auto complete_groups = frame_grouper.completeAfter(last_group_cycle);
            for (FrameGroup &group : complete_groups) {
                if (last_group_cycle != 0 && group.trigger_cycle > last_group_cycle + 1) {
                    sync_cycle_gaps += group.trigger_cycle - last_group_cycle - 1;
                }
                last_group_cycle = group.trigger_cycle;
                auto selected = frame_rate_selector.select(std::move(group));
                if (selected.has_value()) {
                    last_selected_cycle = selected->trigger_cycle;
                    last_selected_timestamp = selected->group_timestamp;
                    if (isp_pipeline != nullptr) {
                        isp_pipeline->submit(std::move(*selected));
                    }
                }
            }

            // This is the downstream consumer cadence, not a camera-thread
            // delay. Four 4-deep rings retain roughly 40 ms at 100 FPS.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const auto now = std::chrono::steady_clock::now();
            const double runtime = std::chrono::duration<double>(now - run_start).count();
            if (options.duration_seconds > 0.0 && runtime >= options.duration_seconds) {
                break;
            }
            const double stats_elapsed = std::chrono::duration<double>(now - stats_time).count();
            if (stats_elapsed < 1.0) {
                continue;
            }

            for (std::size_t index = 0; index < camera_ids.size(); ++index) {
                const CameraId camera_id = camera_ids[index];
                const CameraStatistics stats = manager.statistics(camera_id);
                const double fps = static_cast<double>(stats.received_frames - previous_received[index]) /
                                   stats_elapsed;
                previous_received[index] = stats.received_frames;
                const auto latest = manager.frameBuffer(camera_id).latest();
                spdlog::info(
                    "{} fps={:.1f} frames={} frame_no={} cycle={} device_ts={} drop={} timeout={} "
                    "grab_error={} invalid={} ring_overwrite={} pool_exhausted={} trigger_fail={}",
                    CameraName(camera_id),
                    fps,
                    stats.received_frames,
                    latest != nullptr ? latest->frame_number : 0,
                    latest != nullptr ? latest->trigger_cycle : 0,
                    latest != nullptr ? latest->device_timestamp : 0,
                    stats.frame_number_drops,
                    stats.grab_timeouts,
                    stats.grab_errors,
                    stats.invalid_frames,
                    stats.buffer_overwrites,
                    stats.buffer_pool_exhausted,
                    stats.trigger_failures);
            }
            if (software_scheduler != nullptr) {
                const SoftwareTriggerStatistics stats = software_scheduler->statistics();
                spdlog::info("Trigger cycles={} success={} failed={} scheduler_overrun={}",
                             stats.generated_cycles,
                             stats.fully_successful_cycles,
                             stats.failed_cycles,
                             stats.scheduler_overruns);
            }
            const FrameRateSelectorStatistics selector_stats =
                frame_rate_selector.statistics();
            const double input_fps =
                static_cast<double>(selector_stats.input_groups -
                                    previous_selector_stats.input_groups) /
                stats_elapsed;
            const double output_fps =
                static_cast<double>(selector_stats.output_groups -
                                    previous_selector_stats.output_groups) /
                stats_elapsed;
            spdlog::info(
                "FrameGroup input_fps={:.1f} output_fps={:.1f} input={} output={} "
                "selector_drop={} invalid={} pending={} sync_cycle_gap={} "
                "selected_cycle={} group_ts={}",
                input_fps,
                output_fps,
                selector_stats.input_groups,
                selector_stats.output_groups,
                selector_stats.dropped_groups,
                selector_stats.invalid_groups,
                selector_stats.has_pending_group,
                sync_cycle_gaps,
                last_selected_cycle,
                last_selected_timestamp);
            previous_selector_stats = selector_stats;
            if (isp_pipeline != nullptr) {
                const IspPipelineStatistics isp_stats = isp_pipeline->statistics();
                spdlog::info(
                    "ISP backend={} cameras={} group_fps={:.1f} groups={} frames={} "
                    "queue={}/{} isp_drop={} errors={} latency_ms avg={:.3f} p95={:.3f} max={:.3f}",
                    IspBackendName(config.isp.processor.backend),
                    config.isp.active_camera_count,
                    static_cast<double>(isp_stats.processed_groups) / runtime,
                    isp_stats.processed_groups,
                    isp_stats.processed_frames,
                    isp_stats.queue_size,
                    config.isp.queue_depth,
                    isp_stats.queue_drops,
                    isp_stats.processing_errors,
                    isp_stats.average_frame_latency_ms,
                    isp_stats.p95_frame_latency_ms,
                    isp_stats.max_frame_latency_ms);
            }
            if (stream_encoder != nullptr) {
                const auto encoder_stats = stream_encoder->statistics();
                for (std::size_t index = 0;
                     index < config.encoder.active_camera_count;
                     ++index) {
                    const double encoded_fps =
                        static_cast<double>(encoder_stats[index].encoded_frames -
                                            previous_encoded[index]) /
                        stats_elapsed;
                    const double bitrate_mbps =
                        static_cast<double>(encoder_stats[index].encoded_bytes -
                                            previous_encoded_bytes[index]) * 8.0 /
                        stats_elapsed / 1'000'000.0;
                    previous_encoded[index] = encoder_stats[index].encoded_frames;
                    previous_encoded_bytes[index] = encoder_stats[index].encoded_bytes;
                    spdlog::info(
                        "Encode {} fps={:.1f} frames={} bitrate={:.2f}Mbps queue={}/{} "
                        "queue_drop={} encoder_drop={} errors={} "
                        "latency_ms avg={:.3f} p95={:.3f} max={:.3f}",
                        CameraName(camera_ids[index]),
                        encoded_fps,
                        encoder_stats[index].encoded_frames,
                        bitrate_mbps,
                        encoder_stats[index].queue_size,
                        config.encoder.common.queue_depth,
                        encoder_stats[index].queue_drops,
                        encoder_stats[index].encoder_drops,
                        encoder_stats[index].errors,
                        encoder_stats[index].average_latency_ms,
                        encoder_stats[index].p95_latency_ms,
                        encoder_stats[index].max_latency_ms);
                }
            }
            if (stream_output != nullptr) {
                const auto output_stats = stream_output->statistics();
                for (std::size_t index = 0;
                     index < config.output.active_camera_count;
                     ++index) {
                    spdlog::info(
                        "Network {} mode={} sent={} bytes={} queue={}/{} drop={} errors={} port={} "
                        "end_to_end_ms avg={:.3f} p95={:.3f} max={:.3f}",
                        CameraName(camera_ids[index]),
                        OutputModeName(config.output.mode),
                        output_stats[index].sent_frames,
                        output_stats[index].sent_bytes,
                        output_stats[index].queue_size,
                        config.output.queue_depth,
                        output_stats[index].queue_drops,
                        output_stats[index].errors,
                        config.output.ports[index],
                        output_stats[index].average_end_to_end_latency_ms,
                        output_stats[index].p95_end_to_end_latency_ms,
                        output_stats[index].max_end_to_end_latency_ms);
                }
            }
            if (metadata_sender != nullptr) {
                const MetadataSenderStatistics metadata_stats = metadata_sender->statistics();
                spdlog::info(
                    "Metadata sent={} queue={}/{} drop={} errors={} port={}",
                    metadata_stats.sent_packets,
                    metadata_stats.queue_size,
                    config.metadata.queue_depth,
                    metadata_stats.queue_drops,
                    metadata_stats.errors,
                    config.metadata.port);
            }
            stats_time = now;
        }

        if (preview != nullptr) {
            preview->stop();
        }
        if (software_scheduler != nullptr) {
            software_scheduler->stop();
        }
        if (isp_pipeline != nullptr) {
            isp_pipeline->stop();
        }
        if (stream_encoder != nullptr) {
            stream_encoder->stop();
        }
        if (stream_output != nullptr) {
            stream_output->stop();
        }
        if (metadata_sender != nullptr) {
            metadata_sender->stop();
        }
        manager.stop();
        spdlog::info("Four-camera capture stopped cleanly.");
        return 0;
    }
};

int main(int argc, char **argv)
{
    app::logging::InitAsyncLogging();
    try {
        const CommandLine options = ParseCommandLine(argc, argv);
        if (options.show_help) {
            PrintUsage(argv[0]);
            return 0;
        }
        FourCameraCapture application;
        return application.run(options);
    } catch (const std::exception &exception) {
        spdlog::error("Startup failed: {}", exception.what());
        PrintUsage(argv[0]);
        return 1;
    }
}
