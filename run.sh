#!/usr/bin/env bash
set -euo pipefail

PACKAGE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_LIB_DIR="${PACKAGE_ROOT}/lib/64"

export MVCAM_SDK_PATH="${PACKAGE_ROOT}"
export MVCAM_COMMON_RUNENV="${PACKAGE_ROOT}/lib"
export MVCAM_SOFTWARE_LIBENV="${PACKAGE_ROOT}/lib"
export MVCAM_GENICAM_CLPROTOCOL="${PACKAGE_ROOT}/lib/CLProtocol"
if [[ -n "${LD_LIBRARY_PATH:-}" ]]; then
    export LD_LIBRARY_PATH="${SDK_LIB_DIR}:${LD_LIBRARY_PATH}"
else
    export LD_LIBRARY_PATH="${SDK_LIB_DIR}"
fi

cmake -S "${PACKAGE_ROOT}" -B "${PACKAGE_ROOT}/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "${PACKAGE_ROOT}/build" --target camera_app -j"$(nproc)"

cd "${PACKAGE_ROOT}"
exec "${PACKAGE_ROOT}/build/camera_app" "$@"

