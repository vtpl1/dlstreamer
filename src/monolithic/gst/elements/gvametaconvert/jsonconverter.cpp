/*******************************************************************************
 * Copyright (C) 2018-2024 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "jsonconverter.h"
#include "gva_utils.h"
#include "video_frame.h"
#include <utils.h>

#ifdef AUDIO
#include "audioconverter.h"
#endif
#include "convert_tensor.h"

#include <nlohmann/json.hpp>

#include <iomanip>
#include <iostream>

using json = nlohmann::json;

GST_DEBUG_CATEGORY_STATIC(gst_json_converter_debug);
#define GST_CAT_DEFAULT gst_json_converter_debug

namespace {

#define TIMESTAMP_LENGTH_BEFORE_MICROSECONDS 23
#define TIMESTAMP_OFFSET_POSITION 26
#define MICROSECONDS_TO_REMOVE 3

// Function to cut part of a string
gchar *cut_microseconds(const gchar *input) {
    if (input == NULL)
        return NULL;

    // Calculate the length of the new string
    size_t new_length = strlen(input) - MICROSECONDS_TO_REMOVE;

    // Allocate memory for the new string
    gchar *new_string = (gchar *)g_malloc(new_length + 1);
    if (new_string == NULL)
        return NULL;

    // Copy the part before the microseconds
    strncpy(new_string, input,
            TIMESTAMP_LENGTH_BEFORE_MICROSECONDS); // Copy up to the first three digits of the microseconds
    new_string[TIMESTAMP_LENGTH_BEFORE_MICROSECONDS] = '\0';

    // Append the time zone offset
    strcat(new_string, input + TIMESTAMP_OFFSET_POSITION);

    return new_string;
}

/**
 * @return JSON object which contains parameters such as resolution, timestamp, source and tags.
 */
json get_frame_data(GstGvaMetaConvert *converter, GstBuffer *buffer) {
    assert(converter && buffer && "Expected valid pointers GstGvaMetaConvert and GstBuffer");

    json res = json::object();
    GstSegment converter_segment = converter->base_gvametaconvert.segment;
    GstClockTime timestamp = gst_segment_to_stream_time(&converter_segment, GST_FORMAT_TIME, buffer->pts);

    GstVideoTimeCodeMeta *tc_meta = gst_buffer_get_video_time_code_meta(buffer);

    if (converter->info)
        res["resolution"] = json::object({{"width", converter->info->width}, {"height", converter->info->height}});
    if (converter->source)
        res["source"] = converter->source;
    if (timestamp != G_MAXUINT64)
        res["timestamp"] = timestamp;
    if (converter->tags && json::accept(converter->tags))
        res["tags"] = json::parse(converter->tags);
    if (tc_meta) {
        GstVideoTimeCode *vtc = gst_video_time_code_copy(&tc_meta->tc);
        GDateTime *frame_date_time = gst_video_time_code_to_date_time(vtc);

        // Format the datetime to ISO string with milliseconds
        gchar *iso_string = NULL;
        gchar *iso_string_millisec = NULL;
        GDateTime *utc_datetime = NULL;

        if (converter->timestamp_utc) {
            utc_datetime = g_date_time_to_utc(frame_date_time); // Convert the GDateTime object to UTC
            if (!utc_datetime)
                GST_WARNING("Failed to convert datetime to UTC");
            else {
                g_date_time_unref(frame_date_time);
                frame_date_time = utc_datetime;
                // UTC mode: add 'Z' at the end
                iso_string = g_date_time_format(frame_date_time, "%Y-%m-%dT%H:%M:%S.%fZ");
            }
        } else
            // Non-UTC mode: include offset from UTC
            iso_string = g_date_time_format(frame_date_time, "%Y-%m-%dT%H:%M:%S.%f:%z");

        if (iso_string == NULL)
            GST_WARNING("Failed to format the datetime to ISO string");
        else {

            if (!(converter->timestamp_microseconds)) {
                iso_string_millisec = cut_microseconds(iso_string);
                g_free(iso_string);
                iso_string = iso_string_millisec;
            }

            // Store the formatted timestamp in the result
            res["system_timestamp"] = iso_string;

            // Free the allocated resources
            g_free(iso_string);
        }

        if (frame_date_time)
            g_date_time_unref(frame_date_time);

        if (vtc)
            gst_video_time_code_free(vtc);
    }
    return res;
}

/**
 * @return JSON array which contains ROIs attributes and their detection results.
 * Also contains ROIs classification results if any.
 */
json convert_roi_detection(GstGvaMetaConvert *converter, GstBuffer *buffer) {
    assert(converter && buffer && "Expected valid pointers GstGvaMetaConvert and GstBuffer");

    json res = json::array();
    GVA::VideoFrame video_frame(buffer, converter->info);
    for (GVA::RegionOfInterest &roi : video_frame.regions()) {
        gint id = roi.object_id();

        json jobject = json::object();

        if (converter->add_tensor_data) {
            jobject["tensors"] = json::array();
        }

        auto rect = roi.rect();

        jobject.push_back({"x", rect.x});
        jobject.push_back({"y", rect.y});
        jobject.push_back({"w", rect.w});
        jobject.push_back({"h", rect.h});
        jobject.push_back({"region_id", roi.region_id()});

        if (id != 0)
            jobject.push_back({"id", id});

        const std::string roi_type = roi.label();

        if (!roi_type.empty()) {
            jobject.push_back({"roi_type", roi_type});
        }
        for (GList *l = roi.get_params(); l; l = g_list_next(l)) {

            GstStructure *s = GST_STRUCTURE(l->data);
            const gchar *s_name = gst_structure_get_name(s);
            if (strcmp(s_name, "detection") == 0) {
                double xminval;
                double xmaxval;
                double yminval;
                double ymaxval;
                double confidence;
                int label_id;
                if (gst_structure_get(s, "x_min", G_TYPE_DOUBLE, &xminval, "x_max", G_TYPE_DOUBLE, &xmaxval, "y_min",
                                      G_TYPE_DOUBLE, &yminval, "y_max", G_TYPE_DOUBLE, &ymaxval, NULL)) {
                    json detection = json::object(
                        {{"bounding_box",
                          {{"x_min", xminval}, {"x_max", xmaxval}, {"y_min", yminval}, {"y_max", ymaxval}}}});

                    if (gst_structure_get(s, "confidence", G_TYPE_DOUBLE, &confidence, NULL)) {
                        detection.push_back({"confidence", confidence});
                    }

                    if (gst_structure_get(s, "label_id", G_TYPE_INT, &label_id, NULL)) {
                        detection.push_back({"label_id", label_id});
                    }

                    const std::string label = roi.label();

                    if (!label.empty()) {
                        detection.push_back({"label", label});
                    }
                    jobject.push_back(json::object_t::value_type("detection", detection));
                }
            } else {
                char *label;
                char *model_name;
                if (gst_structure_get(s, "label", G_TYPE_STRING, &label, "model_name", G_TYPE_STRING, &model_name,
                                      NULL)) {
                    double confidence;
                    int label_id;
                    const gchar *attribute_name = gst_structure_has_field(s, "attribute_name")
                                                      ? gst_structure_get_string(s, "attribute_name")
                                                      : s_name;
                    json classification = json::object({{"label", label}, {"model", {{"name", model_name}}}});

                    if (gst_structure_get(s, "confidence", G_TYPE_DOUBLE, &confidence, NULL)) {
                        classification.push_back({"confidence", confidence});
                    }

                    if (gst_structure_get(s, "label_id", G_TYPE_INT, &label_id, NULL)) {
                        classification.push_back({"label_id", label_id});
                    }

                    jobject.push_back(json::object_t::value_type(attribute_name, classification));
                    g_free(label);
                    g_free(model_name);
                }
            }
            if (converter->add_tensor_data) {
                GVA::Tensor s_tensor = GVA::Tensor((GstStructure *)l->data);
                jobject["tensors"].push_back(convert_tensor(s_tensor));
            }
        }
        if (!jobject.empty()) {
            res.push_back(jobject);
        }
    }
    return res;
}

/**
 * @return JSON array which contains raw tensor metas from frame.
 */
json convert_frame_tensors(GstGvaMetaConvert *converter, GstBuffer *buffer) {
    assert(converter && buffer && "Expected valid pointers GstGvaMetaConvert and GstBuffer");

    GVA::VideoFrame video_frame(buffer, converter->info);
    const std::vector<GVA::Tensor> tensors = video_frame.tensors();
    json array = json::array();
    for (auto &tensor : video_frame.tensors()) {
        if (!tensor.has_field("type")) {
            array.push_back(convert_tensor(tensor));
        }
    }
    return array;
}

/**
 * @return JSON object which contains full-frame attributes and full-frame classification results from frame.
 */
json convert_frame_classification(GstGvaMetaConvert *converter, GstBuffer *buffer) {
    assert(converter && buffer && "Expected valid pointers GstGvaMetaConvert and GstBuffer");

    GVA::VideoFrame video_frame(buffer, converter->info);
    const std::vector<GVA::Tensor> tensors = video_frame.tensors();
    if (tensors.empty())
        return json{};

    json jobject = json::object();
    if (converter->add_tensor_data) {
        jobject["tensors"] = json::array();
    }
    jobject.push_back({"x", 0});
    jobject.push_back({"y", 0});
    jobject.push_back({"w", converter->info->width});
    jobject.push_back({"h", converter->info->height});

    for (GVA::Tensor &tensor : video_frame.tensors()) {
        if (tensor.has_field("label") || tensor.has_field("label_id")) {
            std::string label = tensor.label();
            std::string model_name = tensor.model_name();
            json classification = json::object({});
            if (!label.empty()) {
                classification.push_back(json::object_t::value_type("label", label));
            }
            if (!model_name.empty()) {
                classification.push_back(json::object_t::value_type("model", {{"name", model_name}}));
            }
            std::string attribute_name =
                tensor.has_field("attribute_name") ? tensor.get_string("attribute_name") : tensor.name();

            if (tensor.has_field("confidence")) {
                classification.push_back(json::object_t::value_type("confidence", tensor.confidence()));
            }
            if (tensor.has_field("label_id")) {
                classification.push_back(json::object_t::value_type("label_id", tensor.get_int("label_id")));
            }

            jobject.push_back(json::object_t::value_type(attribute_name, classification));
        }
        if (converter->add_tensor_data) {
            jobject["tensors"].push_back(convert_tensor(tensor));
        }
    }
    return jobject;
}

} // namespace

gboolean to_json(GstGvaMetaConvert *converter, GstBuffer *buffer) {
    GST_DEBUG_CATEGORY_INIT(gst_json_converter_debug, "jsonconverter", 0, "JSON converter");

    if (!converter) {
        GST_ERROR("Failed convert to json: GvaMetaConvert is null");
        return FALSE;
    }

    if (!buffer) {
        GST_ERROR_OBJECT(converter, "Failed convert to json: GstBuffer is null");
        return FALSE;
    }

    try {
        if (converter->info) {
            json jframe = get_frame_data(converter, buffer);
            /* objects section */
            json jframe_objects;
            json roi_detection = convert_roi_detection(converter, buffer);
            if (!roi_detection.empty()) {
                jframe_objects = roi_detection;
            } /* roi_detection can contain multiple objects, while frame_classification - only one */
            json frame_classification = convert_frame_classification(converter, buffer);
            if (!frame_classification.empty()) {
                jframe_objects.push_back(frame_classification);
            }
            /* tensors section */
            json jframe_tensors;
            if (converter->add_tensor_data) {
                jframe_tensors = convert_frame_tensors(converter, buffer);
            }

            if (jframe_objects.empty() && jframe_tensors.empty()) {
                if (!converter->add_empty_detection_results) {
                    GST_DEBUG_OBJECT(converter, "No detections found. Not posting JSON message");
                    return TRUE;
                }
            }

            if (!jframe.is_null()) {
                if (!jframe_objects.empty()) {
                    jframe["objects"] = jframe_objects;
                }
                if (!jframe_tensors.empty()) {
                    jframe["tensors"] = jframe_tensors;
                }
                std::string json_message = jframe.dump(converter->json_indent);
                GVA::VideoFrame video_frame(buffer, converter->info);
                video_frame.add_message(json_message);
                GST_INFO_OBJECT(converter, "JSON message: %s", json_message.c_str());
            }
        }
#ifdef AUDIO
        else {
            return convert_audio_meta_to_json(converter, buffer);
        }
#endif
    } catch (const std::exception &e) {
        GST_ERROR_OBJECT(converter, "%s", Utils::createNestedErrorMsg(e).c_str());
        return FALSE;
    }
    return TRUE;
}
