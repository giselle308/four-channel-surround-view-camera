#include "cuda_npp_isp_backend.hpp"

#include <stdexcept>

class CudaNppIspBackend::Impl {};

CudaNppIspBackend::CudaNppIspBackend(const IspConfig &)
{
    throw std::runtime_error(
        "cuda_npp ISP requested, but this build used ENABLE_CUDA_ISP=OFF");
}

CudaNppIspBackend::~CudaNppIspBackend() = default;

bool CudaNppIspBackend::process(const FramePacket &,
                                std::vector<std::uint8_t> &,
                                double &,
                                std::string *error)
{
    if (error != nullptr) {
        *error = "cuda_npp ISP is unavailable in this CPU-only build";
    }
    return false;
}
