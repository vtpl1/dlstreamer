/*******************************************************************************
 * Copyright (C) 2018-2024 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "inference_impl.h"

#include "common/post_processor.h"
#include "common/pre_processor_info_parser.hpp"
#include "common/pre_processors.h"
#include "config.h"
#include "gst_allocator_wrapper.h"
#include "gva_base_inference_priv.hpp"
#include "gva_caps.h"
#include "gva_utils.h"
#include "inference_backend/logger.h"
#include "inference_backend/pre_proc.h"
#include "logger_functions.h"
#include "model_proc_provider.h"
#include "region_of_interest.h"
#include "safe_arithmetic.hpp"
#include "scope_guard.h"
#include "utils.h"
#include "video_frame.h"

#include <assert.h>
#include <cmath>
#include <cstring>
#include <exception>
#include <gst/allocators/allocators.h>
#include <map>
#include <memory>
#include <openvino/runtime/properties.hpp>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef ENABLE_VAAPI
#include "vaapi_utils.h"
#endif

using namespace std::placeholders;
using namespace InferenceBackend;

namespace {

inline std::shared_ptr<Allocator> CreateAllocator(const char *const allocator_name) {
    std::shared_ptr<Allocator> allocator;
    if (allocator_name != nullptr) {
        allocator = std::make_shared<GstAllocatorWrapper>(allocator_name);
        GVA_TRACE("GstAllocatorWrapper is created");
    }
    return allocator;
}

inline std::string GstVideoFormatToString(GstVideoFormat formatType) {
    switch (formatType) {
    case GST_VIDEO_FORMAT_RGBA:
        return "RGBA";
    case GST_VIDEO_FORMAT_BGRA:
        return "BGRA";
    case GST_VIDEO_FORMAT_RGBx:
        return "RGBX";
    case GST_VIDEO_FORMAT_BGRx:
        return "BGRX";
    case GST_VIDEO_FORMAT_RGB:
        return "RGB";
    case GST_VIDEO_FORMAT_BGR:
        return "BGR";
    case GST_VIDEO_FORMAT_NV12:
        return "NV12";
    case GST_VIDEO_FORMAT_I420:
        return "I420";
    default:
        return "";
    }
}

ImagePreprocessorType ImagePreprocessorTypeFromString(const std::string &image_preprocessor_name) {
    constexpr std::pair<const char *, ImagePreprocessorType> preprocessor_types[]{
        {"", ImagePreprocessorType::AUTO},
        {"ie", ImagePreprocessorType::IE},
        {"vaapi", ImagePreprocessorType::VAAPI_SYSTEM},
        {"vaapi-surface-sharing", ImagePreprocessorType::VAAPI_SURFACE_SHARING},
        {"va", ImagePreprocessorType::VAAPI_SYSTEM},
        {"va-surface-sharing", ImagePreprocessorType::VAAPI_SURFACE_SHARING},
        {"opencv", ImagePreprocessorType::OPENCV}};

    for (auto &elem : preprocessor_types) {
        if (image_preprocessor_name == elem.first)
            return elem.second;
    }

    throw std::runtime_error("Invalid pre-process-backend property value provided: " + image_preprocessor_name +
                             ". Check element's description for supported property values.");
}

uint32_t GetOptimalBatchSize(const char *device) {
    uint32_t batch_size = 1;
    // if the device has the format GPU.x we assume that these are discrete graphics and choose larger batch
    if (device and std::string(device).find("GPU.") != std::string::npos)
        batch_size = 8;
    return batch_size;
}

InferenceConfig CreateNestedInferenceConfig(GvaBaseInference *gva_base_inference, const std::string &model_file) {
    assert(gva_base_inference && "Expected valid GvaBaseInference");

    InferenceConfig config;
    std::map<std::string, std::string> base;
    std::map<std::string, std::string> inference = Utils::stringToMap(gva_base_inference->ie_config);

    base[KEY_MODEL] = model_file;
    base[KEY_NIREQ] = std::to_string(gva_base_inference->nireq);
    if (gva_base_inference->device != nullptr) {
        std::string device = gva_base_inference->device;
        base[KEY_DEVICE] = device;

        // Map legacy OV1 inference engine params to OV2 properties to keep backward compatibility:
        if (device == "CPU") {
            if (inference.find(KEY_CPU_THROUGHPUT_STREAMS) != inference.end()) {
                inference[ov::num_streams.name()] = inference[KEY_CPU_THROUGHPUT_STREAMS];
                inference.erase(KEY_CPU_THROUGHPUT_STREAMS);
                GVA_WARNING("Legacy setting detected 'ie-config=%s=x', use 'ie-config=%s=x' instead",
                            KEY_CPU_THROUGHPUT_STREAMS, ov::num_streams.name());
            }
            if (inference.find(ov::num_streams.name()) == inference.end()) {
                inference[ov::num_streams.name()] =
                    (gva_base_inference->cpu_streams == 0) ? "-1" : std::to_string(gva_base_inference->cpu_streams);
            }
            if (inference.find("CPU_THREADS_NUM") != inference.end()) {
                inference[ov::inference_num_threads.name()] = inference["CPU_THREADS_NUM"];
                inference.erase("CPU_THREADS_NUM");
                GVA_WARNING("Legacy setting detected 'ie-config=CPU_THREADS_NUM=x', use 'ie-config=%s=x' instead",
                            ov::inference_num_threads.name());
            }
            if (inference.find("CPU_BIND_THREAD") != inference.end()) {
                inference[ov::hint::enable_cpu_pinning.name()] = (inference["CPU_BIND_THREAD"] == "YES") ? "1" : "0";
                inference.erase("CPU_BIND_THREAD");
                GVA_WARNING("Legacy setting detected 'ie-config=CPU_BIND_THREAD=x', use 'ie-config=%s=x' instead",
                            ov::hint::enable_cpu_pinning.name());
            }
        }
        if (device.find("GPU") != std::string::npos) {
            if (inference.find(KEY_GPU_THROUGHPUT_STREAMS) != inference.end()) {
                inference[ov::num_streams.name()] = inference[KEY_GPU_THROUGHPUT_STREAMS];
                inference.erase(KEY_GPU_THROUGHPUT_STREAMS);
                GVA_WARNING("Legacy setting detected 'ie-config=%s=x', use 'ie-config=%s=x' instead",
                            KEY_GPU_THROUGHPUT_STREAMS, ov::num_streams.name());
            }
            if (inference.find(ov::num_streams.name()) == inference.end()) {
                inference[ov::num_streams.name()] =
                    (gva_base_inference->gpu_streams == 0) ? "-1" : std::to_string(gva_base_inference->gpu_streams);
            }
        }
    }

    base[KEY_PRE_PROCESSOR_TYPE] =
        std::to_string(static_cast<int>(ImagePreprocessorTypeFromString(gva_base_inference->pre_proc_type)));
    base[KEY_IMAGE_FORMAT] =
        GstVideoFormatToString(static_cast<GstVideoFormat>(gva_base_inference->info->finfo->format));

    const uint32_t batch = gva_base_inference->batch_size;
    base[KEY_BATCH_SIZE] = std::to_string(batch);
    base[KEY_RESHAPE] = std::to_string(gva_base_inference->reshape);
    if (gva_base_inference->reshape) {
        if ((gva_base_inference->reshape_width) || (gva_base_inference->reshape_height) || (batch > 1)) {
            base[KEY_RESHAPE_WIDTH] = std::to_string(gva_base_inference->reshape_width);
            base[KEY_RESHAPE_HEIGHT] = std::to_string(gva_base_inference->reshape_height);
        } else {
            base[KEY_RESHAPE_WIDTH] = std::to_string(gva_base_inference->info->width);
            base[KEY_RESHAPE_HEIGHT] = std::to_string(gva_base_inference->info->height);
        }
    }
    base[KEY_CAPS_FEATURE] = std::to_string(static_cast<int>(gva_base_inference->caps_feature));

    config[KEY_BASE] = base;
    config[KEY_INFERENCE] = inference;
    config[KEY_PRE_PROCESSOR] = Utils::stringToMap(gva_base_inference->pre_proc_config);

    return config;
}
/*
bool DoesModelProcDefinePreProcessing(const std::vector<ModelInputProcessorInfo::Ptr> &model_input_processor_info) {
    for (const auto &it : model_input_processor_info) {
        if (!it || it->format != "image")
            continue;
        auto input_desc = PreProcParamsParser(it->params).parse();
        if (input_desc && input_desc->isDefined())
            return true;
    }
    return false;
}
*/
bool IsModelProcSupportedForIE(const std::vector<ModelInputProcessorInfo::Ptr> &model_input_processor_info,
                               GstVideoInfo *input_video_info) {
    auto format = dlstreamer::gst_format_to_video_format(GST_VIDEO_INFO_FORMAT(input_video_info));
    for (const auto &it : model_input_processor_info) {
        if (!it || it->format != "image")
            continue;
        auto input_desc = PreProcParamsParser(it->params).parse();
        if (input_desc &&
            (input_desc->doNeedDistribNormalization() || input_desc->doNeedCrop() || input_desc->doNeedPadding() ||
             input_desc->doNeedColorSpaceConversion(static_cast<int>(format))))
            return false;
    }
    return true;
}

bool IsModelProcSupportedForVaapi(const std::vector<ModelInputProcessorInfo::Ptr> &model_input_processor_info,
                                  GstVideoInfo *input_video_info) {
    auto format = dlstreamer::gst_format_to_video_format(GST_VIDEO_INFO_FORMAT(input_video_info));
    for (const auto &it : model_input_processor_info) {
        if (!it || it->format != "image")
            continue;
        auto input_desc = PreProcParamsParser(it->params).parse();
        // In these cases we need to switch to opencv preproc
        // VAAPI converts color to RGBP by default
        if (input_desc && (input_desc->doNeedDistribNormalization() ||
                           (input_desc->getTargetColorSpace() != PreProcColorSpace::BGR &&
                            input_desc->doNeedColorSpaceConversion(static_cast<int>(format)))))
            return false;
    }
    return true;
}

bool IsModelProcSupportedForVaapiSurfaceSharing(
    const std::vector<ModelInputProcessorInfo::Ptr> &model_input_processor_info, GstVideoInfo *input_video_info) {
    auto format = dlstreamer::gst_format_to_video_format(GST_VIDEO_INFO_FORMAT(input_video_info));
    for (const auto &it : model_input_processor_info) {
        if (!it || it->format != "image")
            continue;
        auto input_desc = PreProcParamsParser(it->params).parse();
        if (input_desc && (input_desc->doNeedDistribNormalization() ||
                           (input_desc->getTargetColorSpace() != PreProcColorSpace::BGR &&
                            input_desc->doNeedColorSpaceConversion(static_cast<int>(format)))))
            return false;
    }
    return true;
}

bool IsPreprocSupported(ImagePreprocessorType preproc,
                        const std::vector<ModelInputProcessorInfo::Ptr> &model_input_processor_info,
                        GstVideoInfo *input_video_info, const std::string device) {
    bool isNpu = (device.find("NPU") != std::string::npos);
    switch (preproc) {
    case ImagePreprocessorType::IE:
        return !isNpu && IsModelProcSupportedForIE(model_input_processor_info, input_video_info);
    case ImagePreprocessorType::VAAPI_SYSTEM:
        return IsModelProcSupportedForVaapi(model_input_processor_info, input_video_info);
    case ImagePreprocessorType::VAAPI_SURFACE_SHARING:
        return !isNpu && IsModelProcSupportedForVaapiSurfaceSharing(model_input_processor_info, input_video_info);
    case ImagePreprocessorType::OPENCV:
        return true;
    case ImagePreprocessorType::AUTO:
    default:
        return false;
    }
}

// Returns default suitable preprocessor according to caps and custom preprocessing options
ImagePreprocessorType
GetPreferredImagePreproc(CapsFeature caps, const std::vector<ModelInputProcessorInfo::Ptr> &model_input_processor_info,
                         GstVideoInfo *input_video_info, const std::string device) {
    ImagePreprocessorType result = ImagePreprocessorType::OPENCV;
    switch (caps) {
    case SYSTEM_MEMORY_CAPS_FEATURE:
        result = ImagePreprocessorType::IE;
        break;
    case VA_SURFACE_CAPS_FEATURE:
    case VA_MEMORY_CAPS_FEATURE:
        result = ImagePreprocessorType::VAAPI_SYSTEM;
        break;
    case DMA_BUF_CAPS_FEATURE:
#ifdef ENABLE_VPUX
        result = ImagePreprocessorType::IE;
#else
        result = ImagePreprocessorType::VAAPI_SYSTEM;
#endif
        break;
    default:
        throw std::runtime_error("Unsupported caps have been detected for image preprocessor!");
    }

    // Fallback to OPENCV
    if (!IsPreprocSupported(result, model_input_processor_info, input_video_info, device)) {
        result = ImagePreprocessorType::OPENCV;
    }

    return result;
}

void SetPreprocessor(InferenceConfig &config,
                     const std::vector<ModelInputProcessorInfo::Ptr> &model_input_processor_info,
                     GstVideoInfo *input_video_info) {
    const auto caps = static_cast<CapsFeature>(std::stoi(config[KEY_BASE][KEY_CAPS_FEATURE]));
    const auto current = static_cast<ImagePreprocessorType>(std::stoi(config[KEY_BASE][KEY_PRE_PROCESSOR_TYPE]));

    // Select preprocessor
    if (current == ImagePreprocessorType::AUTO) {
        const auto preferred =
            GetPreferredImagePreproc(caps, model_input_processor_info, input_video_info, config[KEY_BASE][KEY_DEVICE]);
        config[KEY_BASE][KEY_PRE_PROCESSOR_TYPE] = std::to_string(static_cast<int>(preferred));
    } else if (!IsPreprocSupported(current, model_input_processor_info, input_video_info,
                                   config[KEY_BASE][KEY_DEVICE])) {
        if (current == ImagePreprocessorType::IE &&
            IsPreprocSupported(
                ImagePreprocessorType::OPENCV, model_input_processor_info, input_video_info,
                config[KEY_BASE][KEY_DEVICE])) { // if pre-processing params not supported by IE, change to OpenCV
            config[KEY_BASE][KEY_PRE_PROCESSOR_TYPE] = std::to_string(static_cast<int>(ImagePreprocessorType::OPENCV));
            GVA_WARNING("'pre-process-backend=ie' not supported with current settings, falling back to "
                        "'pre-process-backend=opencv'");
        } else if (current == ImagePreprocessorType::VAAPI_SURFACE_SHARING &&
                   IsPreprocSupported(ImagePreprocessorType::VAAPI_SYSTEM, model_input_processor_info, input_video_info,
                                      config[KEY_BASE][KEY_DEVICE])) {
            config[KEY_BASE][KEY_PRE_PROCESSOR_TYPE] =
                std::to_string(static_cast<int>(ImagePreprocessorType::VAAPI_SYSTEM));
            GVA_WARNING(
                "'pre-process-backend=vaapi-surface-sharing' not supported with current settings, falling back to "
                "'pre-process-backend=vaapi'");
        } else {
            throw std::runtime_error(
                "Specified pre-process-backend can not be chosen due to unsupported operations defined in model-proc. "
                "If you want to use it, please remove inappropriate parameters for desired pre-process-backend.");
        }
    }
}

std::string three_doubles_to_str(const std::array<double, 3> &v) {
    std::string result = std::to_string(v[0]);
    if (v[1] != v[0] || v[2] != v[0]) {
        result += " " + std::to_string(v[1]) + " " + std::to_string(v[2]);
    }
    return result;
}

void UpdateConfigWithLayerInfo(const std::vector<ModelInputProcessorInfo::Ptr> &model_input_processor_info,
                               std::map<std::string, std::map<std::string, std::string>> &config) {
    std::map<std::string, std::string> input_layer_precision;
    std::map<std::string, std::string> input_format;
    for (const ModelInputProcessorInfo::Ptr &preproc : model_input_processor_info) {
        if (!preproc->precision.empty())
            input_layer_precision[preproc->layer_name] = preproc->precision;
        if (!preproc->format.empty())
            input_format[preproc->layer_name] = preproc->format;
    }

    config[KEY_INPUT_LAYER_PRECISION] = input_layer_precision;
    config[KEY_FORMAT] = input_format;

    for (const auto &it : model_input_processor_info) {
        if (!it || it->format != "image")
            continue;
        assert(it->precision == "U8");
        auto input_desc = PreProcParamsParser(it->params).parse();

        // It is clearer to composition arbitrary set of pixel value transformations as affine transformations like v' =
        // v*affine_multiply + affine_add than to composition them as v' = (v-mean)/std However, this involves
        // conversion to OpenCV format
        double affine_multiply = 1.0;
        double affine_add = 0.0;
        bool had_range_or_scale = false;
        if (input_desc && input_desc->doNeedRangeNormalization()) {
            const auto &range = input_desc->getRangeNormalization();
            affine_multiply = (range.max - range.min) / 255.0;
            affine_add += range.min;
            had_range_or_scale = true;
        }
        double scale = 0;
        if (gst_structure_get_double(it->params, "scale", &scale)) {
            affine_multiply /= scale;
            affine_add /= scale;
            had_range_or_scale = true;
        }
        std::array<double, 3> affine_add_3 = {affine_add, affine_add, affine_add};
        std::array<double, 3> affine_multiply_3 = {affine_multiply, affine_multiply, affine_multiply};

        if (input_desc && input_desc->doNeedDistribNormalization()) {

            // If no range nor scale are given but distrib normalization is specified, normalize values to 0..1 range so
            // that distrib normalization works same as in Pytorch and matches what one would expect from our own
            // documentation
            if (!had_range_or_scale)
                affine_multiply /= 255.0;

            // mean and std works as
            // v ' = (v-mean)/std
            // v ' = (v-mean) * 1/std
            // v ' = v * 1/std + (-mean/std)
            // multiplier = 1/std
            // add = (-mean/std)
            auto norm = input_desc->getDistribNormalization();
            for (int i = 0; i < 3; ++i) {
                affine_multiply_3[i] /= norm.std[i];
                affine_add_3[i] -= norm.mean[i] / norm.std[i];
            }
        }
        std::array<double, 3> mean, std_dev;
        // multiplier = 1/std
        // std = 1/multiplier
        // add = -mean/std
        // -add*std = mean
        for (int i = 0; i < 3; ++i) {
            std_dev[i] = 1.0 / affine_multiply_3[i];
            mean[i] = -affine_add_3[i] * std_dev[i];
        }
        if (std_dev != std::array<double, 3>({1.0, 1.0, 1.0})) {
            config[KEY_BASE][KEY_PIXEL_VALUE_SCALE] = three_doubles_to_str(std_dev);
        }
        if (mean != std::array<double, 3>({0.0, 0.0, 0.0})) {
            config[KEY_BASE][KEY_PIXEL_VALUE_MEAN] = three_doubles_to_str(mean);
        }

        int reverse_channels = 0; // TODO: verify that channel reversal works correctly with mean and std!
        if (gst_structure_get_int(it->params, "reverse_input_channels", &reverse_channels)) {
            config[KEY_BASE][KEY_MODEL_FORMAT] = reverse_channels ? "RGB" : "BGR";
        }
    }
}

void ApplyImageBoundaries(std::shared_ptr<InferenceBackend::Image> &image, GstVideoRegionOfInterestMeta *meta,
                          InferenceRegionType inference_region) {
    if (!meta) {
        throw std::invalid_argument("Region of interest meta is null.");
    }
    if (inference_region == FULL_FRAME) {
        image->rect = Rectangle<uint32_t>(meta->x, meta->y, meta->w, meta->h);
        return;
    }

    const auto image_width = image->width;
    const auto image_height = image->height;

    GVA::RegionOfInterest roi(meta);
    const GVA::Rect<double> normalized_bbox = roi.normalized_rect();

    const constexpr double zero = 0;
    const GVA::Rect<uint32_t> raw_coordinates = {
        .x = static_cast<uint32_t>((std::max(round(normalized_bbox.x * image_width), zero))),
        .y = static_cast<uint32_t>((std::max(round(normalized_bbox.y * image_height), zero))),
        .w = static_cast<uint32_t>((std::max(round(normalized_bbox.w * image_width), zero))),
        .h = static_cast<uint32_t>((std::max(round(normalized_bbox.h * image_height), zero)))};

    image->rect.x = std::min(raw_coordinates.x, image_width);
    image->rect.y = std::min(raw_coordinates.y, image_height);
    image->rect.width = (safe_add(raw_coordinates.w, raw_coordinates.x) > image_width) ? image_width - image->rect.x
                                                                                       : raw_coordinates.w;
    image->rect.height = (safe_add(raw_coordinates.h, raw_coordinates.y) > image_height) ? image_height - image->rect.y
                                                                                         : raw_coordinates.h;
}

void UpdateClassificationHistory(GstVideoRegionOfInterestMeta *meta, GvaBaseInference *gva_base_inference,
                                 const GstStructure *classification_result) {
    if (gva_base_inference->type != GST_GVA_CLASSIFY_TYPE)
        return;

    GstGvaClassify *gvaclassify = GST_GVA_CLASSIFY(gva_base_inference);
    gint meta_id = 0;
    get_object_id(meta, &meta_id);
    if (gvaclassify->reclassify_interval != 1 and meta_id > 0)
        gvaclassify->classification_history->UpdateROIParams(meta_id, classification_result);
}

MemoryType GetMemoryType(CapsFeature caps_feature) {
    switch (caps_feature) {
    case CapsFeature::SYSTEM_MEMORY_CAPS_FEATURE:
        return MemoryType::SYSTEM;
    case CapsFeature::DMA_BUF_CAPS_FEATURE:
#ifdef ENABLE_VPUX
        return MemoryType::SYSTEM;
#else
        return MemoryType::DMA_BUFFER;
#endif
    case CapsFeature::VA_SURFACE_CAPS_FEATURE:
    case CapsFeature::VA_MEMORY_CAPS_FEATURE:
        return MemoryType::VAAPI;
    case CapsFeature::ANY_CAPS_FEATURE:
    default:
        return MemoryType::ANY;
    }
}

MemoryType GetMemoryType(MemoryType input_image_memory_type, ImagePreprocessorType image_preprocessor_type) {
    MemoryType type = MemoryType::ANY;
    switch (input_image_memory_type) {
    case MemoryType::SYSTEM: {
        switch (image_preprocessor_type) {
        case ImagePreprocessorType::OPENCV:
        case ImagePreprocessorType::IE:
            type = MemoryType::SYSTEM;
            break;
        default:
            throw std::invalid_argument("For system memory only supports ie, opencv image preprocessors");
        }
        break;
    }
    case MemoryType::VAAPI:
    case MemoryType::DMA_BUFFER: {
        switch (image_preprocessor_type) {
        case ImagePreprocessorType::OPENCV:
        case ImagePreprocessorType::IE:
            type = MemoryType::SYSTEM;
            break;
        case ImagePreprocessorType::VAAPI_SURFACE_SHARING:
        case ImagePreprocessorType::VAAPI_SYSTEM:
            type = input_image_memory_type;
            break;
        default:
            throw std::invalid_argument("Invalid image preprocessor type");
        }
        break;
    }
    default:
        break;
    }
    return type;
}

bool canReuseSharedVADispCtx(GvaBaseInference *gva_base_inference) {
    const std::string device(gva_base_inference->device);

    if (device.find("GPU.") == device.npos && device.find("GPU") != device.npos) {
        // GPU only i.e. all available accelerators
        return true;
    }

    // Add check for GPU.x <--> va(renderDXXX)h264dec , va(renderDXXX)postproc
    {
        // TODO
        // if (device.find("GPU.") != device.npos) { }
        // Reuse shared VADisplay Context if GPU.x == va(renderDXXX)h264dec && va(renderDXXX)postproc
    }

    return false;
}

dlstreamer::ContextPtr createVaDisplay(GvaBaseInference *gva_base_inference) {
    assert(gva_base_inference);

    auto display = gva_base_inference->priv->va_display;
    const std::string device(gva_base_inference->device);

    // Create a new VADisplay context only if the existing one i.e priv->va_display does not match
    if (!canReuseSharedVADispCtx(gva_base_inference)) {
        if (device.find("GPU.") != device.npos) {
            uint32_t rel_dev_index = 0;
            rel_dev_index = Utils::getRelativeGpuDeviceIndex(device);
            display = vaApiCreateVaDisplay(rel_dev_index);

            GVA_INFO("Using new VADisplay (%p) ", static_cast<void *>(display.get()));
            return display;
        }
    }

    if (display) {
        GVA_INFO("Using shared VADisplay (%p) from element %s", static_cast<void *>(display.get()),
                 GST_ELEMENT_NAME(gva_base_inference));
    }

    return display;
}

} // namespace

InferenceImpl::Model InferenceImpl::CreateModel(GvaBaseInference *gva_base_inference, const std::string &model_file,
                                                const std::string &model_proc_path, const std::string &labels_str) {
    assert(gva_base_inference && "Expected a valid pointer to GvaBaseInference");

    if (!Utils::fileExists(model_file))
        throw std::invalid_argument("ERROR: model file '" + model_file + "' does not exist");

    if (Utils::symLink(model_file))
        throw std::invalid_argument("ERROR: model file '" + model_file + "' is a symbolic link");

    Model model;

    if (!model_proc_path.empty()) {
        const constexpr size_t MAX_MODEL_PROC_SIZE = 10 * 1024 * 1024; // 10 Mb
        if (!Utils::CheckFileSize(model_proc_path, MAX_MODEL_PROC_SIZE))
            throw std::invalid_argument("ERROR: model-proc file '" + model_proc_path +
                                        "' size exceeds the allowable size (10 MB).");
        if (Utils::symLink(model_proc_path))
            throw std::invalid_argument("ERROR: model-proc file '" + model_proc_path + "' is a symbolic link");

        ModelProcProvider model_proc_provider;
        model_proc_provider.readJsonFile(model_proc_path);
        model.input_processor_info = model_proc_provider.parseInputPreproc();
        model.output_processor_info = model_proc_provider.parseOutputPostproc();
    } else {
        // use model metadata file to construct preprocessing info
        model.input_processor_info =
            ModelProcProvider::parseInputPreproc(ImageInference::GetModelInfoPreproc(model_file));
    }

    if (Utils::symLink(labels_str))
        throw std::invalid_argument("ERROR: labels-file '" + labels_str + "' is a symbolic link");

    // It will be parsed in PostProcessor
    model.labels = labels_str;

    if (gva_base_inference->batch_size == 0)
        gva_base_inference->batch_size = GetOptimalBatchSize(gva_base_inference->device);

    UpdateModelReshapeInfo(gva_base_inference);
    InferenceConfig ie_config = CreateNestedInferenceConfig(gva_base_inference, model_file);
    UpdateConfigWithLayerInfo(model.input_processor_info, ie_config);
    SetPreprocessor(ie_config, model.input_processor_info, gva_base_inference->info);
    memory_type =
        GetMemoryType(GetMemoryType(static_cast<CapsFeature>(std::stoi(ie_config[KEY_BASE][KEY_CAPS_FEATURE]))),
                      static_cast<ImagePreprocessorType>(std::stoi(ie_config[KEY_BASE][KEY_PRE_PROCESSOR_TYPE])));

    dlstreamer::ContextPtr va_dpy;
    if (memory_type == MemoryType::VAAPI || memory_type == MemoryType::DMA_BUFFER) {
        va_dpy = createVaDisplay(gva_base_inference);

        // Modify IE config for surface sharing
        if (static_cast<ImagePreprocessorType>(std::stoi(ie_config[KEY_BASE][KEY_PRE_PROCESSOR_TYPE])) ==
            ImagePreprocessorType::VAAPI_SURFACE_SHARING) {
            // ie_config[KEY_INFERENCE][InferenceEngine::GPUConfigParams::KEY_GPU_NV12_TWO_INPUTS] =
            //    InferenceEngine::PluginConfigParams::YES;
            if (!ie_config[KEY_BASE][KEY_IMAGE_FORMAT].compare("I420")) {
                // I420 source pads are converted internally to NV12 tensors by vaapi-surface-sharing pre-processor
                GVA_INFO("Overwrite input tensor format to NV12");
                ie_config[KEY_BASE][KEY_IMAGE_FORMAT] = "NV12";
            }
        }
    }

    if (gva_base_inference->inference_region == FULL_FRAME) {
        ie_config[KEY_BASE]["img-width"] = std::to_string(gva_base_inference->info->width);
        ie_config[KEY_BASE]["img-height"] = std::to_string(gva_base_inference->info->height);
    } else {
        ie_config[KEY_BASE]["img-width"] = "0";
        ie_config[KEY_BASE]["img-height"] = "0";
        ie_config[KEY_BASE]["frame-width"] = std::to_string(gva_base_inference->info->width);
        ie_config[KEY_BASE]["frame-height"] = std::to_string(gva_base_inference->info->height);
    }

    auto image_inference = ImageInference::make_shared(
        memory_type, ie_config, allocator.get(), std::bind(&InferenceImpl::InferenceCompletionCallback, this, _1, _2),
        std::bind(&InferenceImpl::PushFramesIfInferenceFailed, this, _1), std::move(va_dpy));
    if (!image_inference)
        throw std::runtime_error("Failed to create inference instance");
    model.inference = image_inference;
    model.name = image_inference->GetModelName();

    return model;
}

InferenceImpl::InferenceImpl(GvaBaseInference *gva_base_inference) {
    assert(gva_base_inference != nullptr && "Expected a valid pointer to gva_base_inference");
    if (!gva_base_inference->model) {
        throw std::runtime_error("Model not specified");
    }
    std::string model_file(gva_base_inference->model);

    std::string model_proc;
    if (gva_base_inference->model_proc) {
        model_proc = gva_base_inference->model_proc;
    }

    std::string labels_str;
    if (gva_base_inference->labels) {
        labels_str = gva_base_inference->labels;
    }

    allocator = CreateAllocator(gva_base_inference->allocator_name);

    GVA_INFO("Loading model: device=%s, path=%s", std::string(gva_base_inference->device).c_str(), model_file.c_str());
    GVA_INFO("Initial settings: batch_size=%u, nireq=%u", gva_base_inference->batch_size, gva_base_inference->nireq);
    this->model = CreateModel(gva_base_inference, model_file, model_proc, labels_str);
}

dlstreamer::ContextPtr InferenceImpl::GetDisplay(GvaBaseInference *gva_base_inference) {
    return gva_base_inference->priv->va_display;
}
void InferenceImpl::SetDisplay(GvaBaseInference *gva_base_inference, const dlstreamer::ContextPtr &display) {
    gva_base_inference->priv->va_display = display;
}

void InferenceImpl::FlushInference() {
    model.inference->Flush();
}

void InferenceImpl::FlushOutputs() {
    PushOutput();
}

void InferenceImpl::UpdateObjectClasses(const gchar *obj_classes_str) {
    // Lock mutex to avoid data race in case of shared inference instance in multichannel mode
    std::unique_lock<std::mutex> lock(_mutex);

    if (obj_classes_str && obj_classes_str[0])
        object_classes = Utils::splitString(obj_classes_str, ',');
    else
        object_classes.clear();
}

void InferenceImpl::UpdateModelReshapeInfo(GvaBaseInference *gva_base_inference) {
    try {
        if (gva_base_inference->reshape)
            return;

        if (gva_base_inference->reshape_width || gva_base_inference->reshape_height) {
            GVA_WARNING("reshape switched to TRUE because reshape-width (%u) or reshape-height (%u) is non-zero",
                        gva_base_inference->reshape_width, gva_base_inference->reshape_height);
            gva_base_inference->reshape = true;
            return;
        }

        if (gva_base_inference->batch_size > 1) {
            GVA_WARNING("reshape switched to TRUE because batch-size (%u) is greater than one",
                        gva_base_inference->batch_size);
            gva_base_inference->reshape = true;
            return;
        }
    } catch (const std::exception &e) {
        std::throw_with_nested(std::runtime_error("Failed to update reshape"));
    }
}

bool InferenceImpl::FilterObjectClass(GstVideoRegionOfInterestMeta *roi) const {
    if (object_classes.empty())
        return true;
    auto compare_quark_string = [roi](const std::string &str) {
        const gchar *roi_type = roi->roi_type ? g_quark_to_string(roi->roi_type) : "";
        return (strcmp(roi_type, str.c_str()) == 0);
    };
    return std::find_if(object_classes.cbegin(), object_classes.cend(), compare_quark_string) != object_classes.cend();
}

bool InferenceImpl::FilterObjectClass(const std::string &object_class) const {
    if (object_classes.empty())
        return true;
    return std::find(object_classes.cbegin(), object_classes.cend(), object_class) != object_classes.cend();
}

InferenceImpl::~InferenceImpl() {
    for (auto proc : model.output_processor_info)
        gst_structure_free(proc.second);
}

bool InferenceImpl::IsRoiSizeValid(const GstVideoRegionOfInterestMeta *roi_meta) {
    return roi_meta->w > 1 && roi_meta->h > 1;
}

/**
 * Acquires output_frames_mutex with std::lock_guard.
 */
void InferenceImpl::PushOutput() {
    ITT_TASK(__FUNCTION__);
    std::lock_guard<std::mutex> guard(output_frames_mutex);

    // track output queues that are full
    std::map<std::string, bool> output_full;

    auto frame = output_frames.begin();
    while (frame != output_frames.end()) {
        if ((*frame).inference_count != 0) {
            break; // inference not completed yet
        }

        for (const std::shared_ptr<InferenceFrame> &inference_roi : (*frame).inference_rois) {
            for (const GstStructure *roi_classification : inference_roi->roi_classifications) {
                UpdateClassificationHistory(&inference_roi->roi, (*frame).filter, roi_classification);
            }
        }

        // 'output_frames' queue can be shared across streams and it is subject to HOL blocking
        // do not send frame to a blocked output, but check if there frames ready to non-blocked outputs
        GstObject *src = &(*frame).filter->base_transform.element.object;
        if (CheckSrcPadBlocked(src) || output_full[src->name]) {
            output_full[src->name] = true;
            frame++; // output blocked, try next frame
        } else {
            PushBufferToSrcPad(*frame);
            frame = output_frames.erase(frame);
        }
    }
}

bool InferenceImpl::CheckSrcPadBlocked(GstObject *src) {
    bool blocked = false;

    GstObject *dst = gst_pad_get_parent(gst_pad_get_peer(GST_BASE_TRANSFORM_SRC_PAD(src)));
    if (dst == nullptr)
        return false;

    if (strcmp(dst->name, "queue") > 0) {
        guint buf_cnt;
        g_object_get(dst, "current-level-buffers", &buf_cnt, NULL);
        GstState state, pending;
        gst_element_get_state(GST_ELEMENT(dst), &state, &pending, GST_CLOCK_TIME_NONE);

        if ((buf_cnt > 1) && (state == GST_STATE_PAUSED)) {
            blocked = true;
        }
    }
    gst_object_unref(dst);

    return blocked;
}

void InferenceImpl::PushBufferToSrcPad(OutputFrame &output_frame) {
    GstBuffer *buffer = output_frame.buffer;

    if (!check_gva_base_inference_stopped(output_frame.filter)) {
        GstFlowReturn ret = gst_pad_push(GST_BASE_TRANSFORM_SRC_PAD(output_frame.filter), buffer);
        if (ret != GST_FLOW_OK) {
            GVA_WARNING("Inference gst_pad_push returned status: %d", ret);
        }
    }
}

std::shared_ptr<InferenceImpl::InferenceResult>
InferenceImpl::MakeInferenceResult(GvaBaseInference *gva_base_inference, Model &model,
                                   GstVideoRegionOfInterestMeta *meta, std::shared_ptr<InferenceBackend::Image> &image,
                                   GstBuffer *buffer) {
    auto result = std::make_shared<InferenceResult>();
    /* expect that std::make_shared must throw instead of returning nullptr */
    assert(result.get() != nullptr && "Expected a valid InferenceResult");

    result->inference_frame = std::make_shared<InferenceFrame>();
    /* expect that std::make_shared must throw instead of returning nullptr */
    assert(result->inference_frame.get() != nullptr && "Expected a valid InferenceFrame");

    result->inference_frame->buffer = buffer;
    result->inference_frame->roi = *meta;
    result->inference_frame->gva_base_inference = gva_base_inference;
    if (gva_base_inference->info)
        result->inference_frame->info = gst_video_info_copy(gva_base_inference->info);

    result->model = &model;
    result->image = image;
    return result;
}

GstFlowReturn InferenceImpl::SubmitImages(GvaBaseInference *gva_base_inference,
                                          const std::vector<GstVideoRegionOfInterestMeta *> &metas, GstBuffer *buffer) {
    ITT_TASK(__FUNCTION__);
    try {
        if (!gva_base_inference)
            throw std::invalid_argument("GvaBaseInference is null");
        if (!gva_base_inference->priv)
            throw std::invalid_argument("GvaBaseInference priv is null");
        if (!gva_base_inference->priv->buffer_mapper)
            throw std::invalid_argument("Mapper is null");
        if (!buffer)
            throw std::invalid_argument("GstBuffer is null");

        auto &buf_mapper = *gva_base_inference->priv->buffer_mapper;
        assert(buf_mapper.memoryType() == GetInferenceMemoryType() && "Mapper mem type =/= inference mem type");

        /* we invoke CreateImage::gva_buffer_map::gst_video_frame_map with
         * GST_VIDEO_FRAME_MAP_FLAG_NO_REF to avoid refcount increase.
         * CreateImage::gva_buffer_unmap::gst_video_frame_unmap also will not decrease refcount.
         */
        InferenceBackend::ImagePtr image =
            buf_mapper.map(buffer, GstMapFlags(GST_MAP_READ | GST_VIDEO_FRAME_MAP_FLAG_NO_REF));

        if (!image)
            throw std::invalid_argument("image is null");

        size_t i = 0;
        for (const auto meta : metas) {
            // Workaround for CodeCoverity
            if (!image)
                break;

            ApplyImageBoundaries(image, meta, gva_base_inference->inference_region);
            auto result = MakeInferenceResult(gva_base_inference, model, meta, image, buffer);
            // Because image is a shared pointer with custom deleter which performs buffer unmapping
            // we need to manually reset it after we passed it to the last InferenceResult
            // Otherwise it may try to unmap buffer which is already pushed to downstream
            // if completion callback is called before we exit this scope
            if (++i == metas.size())
                image.reset();
            std::map<std::string, InferenceBackend::InputLayerDesc::Ptr> input_preprocessors;
            if (!model.input_processor_info.empty() && gva_base_inference->input_prerocessors_factory)
                input_preprocessors =
                    gva_base_inference->input_prerocessors_factory(model.inference, model.input_processor_info, meta);
            model.inference->SubmitImage(std::move(result), input_preprocessors);
        }
    } catch (const std::exception &e) {
        std::throw_with_nested(std::runtime_error("Failed to submit images to inference"));
    }

    // return FLOW_DROPPED as we push buffers from separate thread
    return GST_BASE_TRANSFORM_FLOW_DROPPED;
}

const InferenceImpl::Model &InferenceImpl::GetModel() const {
    return model;
}

GstFlowReturn InferenceImpl::TransformFrameIp(GvaBaseInference *gva_base_inference, GstBuffer *buffer) {
    ITT_TASK(__FUNCTION__);
    std::unique_lock<std::mutex> lock(_mutex);

    assert(gva_base_inference != nullptr && "Expected a valid pointer to gva_base_inference");
    assert(gva_base_inference->info != nullptr && "Expected a valid pointer to GstVideoInfo");
    assert(buffer != nullptr && "Expected a valid pointer to GstBuffer");

    // Shallow copy input buffer instead of increasing ref count
    buffer = gst_buffer_copy(buffer);
    // Unref buffer automatically on early exit
    auto buf_guard = makeScopeGuard([buffer] { gst_buffer_unref(buffer); });

    InferenceStatus status = INFERENCE_EXECUTED;
    {
        ITT_TASK("InferenceImpl::TransformFrameIp check_skip");
        if (++gva_base_inference->num_skipped_frames < gva_base_inference->inference_interval) {
            status = INFERENCE_SKIPPED_PER_PROPERTY;
        }
        if (gva_base_inference->no_block) {
            if (model.inference->IsQueueFull()) {
                status = INFERENCE_SKIPPED_NO_BLOCK;
            }
        }
        if (status == INFERENCE_EXECUTED) {
            gva_base_inference->num_skipped_frames = 0;
        }
    }

    /* Collect all ROI metas into std::vector */
    std::vector<GstVideoRegionOfInterestMeta *> metas;
    GstVideoRegionOfInterestMeta full_frame_meta;
    {
        ITT_TASK("InferenceImpl::TransformFrameIp collectROIMetas");
        switch (gva_base_inference->inference_region) {
        case ROI_LIST: {
            /* iterates through buffer's meta and pushes it in vector if inference needed. */
            GstVideoRegionOfInterestMeta *meta = NULL;
            gpointer state = NULL;
            while ((meta = GST_VIDEO_REGION_OF_INTEREST_META_ITERATE(buffer, &state))) {
                if (!gva_base_inference->is_roi_inference_needed ||
                    gva_base_inference->is_roi_inference_needed(gva_base_inference, gva_base_inference->frame_num,
                                                                buffer, meta)) {
                    metas.push_back(meta);
                }
            }
            break;
        }
        case FULL_FRAME: {
            /* pushes single meta in vector if full-frame inference is invoked. */
            full_frame_meta = GstVideoRegionOfInterestMeta();
            full_frame_meta.x = 0;
            full_frame_meta.y = 0;
            full_frame_meta.w = gva_base_inference->info->width;
            full_frame_meta.h = gva_base_inference->info->height;
            if (IsRoiSizeValid(&full_frame_meta))
                metas.push_back(&full_frame_meta);
            break;
        }
        default:
            throw std::logic_error("Unsupported inference region type");
        }
    }

    // count number ROIs to run inference on
    size_t inference_count = (status == INFERENCE_EXECUTED) ? metas.size() : 0;
    gva_base_inference->frame_num++;
    if (gva_base_inference->frame_num == G_MAXUINT64) {
        GVA_WARNING("The frame counter value limit has been reached. This value will be reset.");
    }

    // push into output_frames queue
    {
        ITT_TASK("InferenceImpl::TransformFrameIp pushIntoOutputFramesQueue");
        std::unique_lock output_lock(output_frames_mutex);

        // pause on accepting a new frame if downstream already blocks
        GstObject *src = &gva_base_inference->base_transform.element.object;
        while (CheckSrcPadBlocked(src)) {
            output_lock.unlock();
            lock.unlock();
            GVA_INFO("Wait on blocking output <%s>", src->name);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            lock.lock();
            output_lock.lock();
        }

        if (!inference_count && output_frames.empty()) {
            // If we don't need to run inference and there are no frames queued for inference then finish transform
            return GST_FLOW_OK;
        }

        // No need to unref buffer copy further
        buf_guard.disable();

        InferenceImpl::OutputFrame output_frame = {
            .buffer = buffer, .inference_count = inference_count, .filter = gva_base_inference, .inference_rois = {}};
        output_frames.push_back(output_frame);
        if (!inference_count) {
            return GST_BASE_TRANSFORM_FLOW_DROPPED;
        }
    }

    return SubmitImages(gva_base_inference, metas, buffer);
}

void InferenceImpl::PushFramesIfInferenceFailed(
    std::vector<std::shared_ptr<InferenceBackend::ImageInference::IFrameBase>> frames) {
    std::lock_guard<std::mutex> guard(output_frames_mutex);
    for (auto &frame : frames) {
        auto inference_result = std::dynamic_pointer_cast<InferenceResult>(frame);
        /* InferenceResult is inherited from IFrameBase */
        assert(inference_result.get() != nullptr && "Expected a valid InferenceResult");

        std::shared_ptr<InferenceFrame> inference_roi = inference_result->inference_frame;
        auto it =
            std::find_if(output_frames.begin(), output_frames.end(), [inference_roi](const OutputFrame &output_frame) {
                return output_frame.buffer == inference_roi->buffer;
            });

        if (it == output_frames.end())
            continue;

        PushBufferToSrcPad(*it);
        output_frames.erase(it);
    }
}

/**
 * Updates buffer pointers for corresponding to 'inference_roi' output_frame, decreases it's inference_count.
 * May affect buffer if it's not writable.
 * Acquires output_frames_mutex with std::lock_guard.
 *
 * @param[in] inference_roi - InferenceFrame to provide buffer's and inference element's info
 */
void InferenceImpl::UpdateOutputFrames(std::shared_ptr<InferenceFrame> &inference_roi) {
    assert(inference_roi && "Inference frame is null");
    std::lock_guard<std::mutex> guard(output_frames_mutex);

    /* we must iterate through std::list because it has no lookup operations */
    for (auto &output_frame : output_frames) {
        if (output_frame.buffer != inference_roi->buffer)
            continue;

        /* only gvadetect or full-frame elements are affecting buffer */
        if (inference_roi->gva_base_inference->type == GST_GVA_DETECT_TYPE ||
            inference_roi->gva_base_inference->inference_region == FULL_FRAME) {
            if (output_frame.inference_count == 0)
                // This condition is necessary if two items in output_frames refer to the same buffer.
                // If current output_frame.inference_count equals 0, then inference for this output_frame
                // already happened, but buffer wasn't pushed further by pipeline yet. We skip this buffer
                // to find another, to which current inference callback really belongs
                continue;
        }
        output_frame.inference_rois.push_back(inference_roi);
        --output_frame.inference_count;
        break;
    }
}

/**
 * Callback called when the inference request is completed. Updates output_frames and invokes post-processing for
 * corresponding inference element then makes gst_pad_push to send buffer further down the pipeline.
 * Nullifies shared_ptr for InferenceBackend::Image created during 'SubmitImages'.
 *
 * @param[in] blobs - the resulting blobs obtained after executing inference
 * @param[in] frames - frames for which an inference was executed
 *
 * @throw throw std::runtime_error when post-processing is failed
 */
void InferenceImpl::InferenceCompletionCallback(
    std::map<std::string, InferenceBackend::OutputBlob::Ptr> blobs,
    std::vector<std::shared_ptr<InferenceBackend::ImageInference::IFrameBase>> frames) {
    ITT_TASK(__FUNCTION__);
    if (frames.empty())
        return;

    std::vector<std::shared_ptr<InferenceFrame>> inference_frames;
    PostProcessor *post_proc = nullptr;

    for (auto &frame : frames) {
        auto inference_result = std::dynamic_pointer_cast<InferenceResult>(frame);
        /* InferenceResult is inherited from IFrameBase */
        assert(inference_result.get() != nullptr && "Expected a valid InferenceResult");

        std::shared_ptr<InferenceFrame> inference_roi = inference_result->inference_frame;
        inference_roi->image_transform_info = inference_result->GetImageTransformationParams();
        inference_result->image.reset(); // deleter will to not make buffer_unref, see 'SubmitImages' method
        post_proc = inference_roi->gva_base_inference->post_proc;

        inference_frames.push_back(inference_roi);
    }

    try {
        if (post_proc != nullptr) {
            PostProcessorExitStatus pp_e_s = post_proc->process(blobs, inference_frames);
            if (pp_e_s == PostProcessorExitStatus::FAIL)
                throw std::runtime_error("Post-processing has been exited with FAIL code.");
        }
    } catch (const std::exception &e) {
        GST_ERROR("%s", Utils::createNestedErrorMsg(e).c_str());
    }

    for (auto &inference_roi : inference_frames) {
        UpdateOutputFrames(inference_roi);
    }
    PushOutput();
}
