#!/bin/bash
# ==============================================================================
# Copyright (C) 2024 Intel Corporation
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

INPUT=${1:-"https://videos.pexels.com/video-files/1192116/1192116-sd_640_360_30fps.mp4"}
DEVICE_STREAM_12=${2:-"NPU"}    # Supported values: CPU, GPU, NPU
DEVICE_STREAM_34=${3:-"GPU"}    # Supported values: CPU, GPU, NPU
MODEL_1=${4:-"yolov8s"}         # Supported values: yolox-tiny, yolox_s, yolov7, yolov8s, yolov9c
MODEL_2=${5:-"yolov8s"}         # Supported values: yolox-tiny, yolox_s, yolov7, yolov8s, yolov9c
OUTPUT=${6:-"file"}             # Supported values: file, json
GSTVA=${7:-"VA"}                # Supported values: VA, VAAPI

cd "$(dirname "$0")"

### DEVICE ##########################################################################
if [[ "$DEVICE_STREAM_12" != "CPU" ]] && [[ "$DEVICE_STREAM_12" != "GPU" ]] && [[ "$DEVICE_STREAM_12" != "NPU" ]]; then
  echo "Error: Not supported Device for Stream 1 & 2. Supported CPU,GPU,NPU."
  exit
fi
if [[ "$DEVICE_STREAM_34" != "CPU" ]] && [[ "$DEVICE_STREAM_34" != "GPU" ]] && [[ "$DEVICE_STREAM_34" != "NPU" ]]; then
  echo "Error: Not supported Device for Stream 3 & 4. Supported CPU,GPU,NPU."
  exit
fi

### MODELS ##########################################################################
declare -A MODEL_PROC_FILES=(
  ["yolox-tiny"]="../../model_proc/public/yolo-x.json"
  ["yolox_s"]="../../model_proc/public/yolo-x.json"
  ["yolov7"]="../../model_proc/public/yolo-v7.json"
  ["yolov8s"]="../../model_proc/public/yolo-v8.json"
  ["yolov9c"]="../../model_proc/public/yolo-v8.json"
)

if ! [[ "${!MODEL_PROC_FILES[*]}" =~ $MODEL_1 ]]; then
  echo "Unsupported model: $MODEL_1" >&2
  exit 1
fi
if ! [[ "${!MODEL_PROC_FILES[*]}" =~ $MODEL_2 ]]; then
  echo "Unsupported model: $MODEL_2" >&2
  exit 1
fi

#### Model #1
MODEL_1_PROC=""
if ! [ -z ${MODEL_PROC_FILES[$MODEL_1]} ]; then
  MODEL_1_PROC=$(realpath "${MODEL_PROC_FILES[$MODEL_1]}")
fi
MODEL_1_PATH="${MODELS_PATH}/public/$MODEL_1/FP16/$MODEL_1.xml"

#### Model #2
MODEL_2_PROC=""
if ! [ -z ${MODEL_PROC_FILES[$MODEL_2]} ]; then
  MODEL_2_PROC=$(realpath "${MODEL_PROC_FILES[$MODEL_2]}")
fi
MODEL_2_PATH="${MODELS_PATH}/public/$MODEL_2/FP16/$MODEL_2.xml"

# check if model 1 and 2 exist in local directory
if [ ! -f $MODEL_1_PATH ]; then
  echo "Model not found: ${MODEL_1_PATH}"
  exit 
fi
if [ ! -f $MODEL_2_PATH ]; then
  echo "Model not found: ${MODEL_2_PATH}"
  exit 
fi

MODEL_1_PATH_PROC="model=${MODEL_1_PATH} model-proc=${MODEL_1_PROC}"
MODEL_2_PATH_PROC="model=${MODEL_2_PATH} model-proc=${MODEL_2_PROC}"

cd - 1>/dev/null

### INPUT ###########################################################################
if [ ! -f ${INPUT} ]; then
  echo "Error: Input file does not exits ${PWD}/${INPUT}"
  exit
fi
SOURCE_ELEMENT="filesrc location=${INPUT}"

### GSTVA , OUTPUT ##################################################################
if [[ "$GSTVA" != "VA" ]] && [[ "$GSTVA" != "VAAPI" ]]; then 
  echo "Error: Wrong value for GSTVA parameter."
  echo "Valid values: VA, VAAPI"
  exit
fi

### STREAM 1 and 2 ###
# CPU device
DECODE_ELEMENT_STR12="decodebin"
PREPROC_BACKEND_STR12="ie"
# GPU , NPU device
## GST-VA ##
if [[ "$GSTVA" == "VA" ]]; then 
  if [[ "$DEVICE_STREAM_12" == "GPU" ]]; then
    PREPROC_BACKEND_STR12="va-surface-sharing"
  fi
  if [[ "$DEVICE_STREAM_12" == "GPU" ]] || [[ "$DEVICE_STREAM_12" == "NPU" ]]; then
    DECODE_ELEMENT_STR12="decodebin"
    DECODE_ELEMENT_STR12+=" ! vapostproc ! video/x-raw(memory:VAMemory)"
    PREPROC_BACKEND_STR12="${PREPROC_BACKEND_STR12} nireq=4 model-instance-id=inf0"
  fi
fi
## GST-VAAPI ##
if [[ "$GSTVA" == "VAAPI" ]]; then 
  if [[ "$DEVICE_STREAM_12" == "GPU" ]]; then
    PREPROC_BACKEND_STR12="vaapi-surface-sharing"
  fi
  if [[ "$DEVICE_STREAM_12" == "GPU" ]] || [[ "$DEVICE_STREAM_12" == "NPU" ]]; then
    DECODE_ELEMENT_STR12+=" ! vaapipostproc ! video/x-raw(memory:VASurface)"
    PREPROC_BACKEND_STR12="${PREPROC_BACKEND_STR12} nireq=4 model-instance-id=inf0"
  fi
fi

### STREAM 3 and 4 ###
# CPU device
DECODE_ELEMENT_STR34="decodebin"
PREPROC_BACKEND_STR34="ie"
# GPU , NPU device
## GST-VA ##
if [[ "$GSTVA" == "VA" ]]; then 
  if [[ "$DEVICE_STREAM_34" == "GPU" ]]; then
    PREPROC_BACKEND_STR34="va-surface-sharing"
  fi
  if [[ "$DEVICE_STREAM_34" == "GPU" ]] || [[ "$DEVICE_STREAM_34" == "NPU" ]]; then
    DECODE_ELEMENT_STR34="decodebin"
    DECODE_ELEMENT_STR34+=" ! vapostproc ! video/x-raw(memory:VAMemory)"
    PREPROC_BACKEND_STR34="${PREPROC_BACKEND_STR34} nireq=4 model-instance-id=inf1"
  fi
fi
## GST-VAAPI ##
if [[ "$GSTVA" == "VAAPI" ]]; then 
  if [[ "$DEVICE_STREAM_34" == "GPU" ]]; then
    PREPROC_BACKEND_STR34="vaapi-surface-sharing"
  fi
  if [[ "$DEVICE_STREAM_34" == "GPU" ]] || [[ "$DEVICE_STREAM_34" == "NPU" ]]; then
    DECODE_ELEMENT_STR34+=" ! vaapipostproc ! video/x-raw(memory:VASurface)"
    PREPROC_BACKEND_STR34="${PREPROC_BACKEND_STR34} nireq=4 model-instance-id=inf1"
  fi
fi

if [[ "$GSTVA" == "VA" ]]; then
  if [[ $(gst-inspect-1.0 va | grep vah264enc) ]]; then
    ENCODER="vah264enc"
  elif [[ $(gst-inspect-1.0 va | grep vah264lpenc) ]]; then
    ENCODER="vah264lpenc"
  else
    echo "Error - VA-API H.264 encoder not found."
    exit
  fi
  SINK_ELEMENT_BASE="gvawatermark ! videoconvertscale ! gvafpscounter ! ${ENCODER} ! h264parse ! mp4mux ! "
fi
if [[ "$GSTVA" == "VAAPI" ]]; then 
  SINK_ELEMENT_BASE="gvawatermark ! videoconvertscale ! gvafpscounter ! vaapih264enc ! h264parse ! mp4mux ! "
fi

if [[ "$OUTPUT" == "file" ]]; then
  FILE=$(basename "${INPUT%*.*}")
  rm -f ${FILE}_*.mp4
  SINK_ELEMENT_STR_1="${SINK_ELEMENT_BASE} filesink location=${FILE}_${GSTVA}_${DEVICE_STREAM_12}_1.mp4"
  SINK_ELEMENT_STR_2="${SINK_ELEMENT_BASE} filesink location=${FILE}_${GSTVA}_${DEVICE_STREAM_12}_2.mp4"
  SINK_ELEMENT_STR_3="${SINK_ELEMENT_BASE} filesink location=${FILE}_${GSTVA}_${DEVICE_STREAM_34}_3.mp4"
  SINK_ELEMENT_STR_4="${SINK_ELEMENT_BASE} filesink location=${FILE}_${GSTVA}_${DEVICE_STREAM_34}_4.mp4"
elif [[ "$OUTPUT" == "json" ]]; then
  rm -f output.json
  SINK_ELEMENT_BASE="gvametaconvert add-tensor-data=true ! gvametapublish file-format=json-lines"
  SINK_ELEMENT_STR_1="${SINK_ELEMENT_BASE} file-path=output_${GSTVA}_${DEVICE_STREAM_12}_1.json ! fakesink async=false"
  SINK_ELEMENT_STR_2="${SINK_ELEMENT_BASE} file-path=output_${GSTVA}_${DEVICE_STREAM_12}_2.json ! fakesink async=false"
  SINK_ELEMENT_STR_3="${SINK_ELEMENT_BASE} file-path=output_${GSTVA}_${DEVICE_STREAM_34}_3.json ! fakesink async=false"
  SINK_ELEMENT_STR_4="${SINK_ELEMENT_BASE} file-path=output_${GSTVA}_${DEVICE_STREAM_34}_4.json ! fakesink async=false"
else
  echo Error: Wrong value for SINK_ELEMENT parameter
  echo Valid values: "file" - render to file, "json" - write to output.json
  exit
fi

PIPELINE_STREAM_1="${SOURCE_ELEMENT} ! ${DECODE_ELEMENT_STR12} ! gvadetect ${MODEL_1_PATH_PROC} \
device=${DEVICE_STREAM_12} pre-process-backend=${PREPROC_BACKEND_STR12} ! queue ! ${SINK_ELEMENT_STR_1}"

PIPELINE_STREAM_2="${SOURCE_ELEMENT} ! ${DECODE_ELEMENT_STR12} ! gvadetect ${MODEL_1_PATH_PROC} \
device=${DEVICE_STREAM_12} pre-process-backend=${PREPROC_BACKEND_STR12} ! queue ! ${SINK_ELEMENT_STR_2}"

PIPELINE_STREAM_3="${SOURCE_ELEMENT} ! ${DECODE_ELEMENT_STR34} ! gvadetect ${MODEL_2_PATH_PROC} \
device=${DEVICE_STREAM_34} pre-process-backend=${PREPROC_BACKEND_STR34} ! queue ! ${SINK_ELEMENT_STR_3}"

PIPELINE_STREAM_4="${SOURCE_ELEMENT} ! ${DECODE_ELEMENT_STR34} ! gvadetect ${MODEL_2_PATH_PROC} \
device=${DEVICE_STREAM_34} pre-process-backend=${PREPROC_BACKEND_STR34} ! queue ! ${SINK_ELEMENT_STR_4}"

PIPELINE="gst-launch-1.0 \
${PIPELINE_STREAM_1} \
${PIPELINE_STREAM_2} \
${PIPELINE_STREAM_3} \
${PIPELINE_STREAM_4}"

echo "${PIPELINE}"

if [[ "$OUTPUT" == "json" ]]; then
  # Remove each output stream file
  eval "rm -f output_${GSTVA}_${DEVICE_STREAM_12}_1.json"
  eval "rm -f output_${GSTVA}_${DEVICE_STREAM_12}_2.json"
  eval "rm -f output_${GSTVA}_${DEVICE_STREAM_34}_3.json"
  eval "rm -f output_${GSTVA}_${DEVICE_STREAM_34}_4.json"
fi

# Run main multi stream pipeline command
${PIPELINE}

if [[ "$OUTPUT" == "json" ]]; then
  FILE1="${PWD}/output_${GSTVA}_${DEVICE_STREAM_12}_1.json"
  FILE2="${PWD}/output_${GSTVA}_${DEVICE_STREAM_12}_2.json"
  FILE3="${PWD}/output_${GSTVA}_${DEVICE_STREAM_34}_3.json"
  FILE4="${PWD}/output_${GSTVA}_${DEVICE_STREAM_34}_4.json"

  if [ -f ${FILE1} ] && [ -f ${FILE2} ] && [ -f ${FILE3} ] && [ -f ${FILE4} ]; then
    echo "Building output.json file ..."
    eval "rm -f output.json"
    # Remove empty lines from each (per stream) output file
    eval "sed -i '/^$/d' ${FILE1}"
    eval "sed -i '/^$/d' ${FILE2}"
    eval "sed -i '/^$/d' ${FILE3}"
    eval "sed -i '/^$/d' ${FILE4}"
    # Append content of each test output file to the main output.json file
    eval "cat ${FILE1} >> output.json"
    eval "cat ${FILE2} >> output.json"
    eval "cat ${FILE3} >> output.json"
    eval "cat ${FILE4} >> output.json"
  fi
fi