/**
 * @file face_detection_config.h
 * @brief Face detection demo configuration structure (shared with the
 *        persistent configuration manager)
 * @details Kept dependency-free (only aicam_types.h/stdint) so both the
 *          video-layer module and the JSON/NVS config manager can include it
 *          without pulling NN headers.
 */

#ifndef FACE_DETECTION_CONFIG_H
#define FACE_DETECTION_CONFIG_H

#include "aicam_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Configuration ==================== */

/* Report channel selector (mutually exclusive by design: the backend link
 * is either the webhook URL or the MQTT broker). */
#define FD_REPORT_CHANNEL_WEBHOOK 0u
#define FD_REPORT_CHANNEL_MQTT    1u

/* Upper bound for the report fixed-size letterbox mode (see doc section 4) */
#define FD_REPORT_FIXED_SIZE_MAX 512u

typedef struct {
    aicam_bool_t enabled;                 // Master switch
    /* Detection */
    uint32_t detection_conf_threshold;    // Min detection confidence x100 (1..99)
    uint32_t min_face_area;               // Min face box area (permille^2)
    /* Tracking */
    uint32_t track_iou_threshold;         // IoU match threshold x100 (1..99)
    uint32_t track_max_missed;            // Frames a track survives unmatched
    uint32_t track_confirm_frames;        // Consecutive hits before confirmed
    uint32_t face_hold_ms;                // Anti-blink hold when batch empty (100..2000)
    aicam_bool_t extrapolation_enabled;   // Motion-compensate boxes/crops to
                                          // "now" (disable for fast models)
    /* Face blur (mosaic) */
    aicam_bool_t blur_enabled;            // Mosaic switch
    uint32_t mosaic_block;                // Mosaic block size in pixels (4..64)
    uint32_t blur_expand_ratio;           // Mosaic region expansion in percent
    /* Report crop */
    uint32_t crop_expand_ratio;           // Crop expansion in percent (0..100)
    uint32_t crop_min_size;               // Min crop edge in pixels
    uint32_t report_fixed_size;           // 0 = native crop size, else 32..512 letterbox
    uint32_t jpeg_quality;                // Report JPEG quality (10..95)
    /* Reporting */
    aicam_bool_t report_enabled;
    uint32_t report_channel;              // FD_REPORT_CHANNEL_*
    uint32_t report_confidence_threshold; // x100 (1..99), >= display threshold
    aicam_bool_t report_periodic_enabled; // Re-report while result expired
    uint32_t report_cooldown_ms;          // Min interval between reports per track
    uint32_t result_timeout_ms;           // Wait window for a recognition result
    uint32_t result_expire_ms;            // Result retention on a live track
    /* Overlay drawing */
    aicam_bool_t show_box;
    aicam_bool_t show_id;
    aicam_bool_t show_confidence;
    aicam_bool_t show_landmarks;          // Only when the model provides them
    aicam_bool_t show_recognition;
} face_detection_config_t;

#ifdef __cplusplus
}
#endif

#endif /* FACE_DETECTION_CONFIG_H */
