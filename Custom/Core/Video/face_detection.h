/**
 * @file face_detection.h
 * @brief Face detection demo: tracking, mosaic, crop reporting and
 *        recognition-result overlay
 * @details Consumes face detections from the ACTIVE model (single-model
 *          demo — PP_TYPE_OD or PP_TYPE_MPE flavors are both accepted,
 *          landmarks only when the model emits them), runs an IoU tracker
 *          assigning persistent track ids, applies a configurable mosaic on
 *          the camera PIPE1 RGB565 frames, crops expanded face regions for
 *          reporting (native size or fixed-size letterbox), publishes them
 *          over webhook (sync response) or MQTT (async result on the
 *          control/receive topic) and burns boxes/ids/confidences/names
 *          into the encoded stream. See Docs/Face_Detection_Demo.md.
 *
 *          Thread model:
 *          - face_detection_on_result(): AI node thread
 *          - face_detection_draw_overlay(): camera node thread
 *          - face_detection_apply_result_json(): MQTT thread / webhook worker
 *          - config/stats/results accessors: web server thread
 *          - internal report worker: own thread
 *          All shared state is protected by one mutex.
 */

#ifndef FACE_DETECTION_H
#define FACE_DETECTION_H

#include "aicam_types.h"
#include "nn.h"
#include "face_detection_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Types ==================== */

/* Max simultaneously tracked faces */
#define FD_MAX_TRACKS 16
/* Max landmarks carried per track (blazeface 6, yunet 5; headroom) */
#define FD_MAX_LANDMARKS 10
/* Recognition result display buffer */
#define FD_RESULT_NAME_MAX 32

/* Recognition result state of a track */
typedef enum {
    FD_RESULT_NONE = 0,     /* Not reported yet or expired -> "recognizing" */
    FD_RESULT_PENDING,      /* Report sent, waiting for the backend */
    FD_RESULT_OK,           /* Recognized (name + confidence) */
    FD_RESULT_UNKNOWN,      /* Backend replied no match */
    FD_RESULT_TIMEOUT,      /* Backend did not answer in time */
    FD_RESULT_ERROR         /* Backend/transport error */
} fd_result_status_t;

/* Report reasons (protocol "reason" field) */
typedef enum {
    FD_REASON_NEW_TRACK = 0,
    FD_REASON_PERIODIC = 1,
    FD_REASON_MANUAL = 2
} fd_report_reason_t;

/* Runtime statistics (web-visible) */
typedef struct {
    aicam_bool_t enabled;
    uint32_t confirmed_tracks;      /* Faces currently confirmed in frame */
    uint32_t total_tracks;          /* Cumulative track ids assigned */
    uint32_t last_detections;       /* Faces in the latest inference */
    uint32_t reports_queued;        /* Reports handed to the worker */
    uint32_t reports_sent;          /* Reports delivered to a channel */
    uint32_t reports_dropped;       /* Dropped (queue full / too small) */
    uint32_t reports_failed;        /* Channel errors */
    uint32_t results_ok;
    uint32_t results_unknown;
    uint32_t results_timeout;
    uint32_t last_update_ms;        /* Kernel ticks of the last result */
} face_detection_stats_t;

/* Snapshot of one track for the /results endpoint */
typedef struct {
    uint32_t track_id;
    float x, y, w, h;               /* Normalized last box */
    float detection_confidence;
    uint32_t nb_landmarks;
    float landmarks[FD_MAX_LANDMARKS][2];
    fd_result_status_t result_status;
    char result_name[FD_RESULT_NAME_MAX];
    float result_confidence;
} face_detection_track_info_t;

typedef struct {
    uint32_t count;
    face_detection_track_info_t tracks[FD_MAX_TRACKS];
} face_detection_results_t;

/* ==================== API Functions ==================== */

/**
 * @brief Initialize the module (idempotent)
 * @details Loads the persisted configuration when the config manager is up,
 *          otherwise factory defaults (retried lazily).
 */
aicam_result_t face_detection_init(void);

/**
 * @brief Deinitialize and release resources
 */
aicam_result_t face_detection_deinit(void);

aicam_bool_t face_detection_is_initialized(void);

/**
 * @brief Get a copy of the current configuration
 */
aicam_result_t face_detection_get_config(face_detection_config_t *config);

/**
 * @brief Set the configuration (web layer validates fields beforehand)
 */
aicam_result_t face_detection_set_config(const face_detection_config_t *config);

/**
 * @brief Fill a config struct with factory defaults
 */
void face_detection_get_default_config(face_detection_config_t *config);

/**
 * @brief Get a snapshot of the statistics
 */
aicam_result_t face_detection_get_stats(face_detection_stats_t *stats);

/**
 * @brief Snapshot of visible confirmed tracks (for the web overlay)
 */
aicam_result_t face_detection_get_results(face_detection_results_t *results);

/**
 * @brief Reset statistics and track/result caches
 */
aicam_result_t face_detection_reset(void);

/**
 * @brief Feed a face detection result (called on the AI node thread)
 * @note  Signature matches video_ai_result_callback_t.
 */
void face_detection_on_result(const nn_result_t *result, uint32_t frame_id,
                              uint32_t inference_time_ms, void *user_data);

/**
 * @brief Draw the demo overlay on a camera PIPE1 RGB565 frame
 * @details Applies (in order): face mosaic, detection boxes with
 *          id/confidence/recognition labels (edge-aware placement),
 *          landmarks. Also crops+encodes pending reports. Safe to call
 *          every camera frame; no-ops when disabled.
 */
void face_detection_draw_overlay(uint8_t *fb, uint32_t width, uint32_t height);

/**
 * @brief Apply a recognition result JSON (protocol section 5.3)
 * @details Matches by echoed request_id; idempotent; stale/late results for
 *          recycled tracks are dropped silently.
 * @return AICAM_TRUE when the payload was a face result and was consumed
 */
aicam_bool_t face_detection_apply_result_json(const char *json, uint32_t len);

/**
 * @brief MQTT control-channel sniff hook
 * @details Called by the MQTT service for every control-topic message;
 *          consumes "face_recognition_result" payloads.
 * @return AICAM_TRUE when the message was consumed (caller must skip
 *         normal control-command parsing)
 */
aicam_bool_t face_detection_try_handle_mqtt(const char *json, uint32_t len);

/**
 * @brief Force a report for one track (0 = all confirmed tracks)
 * @return Number of reports armed
 */
uint32_t face_detection_trigger_report(uint32_t track_id);

/**
 * @brief Push a user-uploaded face JPEG through the recognition channel
 *        (webhook sync response / MQTT reply) and return the verdict.
 * @param status_out 0=ok (name/conf filled), 1=unknown/no_face, 3=error
 * @return AICAM_OK when the request completed (check status_out for verdict)
 */
aicam_result_t face_detection_verify_image(const uint8_t *jpeg, uint32_t size,
                                           char *name_out, uint32_t name_size,
                                           float *conf_out, uint32_t *status_out,
                                           uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* FACE_DETECTION_H */
