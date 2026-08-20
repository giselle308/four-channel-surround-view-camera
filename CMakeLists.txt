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

set(MVS_SDK_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/64")
find_library(MVS_CAMERA_CONTROL
    NAMES MvCameraControl
    PATHS "${MVS_SDK_LIB_DIR}"
    NO_DEFAULT_PATH
    REQUIRED
)

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

# 让 build/camera_app 能直接找到随包附带的海康 SDK 运行库。
set_target_properties(camera_app PROPERTIES
    BUILD_RPATH "${MVS_SDK_LIB_DIR}"
    INSTALL_RPATH "\$ORIGIN/../lib/64"
)

