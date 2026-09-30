/**
******************************************************************************
* @file    pp_fd_yunet_ui.c
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
#include <string.h>
#include "cJSON.h"
#include "mem.h"
#include "ll_aton_runtime.h"
#include "ll_aton_reloc_network.h"

#include "fd_yunet_pp_if.h"
#if 0
#include "fd_yunet_anchors_32.h"
#include "fd_yunet_anchors_16.h"
#include "fd_yunet_anchors_8.h"
#endif
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

/* Per-instance parameters for FD Yunet UI postprocess (mapped to OD type) */
typedef struct {
    fd_yunet_pp_static_param_t core;   /* original static params, now per-instance */
    fd_pp_outBuffer_t *fd_pp_buffer;   /* internal face detection buffer */
    fd_pp_keyPoints_t *fd_kp_buffer;   /* internal keypoints buffer */
    mpe_detect_t *mpe_detect_buffer;   /* per-instance MPE-style detection buffer */
    char **class_names;                /* per-instance class name array */
    int16_t *anchors_32;               /* generated anchor tables (grid corners) */
    int16_t *anchors_16;
    int16_t *anchors_8;
} pp_fd_yunet_ui_params_t;

/*
YuNet anchor convention (matches OpenCV FaceDetectorYN decode
cx = (col + offset) * stride): one anchor per feature-map cell, laid out
row-major, 2 x int16 pixel values each: {col*stride, row*stride}. The decode
also reads stride from anchors[1].x, which only works with this layout.
The tables are generated at init from in_size, so any square input that is a
multiple of 32 is supported and nb_detections_* are derived, not hardcoded.
*/
static int16_t *fd_yunet_gen_anchor_table(uint32_t in_size, uint32_t stride)
{
    uint32_t dim = in_size / stride;
    int16_t *tbl = (int16_t *)hal_mem_alloc_any(sizeof(int16_t) * 2U * dim * dim);
    if (!tbl) {
        return NULL;
    }
    for (uint32_t r = 0; r < dim; r++) {
        for (uint32_t c = 0; c < dim; c++) {
            tbl[(r * dim + c) * 2U + 0] = (int16_t)(c * stride);
            tbl[(r * dim + c) * 2U + 1] = (int16_t)(r * stride);
        }
    }
    return tbl;
}

/*
Example JSON configuration (all fields optional):
"postprocess_params": {
  "class_names": ["face"],
  "confidence_threshold": 0.6,
  "iou_threshold": 0.5,
  "max_detections": 100,
  "image_size": 320
}
nb_detections_8/16/32 are accepted for compatibility but ignored: they are
derived from image_size together with the generated anchor tables.
*/
static int32_t init(const char *json_str, void **pp_params, void *nn_inst)
{
    pp_fd_yunet_ui_params_t *pp_ctx =
        (pp_fd_yunet_ui_params_t *)hal_mem_alloc_any(sizeof(pp_fd_yunet_ui_params_t));
    if (!pp_ctx) {
        return AI_FD_PP_ERROR_NO;
    }
    memset(pp_ctx, 0, sizeof(pp_fd_yunet_ui_params_t));

    fd_yunet_pp_static_param_t *params = &pp_ctx->core;

    /* Default parameters (self-contained, can be overridden by JSON) */
    params->in_size          = 320;   /* typical Yunet input size */
    params->nb_keypoints     = 5;     /* eyes, nose, mouth corners */
    params->nb_detections_32 = 168;
    params->nb_detections_16 = 672;
    params->nb_detections_8  = 2688;
    params->max_boxes_limit  = 100;
    params->allocated_boxes  = 100;
    params->conf_threshold   = 0.6f;
    params->iou_threshold    = 0.5f;
    params->pAnchors_32      = NULL;  /* can be provided via JSON if needed */
    params->pAnchors_16      = NULL;
    params->pAnchors_8       = NULL;

    /* Quantization parameters from NN instance (12 outputs) */
    NN_Instance_TypeDef *NN_Instance = (NN_Instance_TypeDef *)nn_inst;
    if (NN_Instance != NULL) {
        const LL_Buffer_InfoTypeDef *buffers_info =
            ll_aton_reloc_get_output_buffers_info(NN_Instance, 0);
        if (buffers_info != NULL && buffers_info[0].scale != NULL) {
            /* Stride 32 */
            params->cls_32_scale      = *(buffers_info[2].scale);
            params->cls_32_zero_point = (int8_t)(*buffers_info[2].offset);
            params->obj_32_scale      = *(buffers_info[5].scale);
            params->obj_32_zero_point = (int8_t)(*buffers_info[5].offset);
            params->bbx_32_scale      = *(buffers_info[8].scale);
            params->bbx_32_zero_point = (int8_t)(*buffers_info[8].offset);
            params->kps_32_scale      = *(buffers_info[11].scale);
            params->kps_32_zero_point = (int8_t)(*buffers_info[11].offset);

            /* Stride 16 */
            params->cls_16_scale      = *(buffers_info[1].scale);
            params->cls_16_zero_point = (int8_t)(*buffers_info[1].offset);
            params->obj_16_scale      = *(buffers_info[4].scale);
            params->obj_16_zero_point = (int8_t)(*buffers_info[4].offset);
            params->bbx_16_scale      = *(buffers_info[7].scale);
            params->bbx_16_zero_point = (int8_t)(*buffers_info[7].offset);
            params->kps_16_scale      = *(buffers_info[10].scale);
            params->kps_16_zero_point = (int8_t)(*buffers_info[10].offset);

            /* Stride 8 */
            params->cls_8_scale       = *(buffers_info[0].scale);
            params->cls_8_zero_point  = (int8_t)(*buffers_info[0].offset);
            params->obj_8_scale       = *(buffers_info[3].scale);
            params->obj_8_zero_point  = (int8_t)(*buffers_info[3].offset);
            params->bbx_8_scale       = *(buffers_info[6].scale);
            params->bbx_8_zero_point  = (int8_t)(*buffers_info[6].offset);
            params->kps_8_scale       = *(buffers_info[9].scale);
            params->kps_8_zero_point  = (int8_t)(*buffers_info[9].offset);
        }
    }

    /* Optional JSON overrides (mainly thresholds and class name) */
    if (json_str != NULL) {
        cJSON *root = cJSON_Parse(json_str);
        if (root != NULL) {
            cJSON *pp = cJSON_GetObjectItemCaseSensitive(root, "postprocess_params");
            if (pp == NULL) {
                pp = root;
            }

            if (cJSON_IsObject(pp)) {
                cJSON *class_names = cJSON_GetObjectItemCaseSensitive(pp, "class_names");
                if (cJSON_IsArray(class_names)) {
                    int nb = cJSON_GetArraySize(class_names);
                    if (nb > 0) {
                        pp_ctx->class_names =
                            (char **)hal_mem_alloc_any(sizeof(char *) * (size_t)nb);
                        for (int i = 0; i < nb; i++) {
                            cJSON *name = cJSON_GetArrayItem(class_names, i);
                            if (cJSON_IsString(name)) {
                                uint8_t len = (uint8_t)strlen(name->valuestring) + 1U;
                                pp_ctx->class_names[i] =
                                    (char *)hal_mem_alloc_any(sizeof(char) * len);
                                memcpy(pp_ctx->class_names[i], name->valuestring, len);
                            }
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
                    params->max_boxes_limit = (uint32_t)max_det->valuedouble;
                    params->allocated_boxes = MAX(params->allocated_boxes, params->max_boxes_limit);
                }

                cJSON *img_size = cJSON_GetObjectItemCaseSensitive(pp, "image_size");
                if (cJSON_IsNumber(img_size)) {
                    params->in_size = (uint32_t)img_size->valuedouble;
                }

                cJSON *nbd8 = cJSON_GetObjectItemCaseSensitive(pp, "nb_detections_8");
                if (cJSON_IsNumber(nbd8)) {
                    params->nb_detections_8 = (uint32_t)nbd8->valuedouble;
                }

                cJSON *nbd16 = cJSON_GetObjectItemCaseSensitive(pp, "nb_detections_16");
                if (cJSON_IsNumber(nbd16)) {
                    params->nb_detections_16 = (uint32_t)nbd16->valuedouble;
                }

                cJSON *nbd32 = cJSON_GetObjectItemCaseSensitive(pp, "nb_detections_32");
                if (cJSON_IsNumber(nbd32)) {
                    params->nb_detections_32 = (uint32_t)nbd32->valuedouble;
                }
            }

            cJSON_Delete(root);
        }
    }

    /* Generate the per-stride anchor tables from the final in_size and make
     * nb_detections_* consistent with them (grid cells per level). */
    if (params->in_size == 0 || (params->in_size % 32U) != 0) {
        hal_mem_free(pp_ctx);
        return AI_FD_PP_ERROR;
    }
    pp_ctx->anchors_32 = fd_yunet_gen_anchor_table(params->in_size, 32U);
    pp_ctx->anchors_16 = fd_yunet_gen_anchor_table(params->in_size, 16U);
    pp_ctx->anchors_8  = fd_yunet_gen_anchor_table(params->in_size, 8U);
    if (!pp_ctx->anchors_32 || !pp_ctx->anchors_16 || !pp_ctx->anchors_8) {
        if (pp_ctx->anchors_32) hal_mem_free(pp_ctx->anchors_32);
        if (pp_ctx->anchors_16) hal_mem_free(pp_ctx->anchors_16);
        if (pp_ctx->anchors_8) hal_mem_free(pp_ctx->anchors_8);
        hal_mem_free(pp_ctx);
        return AI_FD_PP_ERROR;
    }
    params->pAnchors_32 = pp_ctx->anchors_32;
    params->pAnchors_16 = pp_ctx->anchors_16;
    params->pAnchors_8  = pp_ctx->anchors_8;
    params->nb_detections_32 = (params->in_size / 32U) * (params->in_size / 32U);
    params->nb_detections_16 = (params->in_size / 16U) * (params->in_size / 16U);
    params->nb_detections_8  = (params->in_size / 8U) * (params->in_size / 8U);

    /* Allocate per-instance buffers */
    size_t max_boxes = params->allocated_boxes;
    pp_ctx->fd_pp_buffer =
        (fd_pp_outBuffer_t *)hal_mem_alloc_large(sizeof(fd_pp_outBuffer_t) * max_boxes);
    pp_ctx->fd_kp_buffer =
        (fd_pp_keyPoints_t *)hal_mem_alloc_large(sizeof(fd_pp_keyPoints_t) * max_boxes * 2U *
                                                 (size_t)params->nb_keypoints);
    pp_ctx->mpe_detect_buffer =
        (mpe_detect_t *)hal_mem_alloc_large(sizeof(mpe_detect_t) * params->max_boxes_limit);

    assert(pp_ctx->fd_pp_buffer != NULL &&
           pp_ctx->fd_kp_buffer != NULL &&
           pp_ctx->mpe_detect_buffer != NULL);

    /* Initialize keypoints pointers for fd output */
    for (size_t i = 0; i < max_boxes; i++) {
        pp_ctx->fd_pp_buffer[i].pKeyPoints =
            &pp_ctx->fd_kp_buffer[i * 2U * (size_t)params->nb_keypoints];
    }

    fd_yunet_pp_reset(params);
    *pp_params = (void *)pp_ctx;
    return AI_FD_PP_ERROR_NO;
}

static int32_t deinit(void *pp_params)
{
    pp_fd_yunet_ui_params_t *pp = (pp_fd_yunet_ui_params_t *)pp_params;
    if (!pp) {
        return AI_FD_PP_ERROR_NO;
    }

    fd_yunet_pp_static_param_t *params = &pp->core;
    (void)params;

    if (pp->fd_pp_buffer != NULL) {
        hal_mem_free(pp->fd_pp_buffer);
        pp->fd_pp_buffer = NULL;
    }
    if (pp->fd_kp_buffer != NULL) {
        hal_mem_free(pp->fd_kp_buffer);
        pp->fd_kp_buffer = NULL;
    }
    if (pp->mpe_detect_buffer != NULL) {
        hal_mem_free(pp->mpe_detect_buffer);
        pp->mpe_detect_buffer = NULL;
    }
    if (pp->anchors_32 != NULL) {
        hal_mem_free(pp->anchors_32);
        pp->anchors_32 = NULL;
    }
    if (pp->anchors_16 != NULL) {
        hal_mem_free(pp->anchors_16);
        pp->anchors_16 = NULL;
    }
    if (pp->anchors_8 != NULL) {
        hal_mem_free(pp->anchors_8);
        pp->anchors_8 = NULL;
    }
    if (pp->class_names != NULL) {
        /* Yunet usually has single class "face" */
        if (pp->class_names[0] != NULL) {
            hal_mem_free(pp->class_names[0]);
        }
        hal_mem_free(pp->class_names);
        pp->class_names = NULL;
    }

    hal_mem_free(pp);
    return AI_FD_PP_ERROR_NO;
}

/* YuNet emits 5 landmarks (OpenCV order): right/left eye, nose tip,
 * right/left mouth corner. Connections: eyes, eyes-nose, mouth corners. */
#define FD_YUNET_NUM_KEYPOINTS 5
#define FD_YUNET_NUM_CONNECTIONS 4
static const char *fd_kp_names[FD_YUNET_NUM_KEYPOINTS] = {
    "right_eye", "left_eye", "nose", "mouth_right", "mouth_left"
};
static const uint8_t fd_kp_connections[FD_YUNET_NUM_CONNECTIONS * 2] = {
    0, 1,  /* eyes */
    0, 2,  /* right eye - nose */
    1, 2,  /* left eye - nose */
    3, 4   /* mouth corners */
};

static void fd_pp_out_t_to_pp_result_t(fd_pp_out_t *pFdOutput,
                                       pp_result_t *result,
                                       const pp_fd_yunet_ui_params_t *pp)
{
    /* MPE flavor (same contract as pp_fd_blazeface_ui): carries the 5
     * keypoints through to drawers/telemetry; consumers that only need
     * boxes read x/y/width/height/conf identically. */
    result->type = PP_TYPE_MPE;
    result->is_valid = pFdOutput->nb_detect > 0;
    result->mpe.nb_detect = (uint8_t)pFdOutput->nb_detect;
    result->mpe.detects = pp->mpe_detect_buffer;

    for (int i = 0; i < pFdOutput->nb_detect; i++) {
        float32_t x_center = pFdOutput->pOutBuff[i].x_center;
        float32_t y_center = pFdOutput->pOutBuff[i].y_center;
        float32_t width    = pFdOutput->pOutBuff[i].width;
        float32_t height   = pFdOutput->pOutBuff[i].height;

        result->mpe.detects[i].x =
            MAX(0.0f, MIN(1.0f, x_center - width / 2.0f));
        result->mpe.detects[i].y =
            MAX(0.0f, MIN(1.0f, y_center - height / 2.0f));
        result->mpe.detects[i].width =
            MAX(0.0f, MIN(1.0f, width));
        result->mpe.detects[i].height =
            MAX(0.0f, MIN(1.0f, height));
        result->mpe.detects[i].conf =
            MAX(0.0f, MIN(1.0f, pFdOutput->pOutBuff[i].conf));

        if (pp->class_names && pp->class_names[0]) {
            result->mpe.detects[i].class_name = pp->class_names[0];
        } else {
            result->mpe.detects[i].class_name = "face";
        }

        /* fd_pp_keyPoints_t carries no confidence; parent box passed the
         * threshold, so report 1.0. */
        result->mpe.detects[i].nb_keypoints = FD_YUNET_NUM_KEYPOINTS;
        result->mpe.detects[i].keypoint_names = (char **)fd_kp_names;
        result->mpe.detects[i].num_connections = FD_YUNET_NUM_CONNECTIONS;
        result->mpe.detects[i].keypoint_connections = (uint8_t *)fd_kp_connections;
        memset(result->mpe.detects[i].keypoints + FD_YUNET_NUM_KEYPOINTS, 0,
               sizeof(keypoint_t) * (33 - FD_YUNET_NUM_KEYPOINTS));

        if (pFdOutput->pOutBuff[i].pKeyPoints != NULL) {
            for (int j = 0; j < FD_YUNET_NUM_KEYPOINTS; j++) {
                float32_t kx = pFdOutput->pOutBuff[i].pKeyPoints[j].x;
                float32_t ky = pFdOutput->pOutBuff[i].pKeyPoints[j].y;
                result->mpe.detects[i].keypoints[j].x = MAX(0.0f, MIN(1.0f, kx));
                result->mpe.detects[i].keypoints[j].y = MAX(0.0f, MIN(1.0f, ky));
                result->mpe.detects[i].keypoints[j].conf = 1.0f;
            }
        }
    }
}

static int32_t run(void *pInput[], uint32_t nb_input, void *pResult, void *pp_params, void *nn_inst)
{
    (void)nn_inst;

    /* Output count mismatch means the model variant does not match this
     * postprocess (or the framework truncated the buffer list). Fail the
     * inference instead of halting the device on an assert. */
    if (nb_input != 12) {
        return AI_FD_PP_ERROR;
    }
    pp_fd_yunet_ui_params_t *pp = (pp_fd_yunet_ui_params_t *)pp_params;
    fd_yunet_pp_static_param_t *params = &pp->core;
    int32_t error = AI_FD_PP_ERROR_NO;
    params->nb_detect = 0;

    memset(pResult, 0, sizeof(pp_result_t));

    fd_pp_out_t fd_out;
    fd_out.pOutBuff = pp->fd_pp_buffer;

    fd_yunet_pp_in_t pp_input = {
        .pCls_32      = (int8_t *)pInput[2],
        .pObj_32      = (int8_t *)pInput[5],
        .pBBoxRaw_32  = (int8_t *)pInput[8],
        .pKpsRaw_32   = (int8_t *)pInput[11],
        .pCls_16      = (int8_t *)pInput[1],
        .pObj_16      = (int8_t *)pInput[4],
        .pBBoxRaw_16  = (int8_t *)pInput[7],
        .pKpsRaw_16   = (int8_t *)pInput[10],
        .pCls_8       = (int8_t *)pInput[0],
        .pObj_8       = (int8_t *)pInput[3],
        .pBBoxRaw_8   = (int8_t *)pInput[6],
        .pKpsRaw_8    = (int8_t *)pInput[9],
    };

    error = fd_yunet_pp_process_int8(&pp_input, &fd_out, params);
    if (error == AI_FD_PP_ERROR_NO) {
        fd_pp_out_t_to_pp_result_t(&fd_out, (pp_result_t *)pResult, pp);
    }
    return error;
}

static int32_t set_confidence_threshold(void *params, float threshold)
{
    pp_fd_yunet_ui_params_t *pp = (pp_fd_yunet_ui_params_t *)params;
    pp->core.conf_threshold = threshold;
    return AI_FD_PP_ERROR_NO;
}

static int32_t set_nms_threshold(void *params, float threshold)
{
    pp_fd_yunet_ui_params_t *pp = (pp_fd_yunet_ui_params_t *)params;
    pp->core.iou_threshold = threshold;
    return AI_FD_PP_ERROR_NO;
}

static int32_t get_confidence_threshold(void *params, float *threshold)
{
    pp_fd_yunet_ui_params_t *pp = (pp_fd_yunet_ui_params_t *)params;
    *threshold = pp->core.conf_threshold;
    return AI_FD_PP_ERROR_NO;
}

static int32_t get_nms_threshold(void *params, float *threshold)
{
    pp_fd_yunet_ui_params_t *pp = (pp_fd_yunet_ui_params_t *)params;
    *threshold = pp->core.iou_threshold;
    return AI_FD_PP_ERROR_NO;
}

static const pp_vtable_t vt = {
    .init                     = init,
    .run                      = run,
    .deinit                   = deinit,
    .set_confidence_threshold = set_confidence_threshold,
    .get_confidence_threshold = get_confidence_threshold,
    .set_nms_threshold        = set_nms_threshold,
    .get_nms_threshold        = get_nms_threshold,
};

/* Static registration entry */
const pp_entry_t pp_entry_fd_yunet_ui = {
    .name = "pp_fd_yunet_ui",
    .vt   = &vt
};

