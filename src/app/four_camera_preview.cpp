#include "four_camera_preview.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

namespace {

constexpr char kWindowName[] = "HIKROBOT Four Camera Preview";
constexpr int kCellWidth = 480;
constexpr int kCellHeight = 360;

struct PreviewCell {
    CameraId camera_id;
    const char *name;
    int row;
    int column;
};

constexpr std::array<PreviewCell, 4> kCells = {{
    {CameraId::FRONT, "Front", 0, 0},
    {CameraId::REAR, "Rear", 0, 1},
    {CameraId::LEFT, "Left", 1, 0},
    {CameraId::RIGHT, "Right", 1, 1},
}};

void RenderCell(const std::shared_ptr<const FramePacket> &packet,
                const PreviewCell &cell,
                cv::Mat &mosaic)
{
    const cv::Rect destination_rect(
        cell.column * kCellWidth, cell.row * kCellHeight, kCellWidth, kCellHeight);
    cv::Mat destination = mosaic(destination_rect);
    destination.setTo(cv::Scalar(24, 24, 24));

    if (packet == nullptr || packet->image_data == nullptr ||
        packet->image_data->size() <
            static_cast<std::size_t>(packet->width) * packet->height) {
        cv::putText(destination,
                    std::string(cell.name) + " - waiting",
                    cv::Point(16, 32),
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.7,
                    cv::Scalar(0, 200, 255),
                    2);
        return;
    }

    cv::Mat raw(static_cast<int>(packet->height),
                static_cast<int>(packet->width),
                CV_8UC1,
                const_cast<std::uint8_t *>(packet->image_data->data()));
    cv::Mat bgr;
    // MVS BayerRG8 and OpenCV's Bayer conversion naming use opposite
    // output-channel aliases; this produces a BGR image for imshow().
    cv::cvtColor(raw, bgr, cv::COLOR_BayerBG2BGR);
    cv::resize(bgr, destination, destination.size(), 0.0, 0.0, cv::INTER_AREA);

    const std::string label = std::string(cell.name) + "  frame=" +
                              std::to_string(packet->frame_number) + "  cycle=" +
                              std::to_string(packet->trigger_cycle);
    cv::putText(destination,
                label,
                cv::Point(12, 28),
                cv::FONT_HERSHEY_SIMPLEX,
                0.6,
                cv::Scalar(0, 0, 0),
                4);
    cv::putText(destination,
                label,
                cv::Point(12, 28),
                cv::FONT_HERSHEY_SIMPLEX,
                0.6,
                cv::Scalar(0, 255, 0),
                1);
}

}  // namespace

FourCameraPreview::FourCameraPreview(const MultiCameraManager &manager) : manager_(manager) {}

FourCameraPreview::~FourCameraPreview()
{
    stop();
}

bool FourCameraPreview::start()
{
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
    close_requested_.store(false, std::memory_order_release);
    thread_ = std::jthread([this](std::stop_token stop_token) { run(stop_token); });
    return true;
}

void FourCameraPreview::stop()
{
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void FourCameraPreview::run(std::stop_token stop_token)
{
    try {
        cv::namedWindow(kWindowName, cv::WINDOW_AUTOSIZE);
        cv::Mat mosaic(kCellHeight * 2, kCellWidth * 2, CV_8UC3);
        while (!stop_token.stop_requested()) {
            for (const PreviewCell &cell : kCells) {
                RenderCell(manager_.frameBuffer(cell.camera_id).latest(), cell, mosaic);
            }
            cv::imshow(kWindowName, mosaic);
            const int key = cv::waitKey(1);
            if (key == 27 || key == 'q' || key == 'Q') {
                close_requested_.store(true, std::memory_order_release);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(32));
        }
        cv::destroyWindow(kWindowName);
    } catch (const cv::Exception &exception) {
        spdlog::error("Preview failed: {}", exception.what());
        close_requested_.store(true, std::memory_order_release);
    }
    running_.store(false, std::memory_order_release);
}
