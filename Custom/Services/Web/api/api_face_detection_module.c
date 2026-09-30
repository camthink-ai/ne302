/**
 * @file api_face_detection_module.c
 * @brief Face Detection demo Web API module
 * @details Routes:
 *          GET  /api/v1/face/config  - current demo configuration
 *          POST /api/v1/face/config  - partial configuration update (merge)
 *          GET  /api/v1/face/status  - live statistics + active model info
 *          GET  /api/v1/face/results - visible tracks + recognition state
 *          POST /api/v1/face/trigger - force a report (all or one track)
 *          POST /api/v1/face/reset   - clear statistics and caches
 */

#include "web_server.h"
#include "web_api.h"
#include "debug.h"
#include "face_detection.h"
#include "ai_service.h"
#include "json_config_mgr.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include "buffer_mgr.h"
#include "common_utils.h"

/* ==================== Internal Helpers ==================== */

/**
 * @brief Read one optional numeric config field with range validation
 * @return 1 applied, 0 not present, -1 invalid (out of range / wrong type)
 */
static int face_apply_uint32(cJSON *request, const char *key,
                             uint32_t *field, uint32_t min, uint32_t max)
{
    cJSON *item = cJSON_GetObjectItem(request, key);
    if (!item) {
        return 0;
    }
    if (!cJSON_IsNumber(item)) {
        return -1;
    }
    double value = cJSON_GetNumberValue(item);
    if (value < (double)min || value > (double)max) {
        return -1;
    }
    *field = (uint32_t)value;
    return 1;
}

/**
 * @brief Read one optional float config field (stored as x100 uint32)
 * @return 1 applied, 0 not present, -1 invalid
 */
static int face_apply_float_x100(cJSON *request, const char *key,
                                 uint32_t *field, double min, double max)
{
    cJSON *item = cJSON_GetObjectItem(request, key);
    if (!item) {
        return 0;
    }
    if (!cJSON_IsNumber(item)) {
        return -1;
    }
    double value = cJSON_GetNumberValue(item);
    if (value < min || value > max) {
        return -1;
    }
    *field = (uint32_t)(value * 100.0 + 0.5);
    return 1;
}

/**
 * @brief Read one optional boolean config field
 * @return 1 applied, 0 not present, -1 wrong type
 */
static int face_apply_bool(cJSON *request, const char *key, aicam_bool_t *field)
{
    cJSON *item = cJSON_GetObjectItem(request, key);
    if (!item) {
        return 0;
    }
    if (!cJSON_IsBool(item)) {
        return -1;
    }
    *field = cJSON_IsTrue(item) ? AICAM_TRUE : AICAM_FALSE;
    return 1;
}

/**
 * @brief Serialize the config to the response JSON shape (doc section 4)
 */
static cJSON *face_config_to_json(const face_detection_config_t *cfg)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "enabled", cfg->enabled);
    cJSON_AddNumberToObject(data, "detection_conf_threshold",
                            (double)cfg->detection_conf_threshold / 100.0);
    cJSON_AddNumberToObject(data, "min_face_area", cfg->min_face_area);
    cJSON_AddNumberToObject(data, "track_iou_threshold",
                            (double)cfg->track_iou_threshold / 100.0);
    cJSON_AddNumberToObject(data, "track_max_missed", cfg->track_max_missed);
    cJSON_AddNumberToObject(data, "track_confirm_frames", cfg->track_confirm_frames);
    cJSON_AddNumberToObject(data, "face_hold_ms", cfg->face_hold_ms);
    cJSON_AddBoolToObject(data, "extrapolation_enabled", cfg->extrapolation_enabled);
    cJSON_AddBoolToObject(data, "blur_enabled", cfg->blur_enabled);
    cJSON_AddNumberToObject(data, "mosaic_block", cfg->mosaic_block);
    cJSON_AddNumberToObject(data, "blur_expand_ratio",
                            (double)cfg->blur_expand_ratio / 100.0);
    cJSON_AddNumberToObject(data, "crop_expand_ratio",
                            (double)cfg->crop_expand_ratio / 100.0);
    cJSON_AddNumberToObject(data, "crop_min_size", cfg->crop_min_size);
    cJSON_AddNumberToObject(data, "report_fixed_size", cfg->report_fixed_size);
    cJSON_AddNumberToObject(data, "jpeg_quality", cfg->jpeg_quality);
    cJSON_AddBoolToObject(data, "report_enabled", cfg->report_enabled);
    cJSON_AddNumberToObject(data, "report_channel", cfg->report_channel);
    cJSON_AddNumberToObject(data, "report_confidence_threshold",
                            (double)cfg->report_confidence_threshold / 100.0);
    cJSON_AddBoolToObject(data, "report_periodic_enabled", cfg->report_periodic_enabled);
    cJSON_AddNumberToObject(data, "report_cooldown_ms", cfg->report_cooldown_ms);
    cJSON_AddNumberToObject(data, "result_timeout_ms", cfg->result_timeout_ms);
    cJSON_AddNumberToObject(data, "result_expire_ms", cfg->result_expire_ms);
    cJSON_AddBoolToObject(data, "show_box", cfg->show_box);
    cJSON_AddBoolToObject(data, "show_id", cfg->show_id);
    cJSON_AddBoolToObject(data, "show_confidence", cfg->show_confidence);
    cJSON_AddBoolToObject(data, "show_landmarks", cfg->show_landmarks);
    cJSON_AddBoolToObject(data, "show_recognition", cfg->show_recognition);
    return data;
}

/**
 * @brief Apply a validated config to the runtime module and NVS
 */
static aicam_result_t face_apply_config(const face_detection_config_t *config)
{
    /* Runtime module first (takes effect on the next frame) */
    aicam_result_t result = face_detection_set_config(config);
    if (result != AICAM_OK) {
        return result;
    }
    /* Persist */
    return json_config_set_face_detection_config(config);
}

/* ==================== Handlers ==================== */

/* Minimal memmem for multipart scanning (model-validation module keeps a
 * private copy too) */
static const char *memfind(const char *haystack, size_t haystack_len,
                           const char *needle, size_t needle_len)
{
    if (!haystack || !needle || needle_len == 0 || haystack_len < needle_len) {
        return NULL;
    }
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (haystack[i] == needle[0] &&
            memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

/**
 * @brief GET /api/v1/face/config
 */
static aicam_result_t face_config_get_handler(http_handler_context_t *ctx)
{
    if (!ctx) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!web_api_verify_method(ctx, "GET")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    face_detection_config_t cfg;
    if (json_config_get_face_detection_config(&cfg) != AICAM_OK) {
        face_detection_get_config(&cfg);
    }

    cJSON *data = face_config_to_json(&cfg);
    char *json_string = cJSON_Print(data);
    cJSON_Delete(data);

    return api_response_success(ctx, json_string, "Face detection configuration retrieved");
}

/**
 * @brief POST /api/v1/face/config (partial merge, validated)
 */
static aicam_result_t face_config_set_handler(http_handler_context_t *ctx)
{
    if (!ctx) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }
    if (!web_api_verify_content_type(ctx, "application/json")) {
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "Invalid Content-Type");
    }

    cJSON *request = web_api_parse_body(ctx);
    if (!request) {
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "Invalid JSON");
    }

    /* Start from the current persisted config and merge provided keys */
    face_detection_config_t cfg;
    if (json_config_get_face_detection_config(&cfg) != AICAM_OK) {
        face_detection_get_config(&cfg);
    }

    cJSON *errors = cJSON_CreateArray();
    int applied = 0;
    int invalid = 0;

    struct {
        const char *key;
        int (*apply)(cJSON *, const char *, uint32_t *, uint32_t, uint32_t);
        uint32_t *field;
        uint32_t min;
        uint32_t max;
    } uint_fields[] = {
        {"min_face_area",           face_apply_uint32, &cfg.min_face_area,           0,    1000000},
        {"track_max_missed",        face_apply_uint32, &cfg.track_max_missed,        1,    50},
        {"track_confirm_frames",    face_apply_uint32, &cfg.track_confirm_frames,    1,    20},
        {"face_hold_ms",            face_apply_uint32, &cfg.face_hold_ms,            100,  2000},
        {"mosaic_block",            face_apply_uint32, &cfg.mosaic_block,            4,    64},
        {"crop_min_size",           face_apply_uint32, &cfg.crop_min_size,           8,    512},
        {"report_fixed_size",       face_apply_uint32, &cfg.report_fixed_size,       0,    FD_REPORT_FIXED_SIZE_MAX},
        {"jpeg_quality",            face_apply_uint32, &cfg.jpeg_quality,            10,   95},
        {"report_channel",          face_apply_uint32, &cfg.report_channel,          0,    FD_REPORT_CHANNEL_MQTT},
        {"report_cooldown_ms",      face_apply_uint32, &cfg.report_cooldown_ms,      100,  3600000},
        {"result_timeout_ms",       face_apply_uint32, &cfg.result_timeout_ms,       100,  60000},
        {"result_expire_ms",        face_apply_uint32, &cfg.result_expire_ms,        1000, 3600000},
    };
    for (size_t i = 0; i < sizeof(uint_fields) / sizeof(uint_fields[0]); i++) {
        int rc = uint_fields[i].apply(request, uint_fields[i].key,
                                      uint_fields[i].field,
                                      uint_fields[i].min, uint_fields[i].max);
        if (rc > 0) {
            applied++;
        } else if (rc < 0) {
            invalid++;
            cJSON_AddItemToArray(errors, cJSON_CreateString(uint_fields[i].key));
        }
    }

    /* Float fields stored as percent-x100 */
    struct {
        const char *key;
        uint32_t *field;
        double min;
        double max;
    } float_fields[] = {
        {"detection_conf_threshold",    &cfg.detection_conf_threshold,    0.01, 0.99},
        {"track_iou_threshold",         &cfg.track_iou_threshold,         0.01, 0.99},
        {"blur_expand_ratio",           &cfg.blur_expand_ratio,           0.0,  1.0},
        {"crop_expand_ratio",           &cfg.crop_expand_ratio,           0.0,  1.0},
        {"report_confidence_threshold", &cfg.report_confidence_threshold, 0.01, 0.99},
    };
    for (size_t i = 0; i < sizeof(float_fields) / sizeof(float_fields[0]); i++) {
        int rc = face_apply_float_x100(request, float_fields[i].key,
                                       float_fields[i].field,
                                       float_fields[i].min, float_fields[i].max);
        if (rc > 0) {
            applied++;
        } else if (rc < 0) {
            invalid++;
            cJSON_AddItemToArray(errors, cJSON_CreateString(float_fields[i].key));
        }
    }

    /* Boolean fields */
    struct {
        const char *key;
        aicam_bool_t *field;
    } bool_fields[] = {
        {"enabled",               &cfg.enabled},
        {"extrapolation_enabled", &cfg.extrapolation_enabled},
        {"blur_enabled",          &cfg.blur_enabled},
        {"report_enabled",        &cfg.report_enabled},
        {"report_periodic_enabled", &cfg.report_periodic_enabled},
        {"show_box",              &cfg.show_box},
        {"show_id",               &cfg.show_id},
        {"show_confidence",       &cfg.show_confidence},
        {"show_landmarks",        &cfg.show_landmarks},
        {"show_recognition",      &cfg.show_recognition},
    };
    for (size_t i = 0; i < sizeof(bool_fields) / sizeof(bool_fields[0]); i++) {
        int rc = face_apply_bool(request, bool_fields[i].key, bool_fields[i].field);
        if (rc > 0) {
            applied++;
        } else if (rc < 0) {
            invalid++;
            cJSON_AddItemToArray(errors, cJSON_CreateString(bool_fields[i].key));
        }
    }

    cJSON_Delete(request);

    if (invalid > 0) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddItemToObject(data, "invalid_fields", errors);
        char *json_string = cJSON_Print(data);
        cJSON_Delete(data);
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, json_string);
    }
    cJSON_Delete(errors);

    aicam_result_t result = face_apply_config(&cfg);
    if (result != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to apply configuration");
    }

    cJSON *data = face_config_to_json(&cfg);
    char *json_string = cJSON_Print(data);
    cJSON_Delete(data);
    (void)applied;
    return api_response_success(ctx, json_string, "Face detection configuration updated");
}

/**
 * @brief GET /api/v1/face/status
 */
static aicam_result_t face_status_handler(http_handler_context_t *ctx)
{
    if (!ctx) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!web_api_verify_method(ctx, "GET")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    face_detection_stats_t stats;
    face_detection_get_stats(&stats);

    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "enabled", stats.enabled);
    cJSON_AddNumberToObject(data, "confirmed_tracks", stats.confirmed_tracks);
    cJSON_AddNumberToObject(data, "total_tracks", stats.total_tracks);
    cJSON_AddNumberToObject(data, "last_detections", stats.last_detections);
    cJSON_AddNumberToObject(data, "reports_queued", stats.reports_queued);
    cJSON_AddNumberToObject(data, "reports_sent", stats.reports_sent);
    cJSON_AddNumberToObject(data, "reports_dropped", stats.reports_dropped);
    cJSON_AddNumberToObject(data, "reports_failed", stats.reports_failed);
    cJSON_AddNumberToObject(data, "results_ok", stats.results_ok);
    cJSON_AddNumberToObject(data, "results_unknown", stats.results_unknown);
    cJSON_AddNumberToObject(data, "results_timeout", stats.results_timeout);
    cJSON_AddNumberToObject(data, "last_update_ms", stats.last_update_ms);

    /* Active model info — the demo runs on the single active model */
    nn_model_info_t model_info;
    if (ai_service_get_model_info(&model_info) == AICAM_OK) {
        cJSON *model = cJSON_CreateObject();
        cJSON_AddStringToObject(model, "name", model_info.name);
        cJSON_AddNumberToObject(model, "input_width", model_info.input_width);
        cJSON_AddNumberToObject(model, "input_height", model_info.input_height);
        cJSON_AddStringToObject(model, "postprocess_type", model_info.postprocess_type);
        cJSON_AddItemToObject(data, "model", model);
    }

    char *json_string = cJSON_Print(data);
    cJSON_Delete(data);

    return api_response_success(ctx, json_string, "Face detection status");
}

/**
 * @brief GET /api/v1/face/results
 */
static aicam_result_t face_results_handler(http_handler_context_t *ctx)
{
    if (!ctx) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!web_api_verify_method(ctx, "GET")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    face_detection_results_t results;
    aicam_result_t rc = face_detection_get_results(&results);
    if (rc != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to snapshot tracks");
    }

    static const char *status_names[] = {
        "none", "pending", "ok", "unknown", "timeout", "error"
    };

    cJSON *data = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (uint32_t i = 0; i < results.count; i++) {
        const face_detection_track_info_t *t = &results.tracks[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "track_id", t->track_id);
        cJSON *bbox = cJSON_CreateDoubleArray(
            (const double[]){(double)t->x, (double)t->y,
                             (double)t->w, (double)t->h}, 4);
        cJSON_AddItemToObject(item, "bbox", bbox);
        cJSON_AddNumberToObject(item, "detection_confidence",
                                (double)((int)(t->detection_confidence * 1000.0f + 0.5f)) / 1000.0);
        if (t->nb_landmarks > 0) {
            cJSON *lms = cJSON_CreateArray();
            for (uint32_t k = 0; k < t->nb_landmarks; k++) {
                cJSON_AddItemToArray(lms, cJSON_CreateDoubleArray(
                    (const double[]){(double)t->landmarks[k][0],
                                     (double)t->landmarks[k][1]}, 2));
            }
            cJSON_AddItemToObject(item, "landmarks", lms);
        } else {
            cJSON_AddNullToObject(item, "landmarks");
        }
        cJSON *res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "status",
            (t->result_status <= FD_RESULT_ERROR) ? status_names[t->result_status] : "none");
        cJSON_AddStringToObject(res, "name", t->result_name);
        cJSON_AddNumberToObject(res, "confidence",
                                (double)((int)(t->result_confidence * 1000.0f + 0.5f)) / 1000.0);
        cJSON_AddItemToObject(item, "recognition", res);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(data, "tracks", arr);
    cJSON_AddNumberToObject(data, "count", results.count);

    char *json_string = cJSON_Print(data);
    cJSON_Delete(data);

    return api_response_success(ctx, json_string, "Face detection results");
}

/**
 * @brief POST /api/v1/face/trigger  body: {"track_id": 0}  (0/absent = all)
 */
static aicam_result_t face_trigger_handler(http_handler_context_t *ctx)
{
    if (!ctx) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    uint32_t track_id = 0;
    cJSON *request = web_api_parse_body(ctx);
    if (request) {
        cJSON *item = cJSON_GetObjectItem(request, "track_id");
        if (cJSON_IsNumber(item) && item->valuedouble >= 0) {
            track_id = (uint32_t)item->valuedouble;
        }
        cJSON_Delete(request);
    }

    uint32_t armed = face_detection_trigger_report(track_id);

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "armed", armed);
    char *json_string = cJSON_Print(data);
    cJSON_Delete(data);

    return api_response_success(ctx, json_string, "Face reports armed");
}

/**
 * @brief POST /api/v1/face/reset
 */
static aicam_result_t face_reset_handler(http_handler_context_t *ctx)
{
    if (!ctx) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    aicam_result_t result = face_detection_reset();
    if (result != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to reset");
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "success", 1);
    char *json_string = cJSON_Print(data);
    cJSON_Delete(data);

    return api_response_success(ctx, json_string, "Face detection statistics reset");
}


/**
 * @brief POST /api/v1/face/verify  multipart file "image"
 * @details Runs a user-uploaded face photo through the recognition channel
 *          (webhook sync / MQTT reply) and returns the verdict, so the
 *          recognition effect can be checked without standing in front of
 *          the camera.
 */
static aicam_result_t face_verify_handler(http_handler_context_t *ctx)
{
    if (!ctx) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }
    if (!web_api_verify_content_type(ctx, "multipart/form-data")) {
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "Invalid Content-Type");
    }

    const char *ctype = ctx->request.content_type;
    const char *bp = strstr(ctype, "boundary=");
    if (!bp) {
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "Missing boundary");
    }
    bp += 9;

    /* Minimal multipart scan: find the first file part's payload */
    const char *body = ctx->request.body;
    size_t body_len = ctx->request.content_length;
    char bnd[128];
    snprintf(bnd, sizeof(bnd), "--%s", bp);
    size_t bnd_len = strlen(bnd);
    const char *p = body;
    const char *end = body + body_len;
    const uint8_t *img = NULL;
    uint32_t img_size = 0;

    while (p + bnd_len < end) {
        const char *b = memfind(p, (uint32_t)(end - p), bnd, (uint32_t)bnd_len);
        if (!b) {
            break;
        }
        p = b + bnd_len;
        if (end - p >= 2 && p[0] == '-' && p[1] == '-') {
            break; /* closing boundary */
        }
        while (p < end && (*p == 0x0D || *p == 0x0A)) {
            p++;
        }
        const char *hdr_end = memfind(p, (uint32_t)(end - p), "\r\n\r\n", 4);
        if (!hdr_end) {
            break;
        }
        size_t hdr_len = (size_t)(hdr_end - p);
        const char *data = hdr_end + 4;
        const char *next = memfind(data, (uint32_t)(end - data), bnd, (uint32_t)bnd_len);
        if (!next) {
            next = end;
        }
        /* strip the trailing CRLF that precedes the next boundary */
        uint32_t payload = (uint32_t)(next - data);
        if (payload >= 2 && data[payload - 1] == 0x0A && data[payload - 2] == 0x0D) {
            payload -= 2;
        }
        /* accept any part that is not purely metadata text */
        if (payload > 0 && (hdr_len > 4096 || payload > 64)) {
            img = (const uint8_t *)data;
            img_size = payload;
            break;
        }
        (void)hdr_len;
        p = next;
    }

    if (!img || img_size == 0) {
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "No image part found");
    }
    if (img_size > 512 * 1024) {
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "Image too large (max 512KB)");
    }

    char name[FD_RESULT_NAME_MAX];
    float conf = 0.0f;
    uint32_t status = 3;
    face_detection_config_t cfg;
    uint32_t timeout = 8000;
    if (face_detection_get_config(&cfg) == AICAM_OK && cfg.result_timeout_ms > 0) {
        timeout = cfg.result_timeout_ms + 1500;
    }
    aicam_result_t rc = face_detection_verify_image(img, img_size,
                                                    name, sizeof(name),
                                                    &conf, &status, timeout);
    if (rc != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Verify failed");
    }

    static const char *status_names[] = {"ok", "unknown", "unknown", "error"};
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "status",
                             status <= 3 ? status_names[status] : "error");
    cJSON_AddStringToObject(data, "name", status == 0 ? name : "");
    cJSON_AddNumberToObject(data, "confidence", (double)conf);
    char *json_string = cJSON_Print(data);
    cJSON_Delete(data);
    return api_response_success(ctx, json_string, "Face verification completed");
}

/* ==================== API Route Registration ==================== */

static const api_route_t face_routes[] = {
    {
        .path = API_PATH_PREFIX "/face/config",
        .method = "GET",
        .handler = face_config_get_handler,
        .require_auth = AICAM_FALSE,
        .user_data = NULL
    },
    {
        .path = API_PATH_PREFIX "/face/config",
        .method = "POST",
        .handler = face_config_set_handler,
        .require_auth = AICAM_FALSE,
        .user_data = NULL
    },
    {
        .path = API_PATH_PREFIX "/face/status",
        .method = "GET",
        .handler = face_status_handler,
        .require_auth = AICAM_FALSE,
        .user_data = NULL
    },
    {
        .path = API_PATH_PREFIX "/face/results",
        .method = "GET",
        .handler = face_results_handler,
        .require_auth = AICAM_FALSE,
        .user_data = NULL
    },
    {
        .path = API_PATH_PREFIX "/face/trigger",
        .method = "POST",
        .handler = face_trigger_handler,
        .require_auth = AICAM_FALSE,
        .user_data = NULL
    },
    {
        .path = API_PATH_PREFIX "/face/verify",
        .method = "POST",
        .handler = face_verify_handler,
        .require_auth = AICAM_FALSE,
        .user_data = NULL
    },
    {
        .path = API_PATH_PREFIX "/face/reset",
        .method = "POST",
        .handler = face_reset_handler,
        .require_auth = AICAM_FALSE,
        .user_data = NULL
    }
};

aicam_result_t web_api_register_face_detection_module(void)
{
    LOG_SVC_INFO("Registering Face Detection API module...");

    size_t route_count = sizeof(face_routes) / sizeof(face_routes[0]);
    for (size_t i = 0; i < route_count; i++) {
        aicam_result_t result = http_server_register_route(&face_routes[i]);
        if (result != AICAM_OK) {
            LOG_SVC_ERROR("Failed to register face route: %s", face_routes[i].path);
            return result;
        }
    }

    LOG_SVC_INFO("Face Detection API module registered (%zu routes)", route_count);
    return AICAM_OK;
}
