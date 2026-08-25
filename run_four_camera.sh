#!/usr/bin/env bash
set -euo pipefail

PACKAGE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -n "${MVS_SDK_LIB_DIR:-}" ]]; then
    SDK_LIB_DIR="${MVS_SDK_LIB_DIR}"
elif [[ "$(uname -m)" == "aarch64" && -f /opt/MVS/lib/aarch64/libMvCameraControl.so ]]; then
    SDK_LIB_DIR="/opt/MVS/lib/aarch64"
else
    SDK_LIB_DIR="${PACKAGE_ROOT}/lib/64"
fi

if [[ "${SDK_LIB_DIR}" == /opt/MVS/* ]]; then
    SDK_ROOT="/opt/MVS"
else
    SDK_ROOT="${PACKAGE_ROOT}"
fi

export MVCAM_SDK_PATH="${MVCAM_SDK_PATH:-${SDK_ROOT}}"
export MVCAM_SDK_VERSION="${MVCAM_SDK_VERSION:-4.8.1}"
export MVCAM_COMMON_RUNENV="${MVCAM_COMMON_RUNENV:-${SDK_ROOT}/lib}"
export MVCAM_SOFTWARE_LIBENV="${MVCAM_SOFTWARE_LIBENV:-${SDK_ROOT}/lib}"
export MVCAM_GENICAM_CLPROTOCOL="${MVCAM_GENICAM_CLPROTOCOL:-${SDK_ROOT}/lib/CLProtocol}"
export ALLUSERSPROFILE="${ALLUSERSPROFILE:-${SDK_ROOT}/MVFG}"

# When launched through SSH, DISPLAY is usually empty even though the Jetson
# desktop is running locally. Route optional preview windows to that desktop.
for argument in "$@"; do
    if [[ "${argument}" == "--preview" && -z "${DISPLAY:-}" && -S /tmp/.X11-unix/X0 ]]; then
        export DISPLAY=:0
        if [[ -r "/run/user/$(id -u)/gdm/Xauthority" ]]; then
            export XAUTHORITY="/run/user/$(id -u)/gdm/Xauthority"
        fi
        break
    fi
done

if [[ -n "${LD_LIBRARY_PATH:-}" ]]; then
    export LD_LIBRARY_PATH="${SDK_LIB_DIR}:${LD_LIBRARY_PATH}"
else
    export LD_LIBRARY_PATH="${SDK_LIB_DIR}"
fi

cmake -S "${PACKAGE_ROOT}" -B "${PACKAGE_ROOT}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMVS_SDK_LIB_DIR="${SDK_LIB_DIR}"
cmake --build "${PACKAGE_ROOT}/build" --target four_camera_capture -j"$(nproc)"

cd "${PACKAGE_ROOT}"
exec "${PACKAGE_ROOT}/build/four_camera_capture" "$@"
