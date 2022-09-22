# ==============================================================================
# Copyright (C) 2018-2021 Intel Corporation
#
# SPDX-License-Identifier: MIT
# ==============================================================================

set (TARGET_NAME "ffmpeg_openvino_decode_resize_inference")

cmake_minimum_required(VERSION 3.1)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV libavformat libavcodec libswscale libavutil)
find_package(OpenVINO COMPONENTS Runtime)
find_package(OpenCL)
find_package(gflags)

if (LIBAV_FOUND AND OpenVINO_FOUND AND OpenCL_FOUND AND gflags_FOUND)

file (GLOB MAIN_SRC *.cpp)
file (GLOB MAIN_HEADERS *.h)

add_executable(${TARGET_NAME} ${MAIN_SRC} ${MAIN_HEADERS})

# use pkg-config if sample builds as standalone. Otherwise vars DLSTREAMER_INCLUDE_DIRS/etc set by top level cmake
if (${CMAKE_CURRENT_SOURCE_DIR} STREQUAL ${PROJECT_SOURCE_DIR})
    pkg_check_modules(DLSTREAMER dl-streamer REQUIRED)
else()
    target_include_directories(${TARGET_NAME}
            PRIVATE
            ${DLSTREAMER_BASE_DIR}/src/ffmpeg/_plugin
            ${DLSTREAMER_BASE_DIR}/src/vaapi/_plugin
            )
endif()

target_include_directories(${TARGET_NAME}
PRIVATE
        ${DLSTREAMER_INCLUDE_DIRS}
)

target_link_libraries(${TARGET_NAME}
PRIVATE
        dlstreamer_ffmpeg
        dlstreamer_vaapi
        openvino::runtime
        ${LIBAV_LIBRARIES}
        ${OpenCL_LIBRARY}
        gflags)

endif()