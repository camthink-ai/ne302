/**
 * @file people_counting.h
 * @brief People counting demo: in-frame counting, line-crossing statistics and face mosaic
 * @details Consumes person detections (primary OD model) and face detections
 *          (secondary blazeface model) produced by the AI node, runs an
 *          IoU-based tracker with line-crossing debounced counting, and draws
 *          the configurable counting line / direction arrows / count OSD plus
 *          CPU face mosaic onto the camera PIPE1 RGB565 frames.
 *
 *          Thread model:
 *          - people_counting_on_person_result()/..._on_face_result(): AI node thread
 *          - people_counting_draw_overlay(): camera node thread
 *          - config/stats accessors: web server thread
 *          All shared state is protected by one mutex.
 */

#ifndef PEOPLE_COUNTING_H
#define PEOPLE_COUNTING_H

#include "aicam_types.h"
#include "nn.h"
#include "people_counting_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Configuration (see people_counting_config.h) ==================== */

/* Runtime statistics (web-visible) */
typedef struct {
    aicam_bool_t enabled;            // Copy of config enabled flag
    uint32_t current_count;          // People currently tracked in frame
    uint32_t in_count;               // Cumulative "in" crossings
    uint32_t out_count;              // Cumulative "out" crossings
    int32_t net_count;               // in - out
    uint32_t active_tracks;          // Active tracker entries
    uint32_t last_person_detections; // Persons in the latest inference
    uint32_t last_face_detections;   // Faces in the latest secondary inference
    aicam_bool_t face_model_loaded;  // Secondary face model load state
    uint32_t last_update_ms;         // Kernel ticks of the last person result
} people_counting_stats_t;

/* Max simultaneously tracked faces */
#define PC_MAX_FACES 8

/* ==================== API Functions ==================== */

/**
 * @brief Initialize the module (idempotent)
 * @return aicam_result_t
 */
aicam_result_t people_counting_init(void);

/**
 * @brief Deinitialize and release resources
 * @return aicam_result_t
 */
aicam_result_t people_counting_deinit(void);

/**
 * @brief Check initialization state
 * @return aicam_bool_t
 */
aicam_bool_t people_counting_is_initialized(void);

/**
 * @brief Get a copy of the current configuration
 */
aicam_result_t people_counting_get_config(people_counting_config_t *config);

/**
 * @brief Set the configuration (partial updates are done by the web layer)
 * @param config Full new configuration
 * @return aicam_result_t
 */
aicam_result_t people_counting_set_config(const people_counting_config_t *config);

/**
 * @brief Fill a config struct with factory defaults
 */
void people_counting_get_default_config(people_counting_config_t *config);

/**
 * @brief Get a snapshot of the statistics
 */
aicam_result_t people_counting_get_stats(people_counting_stats_t *stats);

/**
 * @brief Reset counters (in/out/net) and clear tracks
 * @return aicam_result_t
 */
aicam_result_t people_counting_reset_stats(void);

/**
 * @brief Report the face (secondary) model load state
 */
void people_counting_set_face_model_loaded(aicam_bool_t loaded);



/**
 * @brief Feed a person detection result (called on the AI node thread)
 * @note  Signature matches video_ai_result_callback_t so it can be registered
 *        directly with the AI node / result dispatcher.
 * @param result Primary model OD result
 * @param frame_id Camera frame id of the inference
 * @param inference_time_ms Inference duration (unused)
 * @param user_data Unused
 */
void people_counting_on_person_result(const nn_result_t *result, uint32_t frame_id,
                                      uint32_t inference_time_ms, void *user_data);

/**
 * @brief Feed a face detection result (called on the AI node thread)
 * @note  Signature matches video_ai_result_callback_t so it can be registered
 *        directly as the secondary model result callback.
 * @param result Secondary model (blazeface, PP_TYPE_MPE) result
 * @param frame_id Camera frame id of the inference
 * @param inference_time_ms Inference duration (unused)
 * @param user_data Unused
 */
void people_counting_on_face_result(const nn_result_t *result, uint32_t frame_id,
                                    uint32_t inference_time_ms, void *user_data);

/**
 * @brief Draw the demo overlay on a camera PIPE1 RGB565 frame
 * @details Applies (in order): face mosaic, counting line + direction arrows,
 *          count OSD. Safe to call every camera frame; no-ops when disabled.
 * @param fb Frame buffer (RGB565)
 * @param width Frame width in pixels
 * @param height Frame height in pixels
 */
void people_counting_draw_overlay(uint8_t *fb, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* PEOPLE_COUNTING_H */
