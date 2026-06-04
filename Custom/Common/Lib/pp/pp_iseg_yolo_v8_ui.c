/**
******************************************************************************
* @file    pp_iseg_yolo_v8_ui.c
* @author  GPM Application Team
*
******************************************************************************
* @attention
*
* Copyright (c) 2023 STMicroelectronics.
* All rights reserved.
*
* This software is licensed under terms that can be found in the LICENSE file
* in the root directory of this software component.
* If no LICENSE file comes with this software, it is provided AS-IS.
*
******************************************************************************
*/

#include "pp.h"
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include "cJSON.h"
#include "mem.h"
#include "ll_aton_runtime.h"
#include "ll_aton_reloc_network.h"
#include "debug.h"

#include <math.h>

#include "iseg_yolov8_pp_if.h"

#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

/* ST official ISEG model includes sigmoid + bbox normalization in graph.
   However, ATON NPU strips sigmoid during compilation, outputting raw logits.
   The library's int8 threshold formula (conf_threshold / scale + 0.5 + zp)
   operates in the dequantized value space. We pass the user's probability
   threshold directly; the library filters at logit > threshold (sigmoid > ~0.56
   for threshold=0.25). Sigmoid is applied to the library's output confidence
   in post-processing to get accurate probability values.
   OD models avoid this issue because their zp=-128 maps dequantized range to [0, ~1],
   which coincidentally covers probability space even if sigmoid is stripped. */

/* Per-instance parameters for ISEG YOLOv8 UI postprocess */
typedef struct {
    iseg_yolov8_pp_static_param_t core;     /* original static params, now per-instance */
    iseg_pp_outBuffer_t *iseg_pp_buffer;    /* per-instance output buffer */
    iseg_detect_t *iseg_detect_buffer;     /* per-instance detection buffer */
    uint8_t *iseg_mask_buffer;            /* per-instance mask buffer */
    iseg_yolov8_pp_scratchBuffer_s8_t *scratch_detections_buffer; /* per-instance scratch detections buffer */
    float32_t *mask_float_buffer;          /* per-instance mask float buffer */
    int8_t *mask_int8_buffer;               /* per-instance mask int8 buffer */
    char **class_names;                     /* per-instance class name array */
    uint8_t det_output_index;               /* ATON output index for detection tensor */
    uint8_t mask_output_index;              /* ATON output index for mask tensor */
} pp_iseg_yolo_v8_ui_params_t;

/*
Example JSON configuration:
"postprocess_params": {
  "num_classes": 80,
  "class_names": ["person", "bicycle", "car", ...],
  "confidence_threshold": 0.5,
  "iou_threshold": 0.45,
  "max_detections": 100,
  "total_boxes": 8400,
  "mask_size": 32,
  "num_masks": 32
}
*/
static int32_t init(const char *json_str, void **pp_params, void *nn_inst)
{
    pp_iseg_yolo_v8_ui_params_t *pp_ctx = (pp_iseg_yolo_v8_ui_params_t *)hal_mem_alloc_any(sizeof(pp_iseg_yolo_v8_ui_params_t));
    if (!pp_ctx) {
        return AI_ISEG_POSTPROCESS_ERROR_NO;
    }
    memset(pp_ctx, 0, sizeof(pp_iseg_yolo_v8_ui_params_t));

    iseg_yolov8_pp_static_param_t *params = &pp_ctx->core;
    
    // Get quantization parameters from NN instance (for int8 models)
    NN_Instance_TypeDef *NN_Instance = (NN_Instance_TypeDef *)nn_inst;
    if (NN_Instance != NULL) {
        const LL_Buffer_InfoTypeDef *buffers_info = ll_aton_reloc_get_output_buffers_info(NN_Instance, 0);

        /* ATON orders output buffers by internal ID, which may differ from
           TFLite export order. Detect by shape:
           - Detection: shape[1] = 4(bbox) + nb_classes + nb_mask_coeff
           - Mask: shape[1] = mask_spatial_dim (height of [1,H,W,32]) */
#define ISEG_DET_CHANNELS   116   /* 4 bbox + 80 classes + 32 mask coefficients */
#define ISEG_MASK_SPATIAL   64    /* 64x64 prototype mask height */
        if (buffers_info == NULL) {
            pp_ctx->det_output_index = 1;
            pp_ctx->mask_output_index = 0;
        } else {
            int det_idx = -1, mask_idx = -1;
            for (int i = 0; i < 2; i++) {
                if (buffers_info[i].shape != NULL && buffers_info[i].shape[1] == ISEG_DET_CHANNELS) {
                    det_idx = i;
                    mask_idx = 1 - i;
                } else if (buffers_info[i].shape != NULL && buffers_info[i].shape[1] == ISEG_MASK_SPATIAL) {
                    mask_idx = i;
                    det_idx = 1 - i;
                }
            }
            /* Fallback: assume TFLite order if shape detection fails */
            if (det_idx < 0) { det_idx = 1; mask_idx = 0; }

            pp_ctx->det_output_index = det_idx;
            pp_ctx->mask_output_index = mask_idx;

            if (buffers_info[det_idx].scale != NULL) {
                params->raw_output_scale = *(buffers_info[det_idx].scale);
                params->raw_output_zero_point = *(buffers_info[det_idx].offset);
            }
            if (buffers_info[mask_idx].scale != NULL) {
                params->mask_raw_output_scale = *(buffers_info[mask_idx].scale);
                params->mask_raw_output_zero_point = *(buffers_info[mask_idx].offset);
            }
        }
        /* ST library expects channel-first [channels, boxes] layout (accessed via
           arr[channel * nb_total_boxes + box]). ATON output is also channel-first,
           so no transpose needed — pass raw data directly. */
    }

    params->nb_classes = 80;
    params->nb_total_boxes = 8400;
    params->max_boxes_limit = 100;
    params->conf_threshold = 0.5f;
    params->iou_threshold = 0.45f;
    params->size_masks = 32;
    params->nb_masks = 32;
    params->nb_detect = 0;

    // If JSON is provided, parse and override parameters
    if (json_str != NULL) {
        cJSON *root = cJSON_Parse(json_str);
        if (root != NULL) {
            cJSON *pp = cJSON_GetObjectItemCaseSensitive(root, "postprocess_params");
            if (pp == NULL) {
                // Compatibility: the input is already a postprocess_params object
                pp = root;
            }

            if (cJSON_IsObject(pp)) {
                cJSON *num_classes = cJSON_GetObjectItemCaseSensitive(pp, "num_classes");
                if (cJSON_IsNumber(num_classes)) {
                    params->nb_classes = (int32_t)num_classes->valuedouble;
                }

                cJSON *class_names = cJSON_GetObjectItemCaseSensitive(pp, "class_names");
                if (cJSON_IsArray(class_names)) {
                    pp_ctx->class_names = (char **)hal_mem_alloc_any(sizeof(char *) * params->nb_classes);
                    for (int i = 0; i < params->nb_classes; i++) {
                        cJSON *name = cJSON_GetArrayItem(class_names, i);
                        if (cJSON_IsString(name)) {
                            uint8_t len = strlen(name->valuestring) + 1;
                            pp_ctx->class_names[i] = (char *)hal_mem_alloc_any(sizeof(char) * len);
                            memcpy(pp_ctx->class_names[i], name->valuestring, len);
                        }
                    }
                }

                cJSON *conf_th = cJSON_GetObjectItemCaseSensitive(pp, "confidence_threshold");
                if (cJSON_IsNumber(conf_th)) {
                    params->conf_threshold = (float32_t)conf_th->valuedouble;
                }

                cJSON *iou_th = cJSON_GetObjectItemCaseSensitive(pp, "iou_threshold");
                if (cJSON_IsNumber(iou_th)) {
                    params->iou_threshold = (float32_t)iou_th->valuedouble;
                }

                cJSON *max_det = cJSON_GetObjectItemCaseSensitive(pp, "max_detections");
                if (cJSON_IsNumber(max_det)) {
                    params->max_boxes_limit = (int32_t)max_det->valuedouble;
                }

                cJSON *total_boxes = cJSON_GetObjectItemCaseSensitive(pp, "total_boxes");
                if (cJSON_IsNumber(total_boxes)) {
                    params->nb_total_boxes = (int32_t)total_boxes->valuedouble;
                }

                cJSON *mask_size = cJSON_GetObjectItemCaseSensitive(pp, "mask_size");
                if (cJSON_IsNumber(mask_size)) {
                    params->size_masks = (int32_t)mask_size->valuedouble;
                }

                cJSON *num_masks = cJSON_GetObjectItemCaseSensitive(pp, "num_masks");
                if (cJSON_IsNumber(num_masks)) {
                    params->nb_masks = (int32_t)num_masks->valuedouble;
                }
            }

            cJSON_Delete(root);
        }
    }
    
    // Allocate per-instance output buffers
    pp_ctx->iseg_pp_buffer = (iseg_pp_outBuffer_t *)hal_mem_alloc_large(sizeof(iseg_pp_outBuffer_t) * params->max_boxes_limit);
    pp_ctx->iseg_detect_buffer = (iseg_detect_t *)hal_mem_alloc_large(sizeof(iseg_detect_t) * params->max_boxes_limit);
    
    // Allocate mask buffers
    uint32_t mask_size_per_detection = params->size_masks * params->size_masks;
    pp_ctx->iseg_mask_buffer = (uint8_t *)hal_mem_alloc_large(sizeof(uint8_t) * mask_size_per_detection * params->max_boxes_limit);
    
    // Allocate scratch buffers for int8 processing
    pp_ctx->scratch_detections_buffer = (iseg_yolov8_pp_scratchBuffer_s8_t *)hal_mem_alloc_large(sizeof(iseg_yolov8_pp_scratchBuffer_s8_t) * params->nb_total_boxes);
    pp_ctx->mask_float_buffer = (float32_t *)hal_mem_alloc_large(sizeof(float32_t) * params->nb_masks);
    pp_ctx->mask_int8_buffer = (int8_t *)hal_mem_alloc_large(sizeof(int8_t) * params->nb_masks * params->nb_total_boxes);

    assert(pp_ctx->iseg_pp_buffer != NULL && pp_ctx->iseg_detect_buffer != NULL && pp_ctx->iseg_mask_buffer != NULL);
    assert(pp_ctx->scratch_detections_buffer != NULL && pp_ctx->mask_float_buffer != NULL && pp_ctx->mask_int8_buffer != NULL);
    
    // Initialize mask pointers
    for (size_t i = 0; i < params->max_boxes_limit; i++) {
        pp_ctx->iseg_pp_buffer[i].pMask = &pp_ctx->iseg_mask_buffer[i * mask_size_per_detection];
        pp_ctx->iseg_detect_buffer[i].mask = &pp_ctx->iseg_mask_buffer[i * mask_size_per_detection];
        pp_ctx->iseg_detect_buffer[i].mask_size = mask_size_per_detection;
    }
    
    // Initialize scratch buffer mask pointers
    for (size_t i = 0; i < params->nb_total_boxes; i++) {
        pp_ctx->scratch_detections_buffer[i].pMask = &pp_ctx->mask_int8_buffer[i * params->nb_masks];
    }
    
    // Set scratch buffer pointers
    params->pMask = pp_ctx->mask_float_buffer;
    params->pTmpBuff = pp_ctx->scratch_detections_buffer;
    
    iseg_yolov8_pp_reset(params);
    *pp_params = (void *)pp_ctx;
    return AI_ISEG_POSTPROCESS_ERROR_NO;
}

static int32_t deinit(void *pp_params)
{
    pp_iseg_yolo_v8_ui_params_t *pp = (pp_iseg_yolo_v8_ui_params_t *)pp_params;
    if (!pp) {
        return AI_ISEG_POSTPROCESS_ERROR_NO;
    }

    iseg_yolov8_pp_static_param_t *params = &pp->core;
    
    if (pp->iseg_pp_buffer != NULL) {
        hal_mem_free(pp->iseg_pp_buffer);
        pp->iseg_pp_buffer = NULL;
    }
    
    if (pp->iseg_detect_buffer != NULL) {
        hal_mem_free(pp->iseg_detect_buffer);
        pp->iseg_detect_buffer = NULL;
    }
    
    if (pp->iseg_mask_buffer != NULL) {
        hal_mem_free(pp->iseg_mask_buffer);
        pp->iseg_mask_buffer = NULL;
    }
    
    if (pp->scratch_detections_buffer != NULL) {
        hal_mem_free(pp->scratch_detections_buffer);
        pp->scratch_detections_buffer = NULL;
    }
    
    if (pp->mask_float_buffer != NULL) {
        hal_mem_free(pp->mask_float_buffer);
        pp->mask_float_buffer = NULL;
    }
    
    if (pp->mask_int8_buffer != NULL) {
        hal_mem_free(pp->mask_int8_buffer);
        pp->mask_int8_buffer = NULL;
    }

    if (pp->class_names != NULL) {
        for (int i = 0; i < params->nb_classes; i++) {
            if (pp->class_names[i] != NULL) {
                hal_mem_free(pp->class_names[i]);
            }
        }
        hal_mem_free(pp->class_names);
        pp->class_names = NULL;
    }
    
    hal_mem_free(pp);
    return AI_ISEG_POSTPROCESS_ERROR_NO;
}

static void iseg_pp_out_t_to_pp_result_t(iseg_pp_out_t *pIsegOutput,
                                         pp_result_t *result,
                                         const pp_iseg_yolo_v8_ui_params_t *pp,
                                         float user_conf_threshold)
{
    result->type = PP_TYPE_ISEG;
    result->is_valid = pIsegOutput->nb_detect > 0;
    result->iseg.detects = pp->iseg_detect_buffer;

    /* ATON NPU strips sigmoid, library outputs raw logit confidence.
       Apply sigmoid to get probability, then filter by user threshold. */
    int32_t valid_count = 0;
    for (int i = 0; i < pIsegOutput->nb_detect; i++) {
        float logit_conf = pIsegOutput->pOutBuff[i].conf;
        float prob_conf = 1.0f / (1.0f + expf(-logit_conf));

        if (prob_conf < user_conf_threshold) {
            continue;
        }

        /* Bbox from library is already normalized [0,1] (x_center, y_center, w, h).
           ST model includes bbox normalization in graph. */
        float x_center = pIsegOutput->pOutBuff[i].x_center;
        float y_center = pIsegOutput->pOutBuff[i].y_center;
        float width = pIsegOutput->pOutBuff[i].width;
        float height = pIsegOutput->pOutBuff[i].height;

        result->iseg.detects[valid_count].x = MAX(0.0f, MIN(1.0f, x_center - width / 2.0f));
        result->iseg.detects[valid_count].y = MAX(0.0f, MIN(1.0f, y_center - height / 2.0f));
        result->iseg.detects[valid_count].width = MAX(0.0f, MIN(1.0f, width));
        result->iseg.detects[valid_count].height = MAX(0.0f, MIN(1.0f, height));
        result->iseg.detects[valid_count].conf = prob_conf;

        int cls_idx = pIsegOutput->pOutBuff[i].class_index;
        if (cls_idx >= 0 && cls_idx < pp->core.nb_classes && pp->class_names) {
            result->iseg.detects[valid_count].class_name = pp->class_names[cls_idx];
        } else {
            result->iseg.detects[valid_count].class_name = "unknown";
        }

        /* Copy mask pointer from source detection */
        if (valid_count != i) {
            result->iseg.detects[valid_count].mask = result->iseg.detects[i].mask;
            result->iseg.detects[valid_count].mask_size = result->iseg.detects[i].mask_size;
        }

        valid_count++;
    }

    result->iseg.nb_detect = valid_count;
    result->is_valid = valid_count > 0;
}

static int32_t run(void *pInput[], uint32_t nb_input, void *pResult, void *pp_params, void *nn_inst)
{
    assert(nb_input == 2);
    pp_iseg_yolo_v8_ui_params_t *pp = (pp_iseg_yolo_v8_ui_params_t *)pp_params;
    iseg_yolov8_pp_static_param_t *params = &pp->core;
    int32_t error = AI_ISEG_POSTPROCESS_ERROR_NO;

    params->nb_detect = 0;
    memset(pResult, 0, sizeof(pp_result_t));

    iseg_pp_out_t iseg_pp_out;
    iseg_pp_out.pOutBuff = pp->iseg_pp_buffer;

    /* ATON NPU strips sigmoid, so library outputs raw logit confidence.
       Pass the user's probability threshold directly to the library.
       The library's int8 gate (threshold_s8 = threshold / scale + 0.5 + zp)
       will filter boxes where dequantized logit > threshold, equivalent to
       sigmoid > ~0.56 for threshold=0.25. Exact sigmoid filtering happens
       in iseg_pp_out_t_to_pp_result_t(). */
    float user_conf_threshold = params->conf_threshold;

    /* Use dynamically detected ATON output buffer indices.
       Library expects channel-first [channels, boxes] — pass raw data directly. */
    iseg_yolov8_pp_in_centroid_t pp_input = {
        .pRaw_detections = (int8_t *)pInput[pp->det_output_index],
        .pRaw_masks = (int8_t *)pInput[pp->mask_output_index],
    };

    error = iseg_yolov8_pp_process_int8(&pp_input, &iseg_pp_out, params);

    if (error == AI_ISEG_POSTPROCESS_ERROR_NO) {
        iseg_pp_out_t_to_pp_result_t(&iseg_pp_out, (pp_result_t *)pResult, pp, user_conf_threshold);
    }
    return error;
}

static int32_t set_confidence_threshold(void *params, float threshold)
{
    pp_iseg_yolo_v8_ui_params_t *pp = (pp_iseg_yolo_v8_ui_params_t *)params;
    pp->core.conf_threshold = threshold;
    return AI_ISEG_POSTPROCESS_ERROR_NO;
}

static int32_t set_nms_threshold(void *params, float threshold)
{
    pp_iseg_yolo_v8_ui_params_t *pp = (pp_iseg_yolo_v8_ui_params_t *)params;
    pp->core.iou_threshold = threshold;
    return AI_ISEG_POSTPROCESS_ERROR_NO;
}

static int32_t get_confidence_threshold(void *params, float *threshold)
{
    pp_iseg_yolo_v8_ui_params_t *pp = (pp_iseg_yolo_v8_ui_params_t *)params;
    *threshold = pp->core.conf_threshold;
    return AI_ISEG_POSTPROCESS_ERROR_NO;
}

static int32_t get_nms_threshold(void *params, float *threshold)
{
    pp_iseg_yolo_v8_ui_params_t *pp = (pp_iseg_yolo_v8_ui_params_t *)params;
    *threshold = pp->core.iou_threshold;
    return AI_ISEG_POSTPROCESS_ERROR_NO;
}

static const pp_vtable_t vt = {
    .init = init,
    .run = run,
    .deinit = deinit,
    .set_confidence_threshold = set_confidence_threshold,
    .get_confidence_threshold = get_confidence_threshold,
    .set_nms_threshold = set_nms_threshold,
    .get_nms_threshold = get_nms_threshold,
};

// Define static registration entry
const pp_entry_t pp_entry_iseg_yolo_v8_ui = {
    .name = "pp_iseg_yolo_v8_ui",
    .vt = &vt
};

