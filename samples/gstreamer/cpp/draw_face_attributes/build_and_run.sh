#!/bin/bash
# ==============================================================================
# Copyright (C) 2020-2024 Intel Corporation
#
# SPDX-License-Identifier: MIT
# ==============================================================================

#If you want to use your own input file, provide an absolute path
FILE=${1:-https://github.com/intel-iot-devkit/sample-videos/raw/master/head-pose-face-detection-female-and-male.mp4}
OUTPUT=${2:-json} # Valid values: display, display-and-json, json, file
DEVICE=${3:-CPU} # Valid devices: CPU, GPU,NPU?
OUTPUT_DIRECTORY=${4:-/home/dlstreamer/} #Path where to copy output.json

if [ -z "${MODELS_PATH:-}" ]; then
  echo "Error: MODELS_PATH is not set." >&2
  exit 1
else
  echo "MODELS_PATH: $MODELS_PATH"
fi

BASE_DIR="$(realpath "$(dirname "$0")")"
BUILD_DIR=$HOME/build

if [ -d "${BUILD_DIR}" ]; then
  rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}" || exit

ln -s /opt/opencv/libopencv_imgproc.so.410 /opt/opencv/libopencv_imgproc.so
ln -s /opt/opencv/libopencv_core.so.410 /opt/opencv/libopencv_core.so

export PKG_CONFIG_PATH=/opt/intel/dlstreamer/gstreamer/lib/pkgconfig:/opt/intel/dlstreamer/lib/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig

if [ -f /etc/lsb-release ]; then
    cmake "${BASE_DIR}"
else
    cmake3 "${BASE_DIR}"
fi

make -j "$(nproc)"

"${BUILD_DIR}"/draw_face_attributes -i "$FILE" -o "$OUTPUT" -d "$DEVICE"
cp ${BUILD_DIR}/output.json $OUTPUT_DIRECTORY
