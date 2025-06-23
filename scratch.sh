stat -c "%g" /dev/dri/render* | head -n 1

docker run -it --rm \
--device /dev/dri \
--group-add $(stat -c "%g" /dev/dri/render* | head -n 1) \
--env ZE_ENABLE_ALT_DRIVERS=libze_intel_vpu.so \
--env MODELS_PATH=/home/dlstreamer/models \
intel/dlstreamer:2025.0.1.3-dev-ubuntu22

export DETECTION_MODEL=$MODELS_PATH/intel/person-vehicle-bike-detection-2004/FP16/person-vehicle-bike-detection-2004.xml
export DETECTION_MODEL_PROC=/workspaces/dlstreamer/build/intel64/Debug/samples/model_proc/intel/person-vehicle-bike-detection-2004.json
export VEHICLE_CLASSIFICATION_MODEL=$MODELS_PATH/intel/vehicle-attributes-recognition-barrier-0039/FP16/vehicle-attributes-recognition-barrier-0039.xml
export VEHICLE_CLASSIFICATION_MODEL_PROC=/workspaces/dlstreamer/build/intel64/Debug/samples/model_proc/intel/vehicle-attributes-recognition-barrier-0039.json
export VIDEO_EXAMPLE=$VIDEOS_PATH/20250603_193021.mp4

gst-launch-1.0 \
filesrc location=${VIDEO_EXAMPLE} ! decodebin3 ! \
gvadetect model=${DETECTION_MODEL} model_proc=${DETECTION_MODEL_PROC} device=GPU ! queue ! \
gvaclassify model=${VEHICLE_CLASSIFICATION_MODEL} model-proc=${VEHICLE_CLASSIFICATION_MODEL_PROC} device=GPU object-class=vehicle ! queue ! \
gvawatermark ! videoconvert ! autovideosink sync=false

omz_downloader --name facial-landmarks-98-detection-0001 -o $MODELS_PATH

export DETECTION_MODEL=$MODELS_PATH/intel/facial-landmarks-98-detection-0001/FP16/facial-landmarks-98-detection-0001.xml
export DETECTION_MODEL_PROC=/workspaces/dlstreamer/build/intel64/Debug/samples/model_proc/intel/facial-landmarks-98-detection-0001.json
export VIDEO_EXAMPLE=$VIDEOS_PATH/football_video.mp4

gst-launch-1.0 \
filesrc location=${VIDEO_EXAMPLE} ! decodebin3 ! \
gvaclassify model=${DETECTION_MODEL} model_proc=${DETECTION_MODEL_PROC} device=CPU ! queue ! \
gvawatermark ! videoconvert ! autovideosink sync=false


omz_downloader --name face-detection-adas-0001 -o $MODELS_PATH

export DETECTION_MODEL=$MODELS_PATH/intel/face-detection-adas-0001/FP16/face-detection-adas-0001.xml
export DETECTION_MODEL_PROC=/workspaces/dlstreamer/build/intel64/Debug/samples/model_proc/intel/face-detection-adas-0001.json
export VIDEO_EXAMPLE=$VIDEOS_PATH/football_video.mp4

gst-launch-1.0 \
filesrc location=${VIDEO_EXAMPLE} ! decodebin3 ! \
gvadetect model=${DETECTION_MODEL} model_proc=${DETECTION_MODEL_PROC} device=GPU ! queue ! \
gvawatermark ! videoconvert ! autovideosink sync=false


omz_downloader --name facial-landmarks-35-adas-0002 -o $MODELS_PATH

export DETECTION_MODEL=$MODELS_PATH/intel/facial-landmarks-35-adas-0002/FP16/facial-landmarks-35-adas-0002.xml
export DETECTION_MODEL_PROC=/workspaces/dlstreamer/build/intel64/Debug/samples/model_proc/intel/facial-landmarks-35-adas-0002.json
export VIDEO_EXAMPLE=$VIDEOS_PATH/football_video.mp4

gst-launch-1.0 \
filesrc location=${VIDEO_EXAMPLE} ! decodebin3 ! \
gvadetect model=${DETECTION_MODEL} model_proc=${DETECTION_MODEL_PROC} device=GPU ! queue ! \
gvawatermark ! videoconvert ! autovideosink sync=false


omz_downloader --name face-detection-retail-0005 -o $MODELS_PATH

export DETECTION_MODEL=$MODELS_PATH/intel/face-detection-retail-0005/FP16/face-detection-retail-0005.xml
export DETECTION_MODEL_PROC=/workspaces/dlstreamer/build/intel64/Debug/samples/model_proc/intel/face-detection-retail-0005.json
export VIDEO_EXAMPLE=$VIDEOS_PATH/football_video.mp4

gst-launch-1.0 \
filesrc location=${VIDEO_EXAMPLE} ! decodebin3 ! \
gvadetect model=${DETECTION_MODEL} model_proc=${DETECTION_MODEL_PROC} device=GPU ! queue ! \
gvawatermark ! videoconvert ! autovideosink sync=false



