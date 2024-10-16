#!/bin/bash
# ==============================================================================
# Copyright (C) 2021-2024 Intel Corporation
#
# SPDX-License-Identifier: MIT
# ==============================================================================
# This sample refers to a video file by Rihard-Clement-Ciprian Diac via Pexels
# (https://www.pexels.com)
# ==============================================================================

set -euo pipefail

if [ -z "${MODELS_PATH:-}" ]; then
  echo "Error: MODELS_PATH is not set." >&2 
  exit 1
else 
  echo "MODELS_PATH: $MODELS_PATH"
fi

MODEL=${1:-"yolox_s"} # Supported values: yolo_all, yolox-tiny, yolox_s, yolov7, yolov8s, yolov8n-obb, yolov8n-seg, yolov9c, yolov10s
DEVICE=${2:-"CPU"}    # Supported values: CPU, GPU, NPU
INPUT=${3:-"https://videos.pexels.com/video-files/1192116/1192116-sd_640_360_30fps.mp4"}
OUTPUT=${4:-"file"}   # Supported values: file, display, fps, json, display-and-json

cd "$(dirname "$0")"

if [[ "$MODEL" == "yolov10s" ]] && [[ "$DEVICE" == "NPU" ]]; then
    echo "Error - No support of Yolov10 for NPU."
    exit
fi

IE_CONFIG=""
if [[ "$MODEL" == "yolov10s" ]] && [[ "$DEVICE" == "GPU" ]]; then
  IE_CONFIG=" ie-config=GPU_DISABLE_WINOGRAD_CONVOLUTION=YES "
fi

declare -A MODEL_PROC_FILES=(
  ["yolox-tiny"]="../../model_proc/public/yolo-x.json"
  ["yolox_s"]="../../model_proc/public/yolo-x.json"
  ["yolov5s"]="../../model_proc/public/yolo-v7.json"
  ["yolov5su"]="../../model_proc/public/yolo-v8.json"
  ["yolov7"]="../../model_proc/public/yolo-v7.json"
  ["yolov8s"]="../../model_proc/public/yolo-v8.json"
  ["yolov9c"]="../../model_proc/public/yolo-v8.json"
  ["yolov8n-obb"]=""
  ["yolov8n-seg"]=""
  ["yolov10s"]=""
)

if ! [[ "${!MODEL_PROC_FILES[*]}" =~ $MODEL ]]; then
  echo "Unsupported model: $MODEL" >&2
  exit 1
fi

MODEL_PROC=""
if ! [ -z ${MODEL_PROC_FILES[$MODEL]} ]; then
  MODEL_PROC=$(realpath "${MODEL_PROC_FILES[$MODEL]}")
fi

cd - 1>/dev/null

MODEL_PATH="${MODELS_PATH}/public/$MODEL/FP32/$MODEL.xml"

# check if model exists in local directory
if [ ! -f $MODEL_PATH ]; then
  echo "Model not found: ${MODEL_PATH}"
  exit 
fi

if [[ "$INPUT" == "/dev/video"* ]]; then
  SOURCE_ELEMENT="v4l2src device=${INPUT}"
elif [[ "$INPUT" == *"://"* ]]; then
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
  if [[ $(gst-inspect-1.0 va | grep vah264enc) ]]; then
    ENCODER="vah264enc"
  elif [[ $(gst-inspect-1.0 va | grep vah264lpenc) ]]; then
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

PIPELINE="gst-launch-1.0 $SOURCE_ELEMENT $DECODE_ELEMENT \
gvadetect model=$MODEL_PATH"
if [[ -n "$MODEL_PROC" ]]; then
  PIPELINE="$PIPELINE model-proc=$MODEL_PROC"
fi
PIPELINE="$PIPELINE device=$DEVICE pre-process-backend=$PREPROC_BACKEND $IE_CONFIG ! queue ! \
$SINK_ELEMENT"

echo "${PIPELINE}"
$PIPELINE
