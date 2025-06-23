gst-launch-1.0 urisourcebin buffer-size=4096 uri=https://github.com/intel-iot-devkit/sample-videos/raw/master/head-pose-face-detection-female-and-male.mp4 ! \
decodebin3 ! gvadetect model=/workspaces/dlstreamer/../thirdparty/models/intel/face-detection-adas-0001/FP32/face-detection-adas-0001.xml device=GPU ! \
queue ! gvaclassify model=/workspaces/dlstreamer/../thirdparty/models/intel/age-gender-recognition-retail-0013/FP32/age-gender-recognition-retail-0013.xml model-proc=./model_proc/age-gender-recognition-retail-0013.json device=GPU ! \
queue ! gvaclassify model=/workspaces/dlstreamer/../thirdparty/models/intel/emotions-recognition-retail-0003/FP32/emotions-recognition-retail-0003.xml model-proc=./model_proc/emotions-recognition-retail-0003.json device=GPU ! \
queue ! gvaclassify model=/workspaces/dlstreamer/../thirdparty/models/intel/facial-landmarks-98-detection-0001/FP32/facial-landmarks-98-detection-0001.xml model-proc=/workspaces/dlstreamer/build/intel64/Debug/samples/model_proc/intel/facial-landmarks-98-detection-0001.json device=GPU ! \
queue ! gvawatermark ! videoconvert ! gvafpscounter ! autovideosink sync=false



gst-launch-1.0 urisourcebin buffer-size=4096 uri=https://github.com/intel-iot-devkit/sample-videos/raw/master/head-pose-face-detection-female-and-male.mp4 ! \
decodebin3 ! gvadetect model=/workspaces/dlstreamer/../thirdparty/models/intel/face-detection-adas-0001/FP32/face-detection-adas-0001.xml device=CPU ! queue ! \
gvaclassify model=/workspaces/dlstreamer/../thirdparty/models/intel/age-gender-recognition-retail-0013/FP32/age-gender-recognition-retail-0013.xml model-proc=./model_proc/age-gender-recognition-retail-0013.json device=CPU ! queue ! \
gvaclassify model=/workspaces/dlstreamer/../thirdparty/models/intel/emotions-recognition-retail-0003/FP32/emotions-recognition-retail-0003.xml model-proc=./model_proc/emotions-recognition-retail-0003.json device=CPU ! queue ! \
gvaclassify model=/workspaces/dlstreamer/../thirdparty/models/intel/landmarks-regression-retail-0009/FP32/landmarks-regression-retail-0009.xml model-proc=./model_proc/landmarks-regression-retail-0009.json device=CPU ! queue ! \
gvawatermark ! videoconvert ! gvafpscounter ! autovideosink sync=false


gst-launch-1.0 urisourcebin buffer-size=4096 uri=https://github.com/intel-iot-devkit/sample-videos/raw/master/head-pose-face-detection-female-and-male.mp4 ! \
decodebin3 ! gvadetect model=/workspaces/dlstreamer/../thirdparty/models/intel/face-detection-adas-0001/FP32/face-detection-adas-0001.xml device=CPU ! queue ! \
gvaclassify model=/workspaces/dlstreamer/../thirdparty/models/intel/age-gender-recognition-retail-0013/FP32/age-gender-recognition-retail-0013.xml model-proc=./model_proc/age-gender-recognition-retail-0013.json device=CPU ! queue ! \
gvaclassify model=/workspaces/dlstreamer/../thirdparty/models/intel/emotions-recognition-retail-0003/FP32/emotions-recognition-retail-0003.xml model-proc=./model_proc/emotions-recognition-retail-0003.json device=CPU ! queue ! \
gvawatermark ! videoconvert ! gvafpscounter ! autovideosink sync=false