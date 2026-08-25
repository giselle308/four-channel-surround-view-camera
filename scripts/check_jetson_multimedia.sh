#!/usr/bin/env bash
set -uo pipefail

failures=0
package_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mvs_root="${MVS_SDK_ROOT:-/opt/MVS}"

pass() { printf '[PASS] %s\n' "$1"; }
fail() { printf '[FAIL] %s\n' "$1" >&2; failures=$((failures + 1)); }

printf '%s\n' '=== Jetson multimedia environment ==='
if [[ -r /etc/nv_tegra_release ]]; then
    cat /etc/nv_tegra_release
    pass 'L4T release file'
else
    fail '/etc/nv_tegra_release is missing (not a supported Jetson runtime)'
fi

if command -v dpkg-query >/dev/null 2>&1; then
    dpkg-query -W nvidia-l4t-core nvidia-l4t-gstreamer 2>/dev/null ||
        fail 'L4T core/GStreamer packages are incomplete'
fi

cuda_root="${CUDA_ROOT:-/usr/local/cuda}"
if [[ -x "${cuda_root}/bin/nvcc" && -r "${cuda_root}/include/cuda_runtime.h" ]]; then
    "${cuda_root}/bin/nvcc" --version | tail -n 1
    pass "CUDA toolkit: ${cuda_root}"
else
    fail "CUDA compiler or headers missing under ${cuda_root}"
fi
if [[ -r "${cuda_root}/include/nppi_color_conversion.h" ]] &&
   grep -q 'nppiCFAToRGB_8u_C1C3R' "${cuda_root}/include/nppi_color_conversion.h"; then
    pass 'NPP BayerRG8 demosaic API'
else
    fail 'NPP Bayer demosaic API is unavailable'
fi

mvs_header="${mvs_root}/include/MvCameraControl.h"
[[ -r "${mvs_header}" ]] || mvs_header="${package_root}/include/MvCameraControl.h"
mvs_library="${mvs_root}/lib/aarch64/libMvCameraControl.so"
[[ -r "${mvs_library}" ]] || mvs_library="${package_root}/lib/64/libMvCameraControl.so"
if [[ -r "${mvs_header}" ]]; then
    pass "MVS header: ${mvs_header}"
else
    fail 'MvCameraControl.h is missing'
fi
if [[ -r "${mvs_library}" ]]; then
    pass "MVS library: ${mvs_library}"
else
    fail 'libMvCameraControl.so is missing'
fi

if command -v gst-launch-1.0 >/dev/null 2>&1; then
    gst-launch-1.0 --version | head -n 2
    pass 'GStreamer runtime'
else
    fail 'gst-launch-1.0 is missing'
fi

for plugin in nvvidconv nvv4l2h264enc nvv4l2h265enc appsrc \
              h264parse h265parse rtph264pay rtph265pay udpsink fakesink filesink; do
    if gst-inspect-1.0 "${plugin}" >/dev/null 2>&1; then
        pass "GStreamer plugin: ${plugin}"
    else
        fail "GStreamer plugin missing: ${plugin}"
    fi
done

if pkg-config --exists gstreamer-1.0 gstreamer-app-1.0; then
    pass "GStreamer development files: $(pkg-config --modversion gstreamer-1.0)"
else
    fail 'GStreamer development files are missing'
fi

if command -v tegrastats >/dev/null 2>&1; then
    pass 'tegrastats'
else
    fail 'tegrastats is missing'
fi

if (( failures != 0 )); then
    printf '[RESULT] FAILED: %d required checks failed\n' "${failures}" >&2
    exit 1
fi
printf '%s\n' '[RESULT] PASS: CUDA/NPP and Jetson hardware encode prerequisites are available'
