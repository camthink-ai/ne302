/**
 * @file people_counting_config.h
 * @brief People counting demo configuration structure (shared with the
 *        persistent configuration manager)
 * @details Kept dependency-free (only aicam_types.h/stdint) so both the
 *          video-layer module and the JSON/NVS config manager can include it
 *          without pulling NN headers.
 */

#ifndef PEOPLE_COUNTING_CONFIG_H
#define PEOPLE_COUNTING_CONFIG_H

#include "aicam_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Configuration ==================== */

/* Line coordinates are stored in permille units (0..1000) of the frame
 * width/height so they survive NVS as plain uint32 without float parsing. */
#define PC_COORD_MAX 1000

/* Direction semantics: the counting line P1->P2 defines a 2D cross product;
 * "in" is a crossing from the negative side to the positive side when
 * invert_direction == 0 (swapped when == 1). */
typedef struct {
    aicam_bool_t enabled;            // Master switch for counting processing
    aicam_bool_t show_line;          // Draw the counting line on frames
    aicam_bool_t show_osd;           // Draw the in/out/now count OSD on frames
    aicam_bool_t face_mosaic;        // Apply mosaic blur on detected faces
    uint32_t line_x1;                // Line start X (0..1000 permille)
    uint32_t line_y1;                // Line start Y (0..1000 permille)
    uint32_t line_x2;                // Line end X (0..1000 permille)
    uint32_t line_y2;                // Line end Y (0..1000 permille)
    uint32_t line_thickness;         // Line thickness in pixels (1..16)
    aicam_bool_t invert_direction;   // Swap in/out counting direction
    uint32_t track_iou_threshold;    // IoU threshold x100 for track matching (1..99)
    uint32_t track_max_missed;       // Frames a track survives without a match (1..50)
    uint32_t cross_debounce;         // Consecutive frames on the new side before counting (1..10)
    uint32_t min_box_area;           // Min person box area (permille^2) to be tracked
    uint32_t mosaic_block;           // Mosaic block size in pixels (4..64)
    uint32_t mosaic_expand;          // Face box expansion in percent (0..100)
    uint32_t face_conf_threshold;    // Face detection confidence threshold x100 (1..99)
    uint32_t face_nms_threshold;     // Face detection NMS/IoU threshold x100 (1..99)
} people_counting_config_t;

#ifdef __cplusplus
}
#endif

#endif /* PEOPLE_COUNTING_CONFIG_H */
