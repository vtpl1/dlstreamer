import glob

files_base_path = "/workspaces/thirdparty/videos"
# model_path = "/models/vtpl/vc565_416_416_int8.xml"
# model_proc_path = "/models/vtpl/yolo-vx-4cls.json"

# files_base_path = "/videos"
model_path = "/workspaces/thirdparty/models/565_416_416_int8.xml"
model_proc_path = "/workspaces/thirdparty/models/565_416_416_int8.json"


# files = glob.glob(f"{files_base_path}/*.mp4")
files = [
    "/workspaces/thirdparty/videos/sample123456.mp4",
    # "/videos/6.mp4",
    # "/videos/16.mp4",
    # "/videos/1.mp4",
    # "/videos/3.mp4",
    # "/videos/4.mp4",
    # "/videos/12.mp4",
    # "/videos/13.mp4",
    # "/videos/11.mp4",
    # "/videos/7.mp4",
    # "/videos/10.mp4",
    # "/videos/14.mp4",
    # "/videos/5.mp4",
    # "/videos/2.mp4",
    # "/videos/15.mp4",
    # "/videos/8.mp4",
    # "/videos/9.mp4",
    # "/videos/6.mp4",
    # "/videos/16.mp4",
    # "/videos/1.mp4",
    # "/videos/3.mp4",
    # "/videos/4.mp4",
    # "/videos/12.mp4",
    # "/videos/13.mp4",
    # "/videos/11.mp4",
    # "/videos/7.mp4",
]
# print(files)

MAX_GPU_CHANNELS = 1
channel_count = 0
final_str = ""
gpu_batch_size = 4
for file in files:
    channel_count += 1
    if channel_count > MAX_GPU_CHANNELS:
        break
    f = (
        f'filesrc location={file} ! qtdemux ! h264parse ! vaapih264dec ! capsfilter caps="video/x-raw(memory:VASurface),format=NV12" ! queue ! \\\n'
        f"\t\tgvadetect model={model_path} model_proc={model_proc_path} \\\n"
        f"\t\tmodel-instance-id=YoloXS \\\n"
        f"\t\tdevice=GPU pre-process-backend=vaapi-surface-sharing batch-size={gpu_batch_size} nireq=2 ie-config=GPU_THROUGHPUT_STREAMS=2 ! queue ! \\\n"
        f"\t\tgvafpscounter starting-frame=600 ! \\\n"
        f"\t\tgvatrack tracking-type=short-term-imageless ! queue ! \\\n"
        f"\t\tgvametaconvert format=json ! \\\n"
        # f"\t\tvtplmetapublisher channel-id={channel_count} ! \\\n"
        f"\t\tqueue ! \\\n"
        f"\t\tgvawatermark ! \\\n"
        f"\t\tvaapipostproc ! \\\n"
        f"\t\tcapsfilter caps=\"video/x-raw,format=BGRA\" ! \\\n"
        f"\t\tvideoconvert ! \\\n"        
        f"\t\tximagesink async=false sync=false"
    )
    if len(final_str) > 0:
        final_str = f"{final_str} \\\n\t\t{f}"
    else:
        final_str = f

MAX_CPU_CHANNELS = 0
channel_count = 0

for file in files:
    channel_count += 1
    if channel_count > MAX_CPU_CHANNELS:
        break
    f = (
        f'filesrc location={file} ! qtdemux ! h264parse ! vaapih264dec ! capsfilter caps="video/x-raw,format=NV12" ! queue ! \\\n'
        f"\t\tgvadetect model={model_path} model_proc={model_proc_path} \\\n"
        f"\t\tdevice=CPU ! queue ! \\\n"
        f"\t\tgvafpscounter starting-frame=600 ! \\\n"
        f"\t\tgvatrack tracking-type=short-term-imageless ! queue ! \\\n"
        f"\t\tgvametaconvert format=json ! \\\n"
        # f"\t\tvtplmetapublisher channel-id={channel_count} ! \\\n"
        f"\t\tfakesink async=false sync=false"
    )
    if len(final_str) > 0:
        final_str = f"{final_str} \\\n\t\t{f}"
    else:
        final_str = f
gst_launch_str = f"gst-launch-1.0 \\\n\t\t{final_str}"
print(gst_launch_str)
