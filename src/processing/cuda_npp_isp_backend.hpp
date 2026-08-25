#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "frame_packet.hpp"
#include "isp_processor.hpp"

class CudaNppIspBackend {
public:
    explicit CudaNppIspBackend(const IspConfig &config);
    ~CudaNppIspBackend();

    CudaNppIspBackend(const CudaNppIspBackend &) = delete;
    CudaNppIspBackend &operator=(const CudaNppIspBackend &) = delete;

    bool process(const FramePacket &input,
                 std::vector<std::uint8_t> &output_nv12,
                 double &latency_ms,
                 std::string *error);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
