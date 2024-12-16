/*******************************************************************************
 * Copyright (C) 2024 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "yolo_v8.h"

#include "copy_blob_to_gststruct.h"
#include "inference_backend/image_inference.h"
#include "inference_backend/logger.h"
#include "safe_arithmetic.hpp"

#include <gst/gst.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace post_processing;

void YOLOv8Converter::parseOutputBlob(const float *data, const std::vector<size_t> &dims,
                                      std::vector<DetectedObject> &objects, bool oob) const {

    size_t dims_size = dims.size();
    size_t input_width = getModelInputImageInfo().width;
    size_t input_height = getModelInputImageInfo().height;

    if (dims_size < BlobToROIConverter::min_dims_size)
        throw std::invalid_argument("Output blob dimensions size " + std::to_string(dims_size) +
                                    " is not supported (less than " +
                                    std::to_string(BlobToROIConverter::min_dims_size) + ").");

    size_t object_size = dims[dims_size - 2];
    size_t max_proposal_count = dims[dims_size - 1];

    // transpose
    cv::Mat outputs(object_size, max_proposal_count, CV_32F, (float *)data);
    cv::transpose(outputs, outputs);
    float *output_data = (float *)outputs.data;

    for (size_t i = 0; i < max_proposal_count; ++i) {
        float *classes_scores = output_data + YOLOV8_OFFSET_CS;
        cv::Mat scores(1, object_size - YOLOV8_OFFSET_CS - (oob ? 1 : 0), CV_32FC1, classes_scores);
        cv::Point class_id;
        double maxClassScore;
        cv::minMaxLoc(scores, 0, &maxClassScore, 0, &class_id);
        if (maxClassScore > confidence_threshold) {

            float x = output_data[YOLOV8_OFFSET_X];
            float y = output_data[YOLOV8_OFFSET_Y];
            float w = output_data[YOLOV8_OFFSET_W];
            float h = output_data[YOLOV8_OFFSET_H];
            float r = oob ? output_data[object_size - 1] : 0;
            objects.push_back(DetectedObject(x, y, w, h, r, maxClassScore, class_id.x,
                                             BlobToMetaConverter::getLabelByLabelId(class_id.x), 1.0f / input_width,
                                             1.0f / input_height, true));
        }
        output_data += object_size;
    }
}

TensorsTable YOLOv8Converter::convert(const OutputBlobs &output_blobs) const {
    ITT_TASK(__FUNCTION__);
    try {
        const auto &model_input_image_info = getModelInputImageInfo();
        size_t batch_size = model_input_image_info.batch_size;

        DetectedObjectsTable objects_table(batch_size);

        for (size_t batch_number = 0; batch_number < batch_size; ++batch_number) {
            auto &objects = objects_table[batch_number];

            for (const auto &blob_iter : output_blobs) {
                const InferenceBackend::OutputBlob::Ptr &blob = blob_iter.second;
                if (not blob)
                    throw std::invalid_argument("Output blob is nullptr.");

                size_t unbatched_size = blob->GetSize() / batch_size;
                parseOutputBlob(reinterpret_cast<const float *>(blob->GetData()) + unbatched_size * batch_number,
                                blob->GetDims(), objects, false);
            }
        }

        return storeObjects(objects_table);
    } catch (const std::exception &e) {
        std::throw_with_nested(std::runtime_error("Failed to do YoloV8 post-processing."));
    }
    return TensorsTable{};
}

TensorsTable YOLOv8ObbConverter::convert(const OutputBlobs &output_blobs) const {
    ITT_TASK(__FUNCTION__);
    try {
        const auto &model_input_image_info = getModelInputImageInfo();
        size_t batch_size = model_input_image_info.batch_size;

        DetectedObjectsTable objects_table(batch_size);

        for (size_t batch_number = 0; batch_number < batch_size; ++batch_number) {
            auto &objects = objects_table[batch_number];

            for (const auto &blob_iter : output_blobs) {
                const InferenceBackend::OutputBlob::Ptr &blob = blob_iter.second;
                if (not blob)
                    throw std::invalid_argument("Output blob is nullptr.");

                size_t unbatched_size = blob->GetSize() / batch_size;
                parseOutputBlob(reinterpret_cast<const float *>(blob->GetData()) + unbatched_size * batch_number,
                                blob->GetDims(), objects, true);
            }
        }

        return storeObjects(objects_table);
    } catch (const std::exception &e) {
        std::throw_with_nested(std::runtime_error("Failed to do YoloV8-OBB post-processing."));
    }
    return TensorsTable{};
}

TensorsTable YOLOv8PoseConverter::convert(const OutputBlobs &output_blobs) const {
    ITT_TASK(__FUNCTION__);
    try {
        const auto &model_input_image_info = getModelInputImageInfo();
        size_t batch_size = model_input_image_info.batch_size;

        DetectedObjectsTable objects_table(batch_size);

        for (size_t batch_number = 0; batch_number < batch_size; ++batch_number) {
            auto &objects = objects_table[batch_number];

            for (const auto &blob_iter : output_blobs) {
                const InferenceBackend::OutputBlob::Ptr &blob = blob_iter.second;
                if (not blob)
                    throw std::invalid_argument("Output blob is nullptr.");

                size_t unbatched_size = blob->GetSize() / batch_size;
                parseOutputBlob(reinterpret_cast<const float *>(blob->GetData()) + unbatched_size * batch_number,
                                blob->GetDims(), objects);
            }
        }

        return storeObjects(objects_table);
    } catch (const std::exception &e) {
        std::throw_with_nested(std::runtime_error("Failed to do YoloV8 post-processing."));
    }
    return TensorsTable{};
}

static const std::vector<std::string> point_names = {
    "nose",    "eye_l",   "eye_r", "ear_l", "ear_r",  "shoulder_l", "shoulder_r", "elbow_l", "elbow_r",
    "wrist_l", "wrist_r", "hip_l", "hip_r", "knee_l", "knee_r",     "ankle_l",    "ankle_r"};
// static const std::vector<std::string> point_connections = {
//     "shoulder_l", "shoulder_r", "nose",       "eye_l",   "nose",       "eye_r",   "eye_l",   "ear_l",   "eye_r",
//     "ear_r",      "elbow_l",    "shoulder_l", "elbow_r", "shoulder_r", "wrist_l", "elbow_l", "wrist_r", "elbow_r",
//     "hip_l",      "knee_l",     "hip_r",      "knee_r",  "knee_l",     "ankle_l", "knee_r",  "ankle_r"};
static const std::vector<std::string> point_connections = {
    "nose",    "eye_l", "nose",       "eye_r",      "ear_l",      "shoulder_l", "ear_r",   "shoulder_r", "eye_l",
    "ear_l",   "eye_r", "ear_r",      "shoulder_l", "shoulder_r", "shoulder_l", "hip_l",   "shoulder_r", "hip_r",
    "hip_l",   "hip_r", "shoulder_l", "elbow_l",    "shoulder_r", "elbow_r",    "elbow_l", "wrist_l",    "elbow_r",
    "wrist_r", "hip_l", "knee_l",     "hip_r",      "knee_r",     "knee_l",     "ankle_l", "knee_r",     "ankle_r"};

void YOLOv8PoseConverter::parseOutputBlob(const float *data, const std::vector<size_t> &dims,
                                          std::vector<DetectedObject> &objects) const {
    size_t boxes_dims_size = dims.size();
    size_t input_width = getModelInputImageInfo().width;
    size_t input_height = getModelInputImageInfo().height;

    if (boxes_dims_size < BlobToROIConverter::min_dims_size)
        throw std::invalid_argument("Output blob dimensions size " + std::to_string(boxes_dims_size) +
                                    " is not supported (less than " +
                                    std::to_string(BlobToROIConverter::min_dims_size) + ").");

    size_t object_size = dims[boxes_dims_size - 2];
    size_t max_proposal_count = dims[boxes_dims_size - 1];
    size_t keypoint_count = (object_size - YOLOV8_OFFSET_CS - 1) / 3;

    // Transpose objects
    cv::Mat outputs(object_size, max_proposal_count, CV_32F, (float *)data);
    cv::transpose(outputs, outputs);
    float *output_data = (float *)outputs.data;

    for (size_t i = 0; i < max_proposal_count; ++i) {
        float confidence = output_data[YOLOV8_OFFSET_CS];
        if (confidence > confidence_threshold) {

            // coordinates are relative to bounding box center
            float w = output_data[YOLOV8_OFFSET_W];
            float h = output_data[YOLOV8_OFFSET_H];
            float x = output_data[YOLOV8_OFFSET_X] - w / 2;
            float y = output_data[YOLOV8_OFFSET_Y] - h / 2;

            auto detected_object =
                DetectedObject(x, y, w, h, 0, confidence, 0, BlobToMetaConverter::getLabelByLabelId(0),
                               1.0f / input_width, 1.0f / input_height, false);

            // create relative keypoint positions within bounding box
            cv::Mat positions(keypoint_count, 2, CV_32F);
            for (size_t k = 0; k < keypoint_count; k++) {
                float position_x = output_data[YOLOV8_OFFSET_CS + 1 + k * 3 + 0];
                float position_y = output_data[YOLOV8_OFFSET_CS + 1 + k * 3 + 1];
                positions.at<float>(k, 0) = (position_x - x) / w;
                positions.at<float>(k, 1) = (position_y - y) / h;
            }

            // create tensor with keypoints
            GstStructure *tensor = gst_structure_copy(getModelProcOutputInfo().get());
            gst_structure_set_name(tensor, "keypoints");
            gst_structure_set(tensor, "precision", G_TYPE_INT, GVA_PRECISION_FP32, NULL);
            gst_structure_set(tensor, "format", G_TYPE_STRING, "keypoints", NULL);

            GValueArray *data = g_value_array_new(2);
            GValue gvalue = G_VALUE_INIT;
            g_value_init(&gvalue, G_TYPE_UINT);
            g_value_set_uint(&gvalue, safe_convert<uint32_t>(keypoint_count));
            g_value_array_append(data, &gvalue);
            g_value_set_uint(&gvalue, 2);
            g_value_array_append(data, &gvalue);
            gst_structure_set_array(tensor, "dims", data);
            g_value_array_free(data);

            copy_buffer_to_structure(tensor, reinterpret_cast<const void *>(positions.data),
                                     keypoint_count * 2 * sizeof(float));

            data = g_value_array_new(keypoint_count);
            gvalue = G_VALUE_INIT;
            g_value_init(&gvalue, G_TYPE_FLOAT);
            for (size_t k = 0; k < keypoint_count; k++) {
                g_value_set_float(&gvalue, output_data[YOLOV8_OFFSET_CS + 1 + k * 3 + 2]);
                g_value_array_append(data, &gvalue);
            }
            gst_structure_set_array(tensor, "confidence", data);
            g_value_array_free(data);

            data = g_value_array_new(point_names.size());
            gvalue = G_VALUE_INIT;
            g_value_init(&gvalue, G_TYPE_STRING);
            for (size_t i = 0; i < point_names.size(); i++) {
                g_value_set_string(&gvalue, point_names[i].c_str());
                g_value_array_append(data, &gvalue);
            }
            gst_structure_set_array(tensor, "point_names", data);
            g_value_array_free(data);

            data = g_value_array_new(point_connections.size());
            gvalue = G_VALUE_INIT;
            g_value_init(&gvalue, G_TYPE_STRING);
            for (size_t i = 0; i < point_connections.size(); i++) {
                g_value_set_string(&gvalue, point_connections[i].c_str());
                g_value_array_append(data, &gvalue);
            }
            gst_structure_set_array(tensor, "point_connections", data);
            g_value_array_free(data);

            detected_object.tensors.push_back(tensor);

            objects.push_back(detected_object);
        }
        output_data += object_size;

        // Future optimization: generate masks after running NMS algorithm on detected objects
    }
}

void YOLOv8SegConverter::parseOutputBlob(const float *boxes_data, const std::vector<size_t> &boxes_dims,
                                         const float *masks_data, const std::vector<size_t> &masks_dims,
                                         std::vector<DetectedObject> &objects) const {
    size_t boxes_dims_size = boxes_dims.size();
    size_t masks_dims_size = masks_dims.size();
    size_t input_width = getModelInputImageInfo().width;
    size_t input_height = getModelInputImageInfo().height;

    if (boxes_dims_size < BlobToROIConverter::min_dims_size)
        throw std::invalid_argument("Output blob dimensions size " + std::to_string(boxes_dims_size) +
                                    " is not supported (less than " +
                                    std::to_string(BlobToROIConverter::min_dims_size) + ").");

    size_t object_size = boxes_dims[boxes_dims_size - 2];
    size_t max_proposal_count = boxes_dims[boxes_dims_size - 1];
    size_t mask_count = masks_dims[masks_dims_size - 3];
    size_t class_count = object_size - mask_count - YOLOV8_OFFSET_CS;
    size_t mask_height = masks_dims[masks_dims_size - 2];
    size_t mask_width = masks_dims[masks_dims_size - 1];

    // Transpose objects
    cv::Mat outputs(object_size, max_proposal_count, CV_32F, (float *)boxes_data);
    cv::transpose(outputs, outputs);
    float *output_data = (float *)outputs.data;

    // Map masks
    cv::Mat masks(mask_count, mask_width * mask_height, CV_32F, (float *)masks_data);

    for (size_t i = 0; i < max_proposal_count; ++i) {
        cv::Mat class_scores(1, class_count, CV_32F, output_data + YOLOV8_OFFSET_CS);
        cv::Mat mask_scores(1, mask_count, CV_32F, output_data + YOLOV8_OFFSET_CS + class_count);
        cv::Point class_id;
        double max_class_score;
        cv::minMaxLoc(class_scores, 0, &max_class_score, 0, &class_id);
        if (max_class_score > confidence_threshold) {

            // coordinates are relative to bounding box center
            float w = output_data[YOLOV8_OFFSET_W];
            float h = output_data[YOLOV8_OFFSET_H];
            float x = output_data[YOLOV8_OFFSET_X] - w / 2;
            float y = output_data[YOLOV8_OFFSET_Y] - h / 2;

            auto detected_object = DetectedObject(x, y, w, h, 0, max_class_score, class_id.x,
                                                  BlobToMetaConverter::getLabelByLabelId(class_id.x),
                                                  1.0f / input_width, 1.0f / input_height, false);

            // compose mask for detected bounding box
            cv::Mat composed_mask = mask_scores * masks;
            composed_mask = composed_mask.reshape(1, mask_height);

            // crop composed mask to fit into object bounding box
            cv::Mat cropped_mask;
            int cx = x * mask_width / input_width;
            int cy = y * mask_height / input_height;
            int cw = w * mask_width / input_width;
            int ch = h * mask_height / input_height;
            composed_mask(cv::Rect(cx, cy, cw, ch)).copyTo(cropped_mask);

            // apply sigmoid activation
            cropped_mask.forEach<float>([](float &element, const int position[]) -> void {
                std::ignore = position;
                element = 1 / (1 + std::exp(-element));
            });

            // create segmentation mask tensor
            GstStructure *tensor = gst_structure_copy(getModelProcOutputInfo().get());
            gst_structure_set_name(tensor, "mask_yolov8");
            gst_structure_set(tensor, "precision", G_TYPE_INT, GVA_PRECISION_FP32, NULL);
            gst_structure_set(tensor, "format", G_TYPE_STRING, "segmentation_mask", NULL);

            GValueArray *data = g_value_array_new(2);
            GValue gvalue = G_VALUE_INIT;
            g_value_init(&gvalue, G_TYPE_UINT);
            g_value_set_uint(&gvalue, safe_convert<uint32_t>(cropped_mask.cols));
            g_value_array_append(data, &gvalue);
            g_value_set_uint(&gvalue, safe_convert<uint32_t>(cropped_mask.rows));
            g_value_array_append(data, &gvalue);
            gst_structure_set_array(tensor, "dims", data);
            g_value_array_free(data);

            copy_buffer_to_structure(tensor, reinterpret_cast<const void *>(cropped_mask.data),
                                     cropped_mask.rows * cropped_mask.cols * sizeof(float));
            detected_object.tensors.push_back(tensor);

            objects.push_back(detected_object);
        }
        output_data += object_size;

        // Future optimization: generate masks after running NMS algorithm on detected objects
    }
}

TensorsTable YOLOv8SegConverter::convert(const OutputBlobs &output_blobs) const {
    ITT_TASK(__FUNCTION__);
    try {
        const auto &model_input_image_info = getModelInputImageInfo();
        size_t batch_size = model_input_image_info.batch_size;

        DetectedObjectsTable objects_table(batch_size);

        for (size_t batch_number = 0; batch_number < batch_size; ++batch_number) {
            auto &objects = objects_table[batch_number];

            const InferenceBackend::OutputBlob::Ptr &boxes_blob = output_blobs.at(TENSORS_BOXES_KEY);
            const InferenceBackend::OutputBlob::Ptr &masks_blob = output_blobs.at(TENSORS_MASKS_KEY);

            if ((not boxes_blob) || (not masks_blob))
                throw std::invalid_argument("Output blob is nullptr.");

            size_t boxes_unbatched_size = boxes_blob->GetSize() / batch_size;
            size_t masks_unbatched_size = masks_blob->GetSize() / batch_size;
            parseOutputBlob(
                reinterpret_cast<const float *>(boxes_blob->GetData()) + boxes_unbatched_size * batch_number,
                boxes_blob->GetDims(),
                reinterpret_cast<const float *>(masks_blob->GetData()) + masks_unbatched_size * batch_number,
                masks_blob->GetDims(), objects);
        }

        return storeObjects(objects_table);
    } catch (const std::exception &e) {
        std::throw_with_nested(std::runtime_error("Failed to do YoloV8-SEG post-processing."));
    }
    return TensorsTable{};
}