/**
 * @file api_people_counting_module.c
 * @brief People Counting demo Web API module
 * @details Routes:
 *          GET  /api/v1/counting/config - current demo configuration
 *          POST /api/v1/counting/config - partial configuration update (merge)
 *          GET  /api/v1/counting/stats  - live counting statistics
 *          POST /api/v1/counting/reset  - reset in/out counters and tracks
 */

#include "web_server.h"
#include "web_api.h"
#include "debug.h"
#include "people_counting.h"
#include "ai_service.h"
#include "json_config_mgr.h"
#include "cmsis_os2.h"
#include <string.h>
#include <stdio.h>

/* Face model lazy-reload cadence for the stats poll (ms) */
#define PC_FACE_RELOAD_INTERVAL_MS 10000U

/* ==================== Internal Helpers ==================== */

/**
 * @brief Apply a validated config to the runtime module and NVS
 */
static aicam_result_t counting_apply_config(const people_counting_config_t *config)
{
    /* Runtime module first (takes effect on the next frame) */
    aicam_result_t result = people_counting_set_config(config);
    if (result != AICAM_OK) {
        return result;
    }

    /* Persist */
    result = json_config_set_people_counting_config(config);
    if (result != AICAM_OK) {
        return result;
    }

    /* Face model thresholds go to the nn instance (stored for later if the
     * model is not loaded yet) */
    ai_service_set_face_thresholds(config->face_conf_threshold,
                                   config->face_nms_threshold);

    /* Face model bookkeeping: load on demand when the demo is switched on */
    if (config->enabled && ai_service_face_model_is_loaded() != AICAM_TRUE) {
        ai_service_load_face_model();
    }

    return AICAM_OK;
}

/**
 * @brief Read one optional numeric config field with range validation
 * @return 1 applied, 0 not present, -1 invalid (out of range / wrong type)
 */
static int counting_apply_uint32(cJSON *request, const char *key,
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
 * @brief Read one optional boolean config field
 * @return 1 applied, 0 not present, -1 wrong type
 */
static int counting_apply_bool(cJSON *request, const char *key, aicam_bool_t *field)
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
 * @brief Serialize the config to the response JSON shape (snake_case)
 */
static cJSON *counting_config_to_json(const people_counting_config_t *cfg)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "enabled", cfg->enabled);
    cJSON_AddBoolToObject(data, "show_line", cfg->show_line);
    cJSON_AddBoolToObject(data, "show_osd", cfg->show_osd);
    cJSON_AddBoolToObject(data, "face_mosaic", cfg->face_mosaic);
    cJSON_AddNumberToObject(data, "line_x1", cfg->line_x1);
    cJSON_AddNumberToObject(data, "line_y1", cfg->line_y1);
    cJSON_AddNumberToObject(data, "line_x2", cfg->line_x2);
    cJSON_AddNumberToObject(data, "line_y2", cfg->line_y2);
    cJSON_AddNumberToObject(data, "line_thickness", cfg->line_thickness);
    cJSON_AddBoolToObject(data, "invert_direction", cfg->invert_direction);
    cJSON_AddNumberToObject(data, "track_iou_threshold", cfg->track_iou_threshold);
    cJSON_AddNumberToObject(data, "track_max_missed", cfg->track_max_missed);
    cJSON_AddNumberToObject(data, "cross_debounce", cfg->cross_debounce);
    cJSON_AddNumberToObject(data, "min_box_area", cfg->min_box_area);
    cJSON_AddNumberToObject(data, "mosaic_block", cfg->mosaic_block);
    cJSON_AddNumberToObject(data, "mosaic_expand", cfg->mosaic_expand);
    cJSON_AddNumberToObject(data, "face_conf_threshold", cfg->face_conf_threshold);
    cJSON_AddNumberToObject(data, "face_nms_threshold", cfg->face_nms_threshold);
    /* Read-only helper for the frontend (0..1000 permille) */
    cJSON_AddNumberToObject(data, "coord_max", PC_COORD_MAX);
    return data;
}

/* ==================== Handlers ==================== */

/**
 * @brief GET /api/v1/counting/config
 */
static aicam_result_t counting_config_get_handler(http_handler_context_t *ctx)
{
    if (!ctx) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!web_api_verify_method(ctx, "GET")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    people_counting_config_t cfg;
    if (json_config_get_people_counting_config(&cfg) != AICAM_OK) {
        people_counting_get_config(&cfg);
    }

    cJSON *data = counting_config_to_json(&cfg);
    char *json_string = cJSON_Print(data);
    cJSON_Delete(data);

    return api_response_success(ctx, json_string, "People counting configuration retrieved");
}

/**
 * @brief POST /api/v1/counting/config (partial merge, validated)
 */
static aicam_result_t counting_config_set_handler(http_handler_context_t *ctx)
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
    people_counting_config_t cfg;
    if (json_config_get_people_counting_config(&cfg) != AICAM_OK) {
        people_counting_get_config(&cfg);
    }

    cJSON *errors = cJSON_CreateArray();
    int applied = 0;
    int invalid = 0;

    struct {
        const char *key;
        uint32_t min;
        uint32_t max;
    } uint_fields[] = {
        {"line_x1", 0, PC_COORD_MAX},
        {"line_y1", 0, PC_COORD_MAX},
        {"line_x2", 0, PC_COORD_MAX},
        {"line_y2", 0, PC_COORD_MAX},
        {"line_thickness", 1, 16},
        {"track_iou_threshold", 1, 99},
        {"track_max_missed", 1, 50},
        {"cross_debounce", 1, 10},
        {"min_box_area", 0, 1000000u},
        {"mosaic_block", 4, 64},
        {"mosaic_expand", 0, 100},
        {"face_conf_threshold", 1, 99},
        {"face_nms_threshold", 1, 99},
    };
    uint32_t *uint_targets[] = {
        &cfg.line_x1, &cfg.line_y1, &cfg.line_x2, &cfg.line_y2,
        &cfg.line_thickness, &cfg.track_iou_threshold, &cfg.track_max_missed,
        &cfg.cross_debounce, &cfg.min_box_area, &cfg.mosaic_block, &cfg.mosaic_expand,
        &cfg.face_conf_threshold, &cfg.face_nms_threshold,
    };

    for (size_t i = 0; i < sizeof(uint_fields) / sizeof(uint_fields[0]); i++) {
        int r = counting_apply_uint32(request, uint_fields[i].key, uint_targets[i],
                                      uint_fields[i].min, uint_fields[i].max);
        if (r > 0) {
            applied++;
        } else if (r < 0) {
            invalid++;
            char msg[96];
            snprintf(msg, sizeof(msg), "%s must be a number in [%lu, %lu]",
                     uint_fields[i].key,
                     (unsigned long)uint_fields[i].min,
                     (unsigned long)uint_fields[i].max);
            cJSON_AddItemToArray(errors, cJSON_CreateString(msg));
        }
    }

    const char *bool_keys[] = {
        "enabled", "show_line", "show_osd", "face_mosaic", "invert_direction",
    };
    aicam_bool_t *bool_targets[] = {
        &cfg.enabled, &cfg.show_line, &cfg.show_osd, &cfg.face_mosaic, &cfg.invert_direction,
    };
    for (size_t i = 0; i < sizeof(bool_keys) / sizeof(bool_keys[0]); i++) {
        int r = counting_apply_bool(request, bool_keys[i], bool_targets[i]);
        if (r > 0) {
            applied++;
        } else if (r < 0) {
            invalid++;
            char msg[80];
            snprintf(msg, sizeof(msg), "%s must be a boolean", bool_keys[i]);
            cJSON_AddItemToArray(errors, cJSON_CreateString(msg));
        }
    }

    cJSON_Delete(request);

    if (invalid > 0) {
        cJSON_Delete(errors);
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "Invalid configuration values");
    }
    if (applied == 0) {
        cJSON_Delete(errors);
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "No valid fields provided");
    }

    aicam_result_t result = counting_apply_config(&cfg);

    cJSON *data = counting_config_to_json(&cfg);
    cJSON_AddItemToObject(data, "updated_fields", cJSON_CreateNumber(applied));
    cJSON_AddItemToObject(data, "errors", errors);
    char *json_string = cJSON_Print(data);
    cJSON_Delete(data);

    if (result != AICAM_OK) {
        cJSON_free(json_string);
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to apply configuration");
    }
    return api_response_success(ctx, json_string, "People counting configuration updated");
}

/**
 * @brief GET /api/v1/counting/stats
 */
static aicam_result_t counting_stats_handler(http_handler_context_t *ctx)
{
    if (!ctx) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!web_api_verify_method(ctx, "GET")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    people_counting_stats_t stats;
    people_counting_get_stats(&stats);

    /* Face model live state; when the demo is on and the model is missing
     * (e.g. flashed after boot, or the boot-time load was deferred), retry
     * the load at a bounded rate so the web badge self-heals. */
    aicam_bool_t face_loaded = ai_service_face_model_is_loaded();
    if (stats.enabled && face_loaded != AICAM_TRUE) {
        static uint32_t last_attempt_tick = 0;
        uint32_t now = osKernelGetTickCount();
        if (last_attempt_tick == 0 || (now - last_attempt_tick) >= PC_FACE_RELOAD_INTERVAL_MS) {
            last_attempt_tick = now;
            ai_service_load_face_model();
            face_loaded = ai_service_face_model_is_loaded();
        }
    }
    stats.face_model_loaded = face_loaded;

    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "enabled", stats.enabled);
    cJSON_AddNumberToObject(data, "current_count", stats.current_count);
    cJSON_AddNumberToObject(data, "in_count", stats.in_count);
    cJSON_AddNumberToObject(data, "out_count", stats.out_count);
    cJSON_AddNumberToObject(data, "net_count", stats.net_count);
    cJSON_AddNumberToObject(data, "active_tracks", stats.active_tracks);
    cJSON_AddNumberToObject(data, "last_person_detections", stats.last_person_detections);
    cJSON_AddNumberToObject(data, "last_face_detections", stats.last_face_detections);
    cJSON_AddBoolToObject(data, "face_model_loaded", face_loaded);
    cJSON_AddNumberToObject(data, "last_update_ms", stats.last_update_ms);

    /* Secondary model name when loaded (useful to confirm the right model
     * was flashed to the non-active partition) */
    nn_model_info_t face_info;
    if (face_loaded == AICAM_TRUE &&
        ai_service_get_face_model_info(&face_info) == AICAM_OK) {
        cJSON_AddStringToObject(data, "face_model_name", face_info.name);
    }

    char *json_string = cJSON_Print(data);
    cJSON_Delete(data);

    return api_response_success(ctx, json_string, "People counting statistics");
}

/**
 * @brief POST /api/v1/counting/reset
 */
static aicam_result_t counting_reset_handler(http_handler_context_t *ctx)
{
    if (!ctx) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    aicam_result_t result = people_counting_reset_stats();
    if (result != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to reset statistics");
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "success", 1);
    char *json_string = cJSON_Print(data);
    cJSON_Delete(data);

    return api_response_success(ctx, json_string, "People counting statistics reset");
}

/* ==================== API Route Registration ==================== */

static const api_route_t counting_routes[] = {
    {
        .path = API_PATH_PREFIX "/counting/config",
        .method = "GET",
        .handler = counting_config_get_handler,
        .require_auth = AICAM_FALSE,
        .user_data = NULL
    },
    {
        .path = API_PATH_PREFIX "/counting/config",
        .method = "POST",
        .handler = counting_config_set_handler,
        .require_auth = AICAM_FALSE,
        .user_data = NULL
    },
    {
        .path = API_PATH_PREFIX "/counting/stats",
        .method = "GET",
        .handler = counting_stats_handler,
        .require_auth = AICAM_FALSE,
        .user_data = NULL
    },
    {
        .path = API_PATH_PREFIX "/counting/reset",
        .method = "POST",
        .handler = counting_reset_handler,
        .require_auth = AICAM_FALSE,
        .user_data = NULL
    }
};

aicam_result_t web_api_register_people_counting_module(void)
{
    LOG_SVC_INFO("Registering People Counting API module...");

    size_t route_count = sizeof(counting_routes) / sizeof(counting_routes[0]);
    for (size_t i = 0; i < route_count; i++) {
        aicam_result_t result = http_server_register_route(&counting_routes[i]);
        if (result != AICAM_OK) {
            LOG_SVC_ERROR("Failed to register counting route: %s", counting_routes[i].path);
            return result;
        }
    }

    LOG_SVC_INFO("People Counting API module registered (%zu routes)", route_count);
    return AICAM_OK;
}
