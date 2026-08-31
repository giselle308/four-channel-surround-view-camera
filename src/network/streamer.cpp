#include "streamer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <spdlog/spdlog.h>

#include "bounded_latest_queue.hpp"

namespace {

class EndToEndLatencyTracker {
public:
    void record(std::int64_t group_timestamp_ns)
    {
        if (group_timestamp_ns <= 0) return;
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        if (now_ns < group_timestamp_ns) return;
        const double latency_ms =
            static_cast<double>(now_ns - group_timestamp_ns) / 1'000'000.0;
        const std::lock_guard lock(mutex_);
        samples_.push_back(latency_ms);
        sum_ms_ += latency_ms;
        max_ms_ = std::max(max_ms_, latency_ms);
        if (samples_.size() > kWindowSize) {
            sum_ms_ -= samples_.front();
            samples_.pop_front();
        }
    }

    void populate(StreamerStatistics &result) const
    {
        const std::lock_guard lock(mutex_);
        if (samples_.empty()) return;
        result.average_end_to_end_latency_ms = sum_ms_ / samples_.size();
        result.max_end_to_end_latency_ms = max_ms_;
        std::vector<double> sorted(samples_.begin(), samples_.end());
        std::sort(sorted.begin(), sorted.end());
        result.p95_end_to_end_latency_ms = sorted[static_cast<std::size_t>(
            0.95 * static_cast<double>(sorted.size() - 1))];
    }

private:
    static constexpr std::size_t kWindowSize = 2048;
    mutable std::mutex mutex_;
    std::deque<double> samples_;
    double sum_ms_ = 0.0;
    double max_ms_ = 0.0;
};

std::size_t CameraIndex(CameraId camera_id)
{
    return static_cast<std::size_t>(camera_id);
}

class NullStreamer final : public IStreamer {
public:
    bool start(std::string *) override { return true; }
    bool send(EncodedFrame frame) override
    {
        submitted_.fetch_add(1, std::memory_order_relaxed);
        sent_.fetch_add(1, std::memory_order_relaxed);
        if (frame.data != nullptr) {
            bytes_.fetch_add(frame.data->size(), std::memory_order_relaxed);
        }
        latency_.record(frame.group_timestamp);
        return true;
    }
    void stop() override {}
    StreamerStatistics statistics() const override
    {
        StreamerStatistics result;
        result.submitted_frames = submitted_.load(std::memory_order_relaxed);
        result.sent_frames = sent_.load(std::memory_order_relaxed);
        result.sent_bytes = bytes_.load(std::memory_order_relaxed);
        latency_.populate(result);
        return result;
    }

private:
    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> bytes_{0};
    EndToEndLatencyTracker latency_;
};

class FileStreamer final : public IStreamer {
public:
    explicit FileStreamer(StreamerConfig config)
        : config_(std::move(config)), queue_(config_.queue_depth)
    {
    }
    ~FileStreamer() override { stop(); }

    bool start(std::string *error) override
    {
        try {
            const std::filesystem::path path(config_.file_path);
            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path());
            }
            output_.open(path, std::ios::binary | std::ios::trunc);
        } catch (const std::exception &exception) {
            if (error != nullptr) *error = exception.what();
            return false;
        }
        if (!output_) {
            if (error != nullptr) *error = "failed to open encoded output: " + config_.file_path;
            return false;
        }
        running_.store(true, std::memory_order_release);
        worker_ = std::jthread([this](std::stop_token token) { run(token); });
        return true;
    }

    bool send(EncodedFrame frame) override
    {
        if (!running_.load(std::memory_order_acquire)) return false;
        submitted_.fetch_add(1, std::memory_order_relaxed);
        const auto result = queue_.push(std::move(frame));
        if (result.dropped_oldest) drops_.fetch_add(1, std::memory_order_relaxed);
        return result.accepted;
    }

    void stop() override
    {
        running_.store(false, std::memory_order_release);
        queue_.close();
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
        if (output_.is_open()) output_.close();
    }

    StreamerStatistics statistics() const override
    {
        StreamerStatistics result;
        result.submitted_frames = submitted_.load(std::memory_order_relaxed);
        result.sent_frames = sent_.load(std::memory_order_relaxed);
        result.sent_bytes = bytes_.load(std::memory_order_relaxed);
        result.queue_drops = drops_.load(std::memory_order_relaxed);
        result.errors = errors_.load(std::memory_order_relaxed);
        result.queue_size = queue_.size();
        latency_.populate(result);
        return result;
    }

private:
    void run(std::stop_token token)
    {
        EncodedFrame frame;
        while (queue_.waitPop(frame, token)) {
            if (frame.data == nullptr) {
                errors_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            output_.write(reinterpret_cast<const char *>(frame.data->data()),
                          static_cast<std::streamsize>(frame.data->size()));
            if (!output_) {
                errors_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            sent_.fetch_add(1, std::memory_order_relaxed);
            bytes_.fetch_add(frame.data->size(), std::memory_order_relaxed);
            latency_.record(frame.group_timestamp);
        }
    }

    StreamerConfig config_;
    BoundedLatestQueue<EncodedFrame> queue_;
    std::ofstream output_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> drops_{0};
    std::atomic<std::uint64_t> errors_{0};
    EndToEndLatencyTracker latency_;
};

class GstAppSrcStreamer : public IStreamer {
public:
    GstAppSrcStreamer(StreamerConfig config, std::string name)
        : config_(std::move(config)), queue_(config_.queue_depth), name_(std::move(name))
    {
        gst_init(nullptr, nullptr);
    }
    ~GstAppSrcStreamer() override { stop(); }

    bool start(std::string *error) override
    {
        const std::string pipeline_description = buildPipelineDescription();
        GError *gst_error = nullptr;
        pipeline_ = gst_parse_launch(pipeline_description.c_str(), &gst_error);
        if (pipeline_ == nullptr || gst_error != nullptr) {
            if (error != nullptr) {
                *error = gst_error != nullptr
                             ? gst_error->message
                             : name_ + " pipeline creation failed";
            }
            if (gst_error != nullptr) g_error_free(gst_error);
            stop();
            return false;
        }
        app_src_ = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline_), "input"));
        stats_element_ = gst_bin_get_by_name(GST_BIN(pipeline_), "transport_sink");
        bus_ = gst_element_get_bus(pipeline_);
        if (app_src_ == nullptr || bus_ == nullptr ||
            gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            if (error != nullptr) *error = name_ + " pipeline failed to enter PLAYING";
            stop();
            return false;
        }
        running_.store(true, std::memory_order_release);
        worker_ = std::jthread([this](std::stop_token token) { run(token); });
        return true;
    }

    bool send(EncodedFrame frame) override
    {
        if (!running_.load(std::memory_order_acquire)) return false;
        submitted_.fetch_add(1, std::memory_order_relaxed);
        const auto result = queue_.push(std::move(frame));
        if (result.dropped_oldest) drops_.fetch_add(1, std::memory_order_relaxed);
        return result.accepted;
    }

    void stop() override
    {
        running_.store(false, std::memory_order_release);
        queue_.close();
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
        if (app_src_ != nullptr) gst_app_src_end_of_stream(app_src_);
        if (pipeline_ != nullptr) gst_element_set_state(pipeline_, GST_STATE_NULL);
        if (bus_ != nullptr) {
            gst_object_unref(bus_);
            bus_ = nullptr;
        }
        if (app_src_ != nullptr) {
            gst_object_unref(app_src_);
            app_src_ = nullptr;
        }
        if (stats_element_ != nullptr) {
            gst_object_unref(stats_element_);
            stats_element_ = nullptr;
        }
        if (pipeline_ != nullptr) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
    }

    StreamerStatistics statistics() const override
    {
        StreamerStatistics result;
        result.submitted_frames = submitted_.load(std::memory_order_relaxed);
        result.sent_frames = sent_.load(std::memory_order_relaxed);
        result.sent_bytes = bytes_.load(std::memory_order_relaxed);
        result.queue_drops = drops_.load(std::memory_order_relaxed);
        result.errors = errors_.load(std::memory_order_relaxed);
        result.queue_size = queue_.size();
        latency_.populate(result);
        if (stats_element_ != nullptr) {
            GstStructure *stats = nullptr;
            g_object_get(G_OBJECT(stats_element_), "stats", &stats, nullptr);
            if (stats != nullptr) {
                gchar *stats_text = gst_structure_to_string(stats);
                if (stats_text != nullptr) {
                    result.transport_stats = stats_text;
                    g_free(stats_text);
                }
                gst_structure_free(stats);
            }
        }
        return result;
    }

private:
    virtual std::string buildPipelineDescription() const = 0;

protected:
    const StreamerConfig &config() const noexcept { return config_; }

private:
    void run(std::stop_token token)
    {
        EncodedFrame frame;
        while (queue_.waitPop(frame, token)) {
            if (frame.data == nullptr) {
                errors_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            GstBuffer *buffer = gst_buffer_new_allocate(nullptr, frame.data->size(), nullptr);
            GstMapInfo map{};
            if (buffer == nullptr || !gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
                if (buffer != nullptr) gst_buffer_unref(buffer);
                errors_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            std::memcpy(map.data, frame.data->data(), frame.data->size());
            gst_buffer_unmap(buffer, &map);
            GST_BUFFER_PTS(buffer) = frame.pts_ns;
            GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
            GST_BUFFER_DURATION(buffer) = frame.duration_ns;
            const GstFlowReturn result = gst_app_src_push_buffer(app_src_, buffer);
            if (result != GST_FLOW_OK) {
                errors_.fetch_add(1, std::memory_order_relaxed);
            } else {
                sent_.fetch_add(1, std::memory_order_relaxed);
                bytes_.fetch_add(frame.data->size(), std::memory_order_relaxed);
                latency_.record(frame.group_timestamp);
            }
            checkBus();
        }
    }

    void checkBus()
    {
        while (bus_ != nullptr) {
            GstMessage *message = gst_bus_pop_filtered(
                bus_, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
            if (message == nullptr) break;
            GError *error = nullptr;
            gchar *debug = nullptr;
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                gst_message_parse_error(message, &error, &debug);
                errors_.fetch_add(1, std::memory_order_relaxed);
                spdlog::error("{} streamer error: {} debug={}",
                              name_,
                              error != nullptr ? error->message : "unknown",
                              debug != nullptr ? debug : "");
            } else {
                gst_message_parse_warning(message, &error, &debug);
                spdlog::warn("{} streamer warning: {} debug={}",
                             name_,
                             error != nullptr ? error->message : "unknown",
                             debug != nullptr ? debug : "");
            }
            if (error != nullptr) g_error_free(error);
            if (debug != nullptr) g_free(debug);
            gst_message_unref(message);
        }
    }

    StreamerConfig config_;
    BoundedLatestQueue<EncodedFrame> queue_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    GstElement *pipeline_ = nullptr;
    GstAppSrc *app_src_ = nullptr;
    GstBus *bus_ = nullptr;
    GstElement *stats_element_ = nullptr;
    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> drops_{0};
    std::atomic<std::uint64_t> errors_{0};
    EndToEndLatencyTracker latency_;
    std::string name_;
};

class RtpUdpStreamer final : public GstAppSrcStreamer {
public:
    explicit RtpUdpStreamer(StreamerConfig config)
        : GstAppSrcStreamer(std::move(config), "RTP")
    {
    }

private:
    std::string buildPipelineDescription() const override
    {
        if (config().codec == VideoCodec::AV1) {
            throw std::invalid_argument("AV1 is supported only by SRT/file output");
        }
        const bool h264 = config().codec == VideoCodec::H264;
        std::ostringstream pipeline_description;
        pipeline_description
            << "appsrc name=input is-live=true format=time do-timestamp=false block=false "
            << "caps=" << (h264 ? "video/x-h264" : "video/x-h265")
            << ",stream-format=byte-stream,alignment=au "
            << "! queue max-size-buffers=" << config().queue_depth
            << " max-size-bytes=0 max-size-time=0 leaky=downstream "
            << "! " << (h264 ? "h264parse" : "h265parse") << " config-interval=-1 "
            << "! " << (h264 ? "rtph264pay" : "rtph265pay")
            << " pt=" << static_cast<unsigned int>(config().payload_type)
            << " config-interval=1 mtu=" << config().mtu
            << " timestamp-offset=0 seqnum-offset=0 "
            << "! udpsink host=" << config().server_ip
            << " port=" << config().port << " sync=false async=false";
        return pipeline_description.str();
    }
};

class SrtStreamer final : public GstAppSrcStreamer {
public:
    explicit SrtStreamer(StreamerConfig config)
        : GstAppSrcStreamer(std::move(config), "SRT")
    {
    }

private:
    std::string buildPipelineDescription() const override
    {
        if (config().codec == VideoCodec::AV1) {
            std::ostringstream pipeline_description;
            pipeline_description
                << "appsrc name=input is-live=true format=time do-timestamp=false block=false "
                << "caps=video/x-av1,stream-format=obu-stream,alignment=tu "
                << "! queue max-size-buffers=" << config().queue_depth
                << " max-size-bytes=0 max-size-time=0 leaky=downstream "
                << "! av1parse "
                << "! matroskamux streamable=true "
                << "! srtsink name=transport_sink uri=\"srt://" << config().server_ip
                << ':' << config().port
                << "?mode=caller&latency=" << config().srt_latency_ms << "\" "
                << "wait-for-connection=false sync=false async=false";
            return pipeline_description.str();
        }
        const bool h264 = config().codec == VideoCodec::H264;
        std::ostringstream pipeline_description;
        pipeline_description
            << "appsrc name=input is-live=true format=time do-timestamp=false block=false "
            << "caps=" << (h264 ? "video/x-h264" : "video/x-h265")
            << ",stream-format=byte-stream,alignment=au "
            << "! queue max-size-buffers=" << config().queue_depth
            << " max-size-bytes=0 max-size-time=0 leaky=downstream "
            << "! " << (h264 ? "h264parse" : "h265parse") << " config-interval=-1 "
            << "! mpegtsmux alignment=7 "
            << "! srtsink name=transport_sink uri=\"srt://" << config().server_ip << ':' << config().port
            << "?mode=caller&latency=" << config().srt_latency_ms << "\" "
            << "wait-for-connection=false sync=false async=false";
        return pipeline_description.str();
    }
};

}  // namespace

const char *OutputModeName(OutputMode mode) noexcept
{
    switch (mode) {
        case OutputMode::NULL_OUTPUT: return "null";
        case OutputMode::FILE_OUTPUT: return "file";
        case OutputMode::RTP_UDP: return "rtp";
        case OutputMode::SRT: return "srt";
    }
    return "unknown";
}

OutputMode ParseOutputMode(const std::string &name)
{
    if (name == "null") return OutputMode::NULL_OUTPUT;
    if (name == "file") return OutputMode::FILE_OUTPUT;
    if (name == "rtp") return OutputMode::RTP_UDP;
    if (name == "srt") return OutputMode::SRT;
    throw std::invalid_argument("unsupported output mode: " + name);
}

std::unique_ptr<IStreamer> CreateStreamer(StreamerConfig config)
{
    switch (config.mode) {
        case OutputMode::NULL_OUTPUT:
            return std::make_unique<NullStreamer>();
        case OutputMode::FILE_OUTPUT:
            return std::make_unique<FileStreamer>(std::move(config));
        case OutputMode::RTP_UDP:
            return std::make_unique<RtpUdpStreamer>(std::move(config));
        case OutputMode::SRT:
            return std::make_unique<SrtStreamer>(std::move(config));
    }
    throw std::invalid_argument("invalid output mode");
}

MultiStreamOutput::MultiStreamOutput(MultiStreamOutputConfig config)
    : config_(std::move(config))
{
    if (config_.active_camera_count == 0 || config_.active_camera_count > 4 ||
        config_.queue_depth == 0) {
        throw std::invalid_argument("stream output camera count and queue depth are invalid");
    }
    const std::array<const char *, 4> names = {"front", "rear", "left", "right"};
    const char *extension = config_.codec == VideoCodec::H264
                                ? ".h264"
                                : (config_.codec == VideoCodec::H265 ? ".h265" : ".av1");
    for (std::size_t index = 0; index < config_.active_camera_count; ++index) {
        StreamerConfig stream;
        stream.mode = config_.mode;
        stream.codec = config_.codec;
        stream.server_ip = config_.server_ip;
        stream.port = config_.ports[index];
        stream.payload_type = config_.payload_type;
        stream.mtu = config_.mtu;
        stream.srt_latency_ms = config_.srt_latency_ms;
        stream.queue_depth = config_.queue_depth;
        stream.file_path = (std::filesystem::path(config_.file_directory) /
                            (std::string(names[index]) + extension)).string();
        streamers_[index] = CreateStreamer(std::move(stream));
    }
}

MultiStreamOutput::~MultiStreamOutput()
{
    stop();
}

bool MultiStreamOutput::start(std::string *error)
{
    for (std::size_t index = 0; index < config_.active_camera_count; ++index) {
        if (!streamers_[index]->start(error)) {
            stop();
            return false;
        }
    }
    return true;
}

bool MultiStreamOutput::send(EncodedFrame frame)
{
    const std::size_t index = CameraIndex(frame.camera_id);
    if (index >= config_.active_camera_count || streamers_[index] == nullptr) return false;
    return streamers_[index]->send(std::move(frame));
}

void MultiStreamOutput::stop()
{
    for (auto &streamer : streamers_) {
        if (streamer != nullptr) streamer->stop();
    }
}

std::array<StreamerStatistics, 4> MultiStreamOutput::statistics() const
{
    std::array<StreamerStatistics, 4> result;
    for (std::size_t index = 0; index < streamers_.size(); ++index) {
        if (streamers_[index] != nullptr) result[index] = streamers_[index]->statistics();
    }
    return result;
}
