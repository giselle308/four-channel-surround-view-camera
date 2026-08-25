cmake_minimum_required(VERSION 3.16)
project(surround_view_camera_driver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)
find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc highgui)
find_package(yaml-cpp REQUIRED)
find_package(spdlog REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
)

option(ENABLE_CUDA_ISP "Build the CUDA/NPP BayerRG8 to NV12 ISP backend" ON)
if(ENABLE_CUDA_ISP)
    include(CheckLanguage)
    if(NOT CMAKE_CUDA_COMPILER)
        find_program(CUDA_NVCC_EXECUTABLE
            NAMES nvcc
            HINTS ENV CUDA_ROOT
            PATHS /usr/local/cuda
            PATH_SUFFIXES bin
            NO_DEFAULT_PATH
        )
        if(CUDA_NVCC_EXECUTABLE)
            set(CMAKE_CUDA_COMPILER "${CUDA_NVCC_EXECUTABLE}" CACHE FILEPATH
                "CUDA compiler selected for the Jetson ISP backend" FORCE)
        endif()
    endif()
    check_language(CUDA)
    if(NOT CMAKE_CUDA_COMPILER)
        message(FATAL_ERROR
            "CUDA compiler not found. Phase 2 requires the CUDA/NPP ISP backend; "
            "run scripts/check_jetson_multimedia.sh on the Jetson, or explicitly "
            "set ENABLE_CUDA_ISP=OFF for CPU-only development tests.")
    endif()
    enable_language(CUDA)
    find_package(CUDAToolkit 12 REQUIRED)
    set(ISP_CUDA_SOURCE src/processing/cuda_npp_isp_backend.cu)
else()
    set(ISP_CUDA_SOURCE src/processing/cuda_npp_isp_backend_stub.cpp)
endif()

include(CTest)

set(MVS_SDK_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/64" CACHE PATH
    "Directory containing the platform-specific MVS SDK libraries")
find_library(MVS_CAMERA_CONTROL
    NAMES MvCameraControl
    PATHS "${MVS_SDK_LIB_DIR}"
    NO_DEFAULT_PATH
    REQUIRED
)

add_library(hik_camera STATIC
    src/camera/hik_camera.cpp
)

target_include_directories(hik_camera PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/camera"
)

target_link_libraries(hik_camera PUBLIC
    spdlog::spdlog
    "${MVS_CAMERA_CONTROL}"
)

add_library(capture_types INTERFACE)
target_include_directories(capture_types INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/capture"
)

add_library(multi_camera_manager STATIC
    src/capture/multi_camera_manager.cpp
)
target_include_directories(multi_camera_manager PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/capture"
)
target_link_libraries(multi_camera_manager PUBLIC
    capture_types
    hik_camera
    spdlog::spdlog
    Threads::Threads
)

add_library(camera_sync STATIC
    src/sync/software_trigger_scheduler.cpp
)
target_include_directories(camera_sync PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sync"
)
target_link_libraries(camera_sync PUBLIC
    capture_types
    Threads::Threads
)

add_library(frame_processing STATIC
    src/processing/frame_rate_selector.cpp
)
target_include_directories(frame_processing PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/processing"
)
target_link_libraries(frame_processing PUBLIC
    camera_sync
    capture_types
)

add_library(isp_processing STATIC
    src/processing/isp_processor.cpp
    src/processing/isp_pipeline.cpp
    ${ISP_CUDA_SOURCE}
)
target_include_directories(isp_processing PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/processing"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(isp_processing PUBLIC
    frame_processing
    capture_types
    ${OpenCV_LIBS}
    spdlog::spdlog
    Threads::Threads
)
if(ENABLE_CUDA_ISP)
    target_link_libraries(isp_processing PUBLIC
        CUDA::cudart
        CUDA::nppc
        CUDA::nppicc
        CUDA::nppif
    )
    target_compile_definitions(isp_processing PUBLIC ISP_HAS_CUDA_NPP=1)
    set_target_properties(isp_processing PROPERTIES
        CUDA_STANDARD 17
        CUDA_STANDARD_REQUIRED ON
        CUDA_ARCHITECTURES 87
    )
else()
    target_compile_definitions(isp_processing PUBLIC ISP_HAS_CUDA_NPP=0)
endif()

add_library(video_encoding STATIC
    src/encoding/gstreamer_encoder.cpp
    src/encoding/multi_stream_encoder.cpp
)
target_include_directories(video_encoding PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/encoding"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
)
target_link_libraries(video_encoding PUBLIC
    isp_processing
    capture_types
    PkgConfig::GSTREAMER
    spdlog::spdlog
    Threads::Threads
)

add_library(network_streaming STATIC
    src/network/streamer.cpp
    src/network/metadata_sender.cpp
)
target_include_directories(network_streaming PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/network"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
)
target_link_libraries(network_streaming PUBLIC
    video_encoding
    isp_processing
    PkgConfig::GSTREAMER
    spdlog::spdlog
    Threads::Threads
)

if(BUILD_TESTING)
    add_executable(frame_buffer_test
        tests/frame_buffer_test.cpp
    )
    target_link_libraries(frame_buffer_test PRIVATE capture_types)
    add_test(NAME frame_buffer_test COMMAND frame_buffer_test)

    add_executable(sync_test
        tests/sync_test.cpp
    )
    target_link_libraries(sync_test PRIVATE camera_sync capture_types)
    add_test(NAME sync_test COMMAND sync_test)

    add_executable(frame_rate_selector_test
        tests/frame_rate_selector_test.cpp
    )
    target_link_libraries(frame_rate_selector_test PRIVATE frame_processing)
    add_test(NAME frame_rate_selector_test COMMAND frame_rate_selector_test)

    add_executable(isp_processor_test
        tests/isp_processor_test.cpp
    )
    target_link_libraries(isp_processor_test PRIVATE isp_processing)
    add_test(NAME isp_processor_test COMMAND isp_processor_test)

    add_executable(metadata_wire_test
        tests/metadata_wire_test.cpp
    )
    target_link_libraries(metadata_wire_test PRIVATE network_streaming)
    add_test(NAME metadata_wire_test COMMAND metadata_wire_test)
endif()

add_executable(camera_app
    src/camera/camera.cpp
)

target_include_directories(camera_app PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/camera"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(camera_app PRIVATE
    ${OpenCV_LIBS}
    yaml-cpp
    spdlog::spdlog
    Threads::Threads
    "${MVS_CAMERA_CONTROL}"
)

target_compile_definitions(camera_app PRIVATE
    CAMERA_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/config/config.yaml"
)

add_executable(four_camera_capture
    src/app/four_camera_capture.cpp
    src/app/four_camera_preview.cpp
)
target_include_directories(four_camera_capture PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(four_camera_capture PRIVATE
    ${OpenCV_LIBS}
    multi_camera_manager
    camera_sync
    frame_processing
    isp_processing
    video_encoding
    network_streaming
    yaml-cpp
    spdlog::spdlog
)
target_compile_definitions(four_camera_capture PRIVATE
    FOUR_CAMERA_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/config/config.yaml"
)

# 让 build/camera_app 能直接找到随包附带的海康 SDK 运行库。
set_target_properties(camera_app PROPERTIES
    BUILD_RPATH "${MVS_SDK_LIB_DIR}"
    INSTALL_RPATH "\$ORIGIN/../lib/64"
)

set_target_properties(four_camera_capture PROPERTIES
    BUILD_RPATH "${MVS_SDK_LIB_DIR}"
    INSTALL_RPATH "\$ORIGIN/../lib/64"
)
