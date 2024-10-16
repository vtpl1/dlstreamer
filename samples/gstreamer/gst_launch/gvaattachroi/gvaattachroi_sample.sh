#!/bin/bash
# ==============================================================================
# Copyright (C) 2021-2024 Intel Corporation
#
# SPDX-License-Identifier: MIT
# ==============================================================================

set -euo pipefail

cd "$(dirname "$(realpath "${BASH_SOURCE[0]}")")"

if [ -z "${MODELS_PATH:-}" ]; then
  echo "Error: MODELS_PATH is not set." >&2
  exit 1
else
  echo "MODELS_PATH: $MODELS_PATH"
fi

INPUT=${1:-"https://videos.pexels.com/video-files/1192116/1192116-sd_640_360_30fps.mp4"}
DEVICE=${2:-"CPU"}  # Supported values: CPU, GPU, NPU
OUTPUT=${3:-"file"} # Supported values: file, display, fps, json, display-and-json
ROI_COORDS=${4:-""} # Specifies pixel absolute coordinates of ROI in form: x_top_left,y_top_left,x_bottom_right,y_bottom_right
# If not defined, the roi list file ./roi_list.json will be used

MODEL="yolov8s"
MODEL_PATH="${MODELS_PATH}/public/$MODEL/FP32/$MODEL.xml"

if [[ $INPUT == "/dev/video"* ]]; then
  SOURCE_ELEMENT="v4l2src device=${INPUT}"
elif [[ $INPUT == *"://"* ]]; then
  SOURCE_ELEMENT="urisourcebin buffer-size=4096 uri=${INPUT}"
else
  SOURCE_ELEMENT="filesrc location=${INPUT}"
fi

DECODE_ELEMENT="! decodebin !"
PREPROC_BACKEND="ie"
if [[ "$DEVICE" == "GPU" ]] || [[ "$DEVICE" == "NPU" ]]; then
  DECODE_ELEMENT+=" vapostproc ! video/x-raw(memory:VAMemory) !"
  PREPROC_BACKEND="va-surface-sharing"
fi

if [[ "$OUTPUT" == "file" ]]; then
  FILE=$(basename "${INPUT%.*}")
  rm -f "${FILE}_${DEVICE}.mp4"
  if gst-inspect-1.0 va | grep -q vah264enc; then
    ENCODER="vah264enc"
  elif gst-inspect-1.0 va | grep -q vah264lpenc; then
    ENCODER="vah264lpenc"
  else
    echo "Error - VA-API H.264 encoder not found."
    exit
  fi
  SINK_ELEMENT="gvawatermark ! videoconvertscale ! gvafpscounter ! ${ENCODER} ! h264parse ! mp4mux ! filesink location=${FILE}_${DEVICE}.mp4"
elif [[ "$OUTPUT" == "display" ]] || [[ -z $OUTPUT ]]; then
  SINK_ELEMENT="gvawatermark ! videoconvertscale ! gvafpscounter ! autovideosink sync=false"
elif [[ "$OUTPUT" == "fps" ]]; then
  SINK_ELEMENT="gvafpscounter ! fakesink async=false"
elif [[ "$OUTPUT" == "json" ]]; then
  rm -f output.json
  SINK_ELEMENT="gvametaconvert add-tensor-data=true ! gvametapublish file-format=json-lines file-path=output.json ! fakesink async=false"
elif [[ "$OUTPUT" == "display-and-json" ]]; then
  rm -f output.json
  SINK_ELEMENT="gvawatermark ! gvametaconvert add-tensor-data=true ! gvametapublish file-format=json-lines file-path=output.json ! videoconvert ! gvafpscounter ! autovideosink sync=false"
else
  echo Error wrong value for SINK_ELEMENT parameter
  echo Valid values: "file" - render to file, "display" - render to screen, "fps" - print FPS, "json" - write to output.json, "display-and-json" - render to screen and write to output.json
  exit
fi

ROI_ELEMENT="gvaattachroi"
if [[ -n "$ROI_COORDS" ]]; then
  ROI_ELEMENT="$ROI_ELEMENT roi=$ROI_COORDS !"
else
  ROI_ELEMENT="$ROI_ELEMENT mode=1 file-path=roi_list.json !"
fi

PIPELINE="gst-launch-1.0 $SOURCE_ELEMENT $DECODE_ELEMENT \
$ROI_ELEMENT gvadetect inference-region=1 model=$MODEL_PATH"
PIPELINE="$PIPELINE device=$DEVICE pre-process-backend=$PREPROC_BACKEND ! queue ! \
$SINK_ELEMENT"

echo "${PIPELINE}"
$PIPELINE
