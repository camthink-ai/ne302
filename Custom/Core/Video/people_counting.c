/**
 * @file people_counting.c
 * @brief People counting demo implementation
 * @details IoU tracker + debounced line-crossing counter + overlay drawing
 *          (DMA2D line/text, CPU RGB565 mosaic). See people_counting.h for
 *          the threading model.
 */

#include "people_counting.h"
#include "buffer_mgr.h"
#include "debug.h"
#include "dev_manager.h"
#include "draw.h"
#include "fonts.h"
#include "mem.h"
#include "pixel_format_map.h"
#include "cmsis_os2.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ==================== Internal Definitions ==================== */

#define PC_MAX_TRACKS 16

/* Debug aid: draw the raw face detection box (pre-expansion) so detection
 * accuracy vs mosaic mapping can be judged on screen. Flip to 1 to enable. */
#define PC_DEBUG_FACE_BOXES 0

/* Stop drawing face boxes/mosaic when the last face result is older than
 * this (ms) — covers the model being disabled or erroring mid-run. */
#define PC_FACE_STALE_MS 1200U

typedef struct {
    aicam_bool_t active;             // Slot in use
    uint32_t id;                     // Track id (for debugging/telemetry)
    float x, y, w, h;                // Last matched box (normalized)
    float cx, cy;                    // Centroid (normalized)
    float vx, vy;                    /* Centroid velocity (normalized/ms, EMA) */
    uint32_t last_tick;              /* Tick of the last match (velocity anchor) */
    int side;                        // Side of the counting line (-1/0/+1)
    int pending_side;                // Debounce candidate side
    uint32_t pending_frames;         // Consecutive frames seen on pending_side
    uint32_t missed;                 // Consecutive frames without a match
    uint32_t last_frame_id;          // Frame id of the last match
} pc_track_t;

/* Face entries are tracked between results so their velocity can compensate
 * the AI-pipeline latency when drawing (box is extrapolated to "now"). */
typedef struct {
    float x, y, w, h;                // Last box (normalized, top-left)
    float cx, cy;                    // Center (normalized)
    float vx, vy;                    // Center velocity (normalized/ms, EMA)
    uint32_t last_tick;              // Tick of the last update
} pc_face_track_t;

typedef struct {
    aicam_bool_t initialized;
    osMutexId_t mutex;
    people_counting_config_t config;
    people_counting_stats_t stats;
    pc_track_t tracks[PC_MAX_TRACKS];
    uint32_t next_track_id;
    uint32_t last_person_frame_id;
    pc_face_track_t faces[PC_MAX_FACES];
    uint32_t face_count;
    uint32_t last_face_tick;              /* Tick of the last face result */
    /* Draw state (camera thread only, no lock needed once set) */
    device_t *draw_dev;
    DRAW_Font_t font16;
    aicam_bool_t font_ready;
} people_counting_ctx_t;

static people_counting_ctx_t g_pc = {0};

/* ==================== Internal Function Declarations ==================== */

static int pc_side_of_line(const people_counting_config_t *cfg, float x, float y);
static float pc_box_iou(const pc_track_t *t, float x, float y, float w, float h);
static void pc_track_update_side(people_counting_ctx_t *ctx, pc_track_t *track, int new_side);
static aicam_bool_t pc_class_is_person(const char *class_name);
static void pc_mosaic_region_rgb565(uint16_t *fb, uint32_t fw, uint32_t fh,
                                    int x0, int y0, int x1, int y1, uint32_t block);
static void pc_draw_line_hw(const people_counting_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                            int x1, int y1, int x2, int y2, uint32_t thickness, uint32_t color);
static void pc_draw_rect_hw(const people_counting_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                            int x, int y, int w, int h, uint32_t thickness, uint32_t color);
static void pc_draw_text_hw(const people_counting_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                            int x, int y, const char *str);
static void pc_blend_rect_hw(const people_counting_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                             int x, int y, int w, int h, uint32_t color, uint8_t alpha);
static uint32_t ctx_font_width(const people_counting_ctx_t *ctx, const char *str);
static aicam_bool_t pc_lazy_draw_init(people_counting_ctx_t *ctx);

/* ==================== Helper Functions ==================== */

/**
 * @brief Side of point (x,y) relative to the counting line P1->P2
 * @return -1, 0 (on the line) or +1 based on the 2D cross product sign
 */
static int pc_side_of_line(const people_counting_config_t *cfg, float x, float y)
{
    float x1 = (float)cfg->line_x1 / PC_COORD_MAX;
    float y1 = (float)cfg->line_y1 / PC_COORD_MAX;
    float x2 = (float)cfg->line_x2 / PC_COORD_MAX;
    float y2 = (float)cfg->line_y2 / PC_COORD_MAX;
    float cross = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
    return (cross > 0.0f) - (cross < 0.0f);
}

static float pc_box_iou(const pc_track_t *t, float x, float y, float w, float h)
{
    float ix0 = t->x > x ? t->x : x;
    float iy0 = t->y > y ? t->y : y;
    float ix1 = (t->x + t->w) < (x + w) ? (t->x + t->w) : (x + w);
    float iy1 = (t->y + t->h) < (y + h) ? (t->y + t->h) : (y + h);
    if (ix1 <= ix0 || iy1 <= iy0) {
        return 0.0f;
    }
    float inter = (ix1 - ix0) * (iy1 - iy0);
    float union_area = t->w * t->h + w * h - inter;
    return union_area > 0.0f ? inter / union_area : 0.0f;
}

/**
 * @brief Update the track's side state machine, counting debounced crossings
 */
static void pc_track_update_side(people_counting_ctx_t *ctx, pc_track_t *track, int new_side)
{
    const people_counting_config_t *cfg = &ctx->config;

    if (new_side == 0 || new_side == track->side) {
        track->pending_side = 0;
        track->pending_frames = 0;
        return;
    }

    if (new_side == track->pending_side) {
        track->pending_frames++;
    } else {
        track->pending_side = new_side;
        track->pending_frames = 1;
    }

    if (track->pending_frames >= cfg->cross_debounce) {
        int old_side = track->side;
        track->side = new_side;
        track->pending_side = 0;
        track->pending_frames = 0;

        /* Only count genuine side flips; tracks born on the line (old == 0)
         * just settle on their first side. */
        if (old_side != 0) {
            aicam_bool_t is_in = (old_side < 0 && new_side > 0) ? AICAM_TRUE : AICAM_FALSE;
            if (cfg->invert_direction) {
                is_in = !is_in;
            }
            if (is_in) {
                ctx->stats.in_count++;
            } else {
                ctx->stats.out_count++;
            }
            ctx->stats.net_count = (int32_t)ctx->stats.in_count - (int32_t)ctx->stats.out_count;
        }
    }
}

static aicam_bool_t pc_class_is_person(const char *class_name)
{
    if (!class_name || class_name[0] == '\0') {
        /* Class-agnostic (person-only) models: accept everything */
        return AICAM_TRUE;
    }
    /* Case-insensitive compare with "person" */
    static const char person[] = "person";
    for (uint32_t i = 0; i < sizeof(person) - 1; i++) {
        char c = class_name[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != person[i]) {
            return AICAM_FALSE;
        }
    }
    return class_name[sizeof(person) - 1] == '\0' ? AICAM_TRUE : AICAM_FALSE;
}

/* ==================== Detection Input (AI node thread) ==================== */

void people_counting_on_person_result(const nn_result_t *result, uint32_t frame_id,
                                      uint32_t inference_time_ms, void *user_data)
{
    (void)inference_time_ms;
    (void)user_data;
    if (!g_pc.initialized || !result || !g_pc.config.enabled) {
        return;
    }
    if (result->type != PP_TYPE_OD || !result->is_valid) {
        return;
    }

    /* Collect person detections (normalized top-left boxes) */
    float det_x[PC_MAX_TRACKS], det_y[PC_MAX_TRACKS], det_w[PC_MAX_TRACKS], det_h[PC_MAX_TRACKS];
    uint32_t det_count = 0;
    uint32_t nb = result->od.nb_detect;
    if (nb > PC_MAX_TRACKS) {
        nb = PC_MAX_TRACKS;
    }
    for (uint32_t i = 0; i < nb; i++) {
        const od_detect_t *d = &result->od.detects[i];
        if (!pc_class_is_person(d->class_name)) {
            continue;
        }
        float w = d->width, h = d->height;
        if (w <= 0.0f || h <= 0.0f) {
            continue;
        }
        float x = d->x, y = d->y;
        if (x < 0.0f) x = 0.0f;
        if (y < 0.0f) y = 0.0f;
        if (x + w > 1.0f) w = 1.0f - x;
        if (y + h > 1.0f) h = 1.0f - y;
        if (w <= 0.0f || h <= 0.0f) {
            continue;
        }
        det_x[det_count] = x;
        det_y[det_count] = y;
        det_w[det_count] = w;
        det_h[det_count] = h;
        det_count++;
    }

    if (osMutexAcquire(g_pc.mutex, 50) != osOK) {
        return;
    }

    /* Guard against a stale/duplicated result delivery */
    if (frame_id == g_pc.last_person_frame_id) {
        osMutexRelease(g_pc.mutex);
        return;
    }
    g_pc.last_person_frame_id = frame_id;

    people_counting_config_t *cfg = &g_pc.config;
    float iou_thr = (float)cfg->track_iou_threshold / 100.0f;

    /* Area filter (permille^2): w[pm] * h[pm] >= min_box_area */
    uint32_t kept = 0;
    for (uint32_t i = 0; i < det_count; i++) {
        uint32_t area_pm2 = (uint32_t)(det_w[i] * PC_COORD_MAX * det_h[i] * PC_COORD_MAX);
        if (area_pm2 >= cfg->min_box_area) {
            det_x[kept] = det_x[i];
            det_y[kept] = det_y[i];
            det_w[kept] = det_w[i];
            det_h[kept] = det_h[i];
            kept++;
        }
    }
    det_count = kept;
    g_pc.stats.last_person_detections = det_count;

    /* Greedy IoU matching (best pair first), one match per track/detection */
    aicam_bool_t det_matched[PC_MAX_TRACKS] = {AICAM_FALSE};
    aicam_bool_t track_matched[PC_MAX_TRACKS] = {AICAM_FALSE};
    for (uint32_t round = 0; round < det_count; round++) {
        float best_iou = iou_thr;
        int best_track = -1;
        int best_det = -1;
        for (int t = 0; t < PC_MAX_TRACKS; t++) {
            if (!g_pc.tracks[t].active || track_matched[t]) {
                continue;
            }
            for (uint32_t d = 0; d < det_count; d++) {
                if (det_matched[d]) {
                    continue;
                }
                float iou = pc_box_iou(&g_pc.tracks[t], det_x[d], det_y[d], det_w[d], det_h[d]);
                if (iou > best_iou) {
                    best_iou = iou;
                    best_track = t;
                    best_det = (int)d;
                }
            }
        }
        if (best_track < 0) {
            break;
        }
        pc_track_t *track = &g_pc.tracks[best_track];
        float ncx = det_x[best_det] + det_w[best_det] * 0.5f;
        float ncy = det_y[best_det] + det_h[best_det] * 0.5f;
        uint32_t now_tick = osKernelGetTickCount();
        uint32_t vdt = now_tick - track->last_tick;
        if (track->last_tick != 0 && vdt > 0 && vdt < 500) {
            /* EMA velocity (normalized/ms) for motion-compensated crops */
            float ivx = (ncx - track->cx) / (float)vdt;
            float ivy = (ncy - track->cy) / (float)vdt;
            track->vx = (track->vx == 0.0f) ? ivx : track->vx * 0.5f + ivx * 0.5f;
            track->vy = (track->vy == 0.0f) ? ivy : track->vy * 0.5f + ivy * 0.5f;
        }
        track->x = det_x[best_det];
        track->y = det_y[best_det];
        track->w = det_w[best_det];
        track->h = det_h[best_det];
        track->cx = ncx;
        track->cy = ncy;
        track->missed = 0;
        track->last_tick = now_tick;
        track->last_frame_id = frame_id;
        det_matched[best_det] = AICAM_TRUE;
        track_matched[best_track] = AICAM_TRUE;

        int side = pc_side_of_line(cfg, track->cx, track->cy);
        pc_track_update_side(&g_pc, track, side);
    }

    /* Create tracks for unmatched detections */
    for (uint32_t d = 0; d < det_count; d++) {
        if (det_matched[d]) {
            continue;
        }
        for (int t = 0; t < PC_MAX_TRACKS; t++) {
            pc_track_t *track = &g_pc.tracks[t];
            if (track->active) {
                continue;
            }
            memset(track, 0, sizeof(pc_track_t));
            track->active = AICAM_TRUE;
            track->id = ++g_pc.next_track_id;
            track->x = det_x[d];
            track->y = det_y[d];
            track->w = det_w[d];
            track->h = det_h[d];
            track->cx = track->x + track->w * 0.5f;
            track->cy = track->y + track->h * 0.5f;
            track->vx = 0.0f;
            track->vy = 0.0f;
            track->last_tick = osKernelGetTickCount();
            track->side = pc_side_of_line(cfg, track->cx, track->cy);
            track->missed = 0;
            track->last_frame_id = frame_id;
            break;
        }
    }

    /* Age unmatched tracks and drop expired ones */
    uint32_t active = 0;
    for (int t = 0; t < PC_MAX_TRACKS; t++) {
        pc_track_t *track = &g_pc.tracks[t];
        if (!track->active) {
            continue;
        }
        if (!track_matched[t]) {
            track->missed++;
            if (track->missed > cfg->track_max_missed) {
                memset(track, 0, sizeof(pc_track_t)); /* deactivate */
                continue;
            }
        }
        active++;
    }

    g_pc.stats.active_tracks = active;
    g_pc.stats.current_count = active;
    g_pc.stats.last_update_ms = osKernelGetTickCount();

    osMutexRelease(g_pc.mutex);
}

void people_counting_on_face_result(const nn_result_t *result, uint32_t frame_id,
                                    uint32_t inference_time_ms, void *user_data)
{
    (void)frame_id;
    (void)inference_time_ms;
    (void)user_data;
    if (!g_pc.initialized) {
        return;
    }

    uint32_t now = osKernelGetTickCount();

    if (osMutexAcquire(g_pc.mutex, 50) != osOK) {
        return;
    }

    g_pc.last_face_tick = now;

    /* No valid faces this frame (person left, or model degraded to noise):
     * clear the snapshot so the overlay does not keep drawing the last
     * known face forever. */
    if (!result || !result->is_valid || result->type != PP_TYPE_MPE) {
        g_pc.face_count = 0;
        g_pc.stats.last_face_detections = 0;
        osMutexRelease(g_pc.mutex);
        return;
    }

    /* Collect the incoming faces, then match them to the previous tracks so
     * each carries a velocity for latency-compensated drawing. */
    float nx[PC_MAX_FACES], ny[PC_MAX_FACES], nw[PC_MAX_FACES], nh[PC_MAX_FACES];
    uint32_t nb = result->mpe.nb_detect;
    if (nb > PC_MAX_FACES) {
        nb = PC_MAX_FACES;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < nb; i++) {
        const mpe_detect_t *d = &result->mpe.detects[i];
        if (d->width <= 0.0f || d->height <= 0.0f) {
            continue;
        }
        nx[count] = d->x;
        ny[count] = d->y;
        nw[count] = d->width;
        nh[count] = d->height;
        count++;
    }

    pc_face_track_t updated[PC_MAX_FACES];
    uint32_t updated_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        float cx = nx[i] + nw[i] * 0.5f;
        float cy = ny[i] + nh[i] * 0.5f;

        /* Nearest previous face within a generous radius */
        int best = -1;
        float best_d2 = 0.2f * 0.2f;
        for (uint32_t t = 0; t < g_pc.face_count; t++) {
            float ddx = cx - g_pc.faces[t].cx;
            float ddy = cy - g_pc.faces[t].cy;
            float d2 = ddx * ddx + ddy * ddy;
            if (d2 < best_d2) {
                best_d2 = d2;
                best = (int)t;
            }
        }

        pc_face_track_t *out = &updated[updated_count++];
        memset(out, 0, sizeof(pc_face_track_t));
        out->x = nx[i];
        out->y = ny[i];
        out->w = nw[i];
        out->h = nh[i];
        out->cx = cx;
        out->cy = cy;
        out->last_tick = now;
        if (best >= 0) {
            pc_face_track_t *old = &g_pc.faces[best];
            uint32_t dt = now - old->last_tick;
            if (old->last_tick != 0 && dt > 0 && dt < 500) {
                float ivx = (cx - old->cx) / (float)dt;
                float ivy = (cy - old->cy) / (float)dt;
                out->vx = (old->vx == 0.0f) ? ivx : old->vx * 0.5f + ivx * 0.5f;
                out->vy = (old->vy == 0.0f) ? ivy : old->vy * 0.5f + ivy * 0.5f;
            }
        }
    }

    memcpy(g_pc.faces, updated, sizeof(pc_face_track_t) * updated_count);
    g_pc.face_count = updated_count;
    g_pc.stats.last_face_detections = updated_count;

    osMutexRelease(g_pc.mutex);
}

/* ==================== Overlay Drawing (camera thread) ==================== */

/**
 * @brief In-place block-average mosaic on an RGB565 frame region
 */
static void pc_mosaic_region_rgb565(uint16_t *fb, uint32_t fw, uint32_t fh,
                                    int x0, int y0, int x1, int y1, uint32_t block)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)fw) x1 = (int)fw;
    if (y1 > (int)fh) y1 = (int)fh;
    if (x1 - x0 < 2 || y1 - y0 < 2 || block < 2) {
        return;
    }

    for (int by = y0; by < y1; by += (int)block) {
        int ye = (by + (int)block < y1) ? by + (int)block : y1;
        for (int bx = x0; bx < x1; bx += (int)block) {
            int xe = (bx + (int)block < x1) ? bx + (int)block : x1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = by; y < ye; y++) {
                const uint16_t *row = fb + (size_t)y * fw + bx;
                for (int x = bx; x < xe; x++) {
                    uint16_t p = *row++;
                    r += (uint32_t)((p >> 11) & 0x1F);
                    g += (uint32_t)((p >> 5) & 0x3F);
                    b += (uint32_t)(p & 0x1F);
                    n++;
                }
            }
            if (n == 0) {
                continue;
            }
            uint16_t avg = (uint16_t)((((r / n) & 0x1F) << 11) |
                                     ((((g / n) & 0x3F) << 5)) |
                                     ((b / n) & 0x1F));
            for (int y = by; y < ye; y++) {
                uint16_t *row = fb + (size_t)y * fw + bx;
                for (int x = bx; x < xe; x++) {
                    *row++ = avg;
                }
            }
        }
    }
}

static void pc_draw_line_hw(const people_counting_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                            int x1, int y1, int x2, int y2, uint32_t thickness, uint32_t color)
{
    /* draw.c rejects endpoints outside the framebuffer */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 < 0) x2 = 0;
    if (y2 < 0) y2 = 0;
    if (x1 >= (int)fw) x1 = (int)fw - 1;
    if (y1 >= (int)fh) y1 = (int)fh - 1;
    if (x2 >= (int)fw) x2 = (int)fw - 1;
    if (y2 >= (int)fh) y2 = (int)fh - 1;

    draw_line_param_t param = {0};
    param.p_dst = fb;
    param.dst_width = (int)fw;
    param.dst_height = (int)fh;
    param.x1 = x1;
    param.y1 = y1;
    param.x2 = x2;
    param.y2 = y2;
    param.line_width = (int)thickness;
    param.color = color;
    device_ioctl(ctx->draw_dev, DRAW_CMD_LINE, (uint8_t *)&param, sizeof(param));
}

static void pc_draw_rect_hw(const people_counting_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                            int x, int y, int w, int h, uint32_t thickness, uint32_t color)
{
    /* draw.c validates x/y >= 0 and x+w <= dst_width */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fw) w = (int)fw - x;
    if (y + h > (int)fh) h = (int)fh - y;
    if (w <= 0 || h <= 0) {
        return;
    }

    draw_rect_param_t param = {0};
    param.p_dst = fb;
    param.dst_width = (int)fw;
    param.dst_height = (int)fh;
    param.x_pos = x;
    param.y_pos = y;
    param.width = w;
    param.height = h;
    param.line_width = (int)thickness;
    param.color = color;
    device_ioctl(ctx->draw_dev, DRAW_CMD_RECT, (uint8_t *)&param, sizeof(param));
}

static void pc_draw_text_hw(const people_counting_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                            int x, int y, const char *str)
{
    draw_printf_param_t param = {0};
    param.p_font = (DRAW_Font_t *)&ctx->font16;
    param.p_dst = fb;
    param.dst_width = (int)fw;
    param.dst_height = (int)fh;
    param.x_pos = x;
    param.y_pos = y;
    strncpy(param.str, str, MAX_LINE_CHAR - 1);
    param.str[MAX_LINE_CHAR - 1] = '\0';
    device_ioctl(ctx->draw_dev, DRAW_CMD_PRINTF, (uint8_t *)&param, sizeof(param));
}

static void pc_blend_rect_hw(const people_counting_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                             int x, int y, int w, int h, uint32_t color, uint8_t alpha)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > (int)fw) w = (int)fw - x;
    if (y + h > (int)fh) h = (int)fh - y;
    if (w <= 0 || h <= 0) {
        return;
    }
    draw_colorrect_param_t param = {0};
    param.p_dst = fb;
    param.dst_width = (int)fw;
    param.dst_height = (int)fh;
    param.x_pos = x;
    param.y_pos = y;
    param.width = w;
    param.height = h;
    param.color = color;
    param.alpha = alpha;
    device_ioctl(ctx->draw_dev, DRAW_CMD_BLEND_COLOR_RECT, (uint8_t *)&param, sizeof(param));
}

/**
 * @brief One-shot draw subsystem bring-up (device + font + color mode)
 */
static aicam_bool_t pc_lazy_draw_init(people_counting_ctx_t *ctx)
{
    if (ctx->draw_dev && ctx->font_ready) {
        return AICAM_TRUE;
    }

    if (!ctx->draw_dev) {
        ctx->draw_dev = device_find_pattern(DRAW_DEVICE_NAME, DEV_TYPE_VIDEO);
        if (!ctx->draw_dev) {
            return AICAM_FALSE;
        }
        /* Same color mode contract as ai_draw_service (RGB565 in/out) */
        draw_colormode_param_t cm = {0};
        cm.in_colormode = fmt_dcmipp_to_dma2d(DCMIPP_PIXEL_PACKER_FORMAT_RGB565_1);
        cm.out_colormode = DMA2D_OUTPUT_RGB565;
        device_ioctl(ctx->draw_dev, DRAW_CMD_SET_COLOR_MODE, (uint8_t *)&cm, sizeof(cm));
    }

    if (!ctx->font_ready) {
        draw_fontsetup_param_t font_param = {0};
        font_param.p_font_in = &Font16;
        font_param.p_font = &ctx->font16;
        if (device_ioctl(ctx->draw_dev, DRAW_CMD_FONT_SETUP,
                         (uint8_t *)&font_param, sizeof(font_param)) != AICAM_OK) {
            return AICAM_FALSE;
        }
        ctx->font_ready = AICAM_TRUE;
    }

    return AICAM_TRUE;
}

void people_counting_draw_overlay(uint8_t *fb, uint32_t width, uint32_t height)
{
    if (!g_pc.initialized || !fb || width == 0 || height == 0) {
        return;
    }
    if (!g_pc.config.enabled) {
        return;
    }
    if (!pc_lazy_draw_init(&g_pc)) {
        return;
    }

    /* Snapshot shared state under the mutex, draw outside it */
    people_counting_config_t cfg;
    people_counting_stats_t stats;
    pc_face_track_t faces[PC_MAX_FACES];
    uint32_t face_count = 0;
    uint32_t now = osKernelGetTickCount();
    if (osMutexAcquire(g_pc.mutex, 20) == osOK) {
        memcpy(&cfg, &g_pc.config, sizeof(cfg));
        memcpy(&stats, &g_pc.stats, sizeof(stats));
        face_count = g_pc.face_count;
        memcpy(faces, g_pc.faces, sizeof(pc_face_track_t) * (face_count <= PC_MAX_FACES ? face_count : PC_MAX_FACES));
        /* Staleness guard: if the face model stopped delivering results
         * (disabled / error / pipeline stall), stop drawing its last frame. */
        if (face_count > 0 && (now - g_pc.last_face_tick) > PC_FACE_STALE_MS) {
            face_count = 0;
        }
        osMutexRelease(g_pc.mutex);
    } else {
        return;
    }

    /* 1) Face mosaic (drawn first so detection boxes stay on top). Boxes are
     * extrapolated to "now" with the tracked velocity so they keep up with
     * people moving between AI results (the AI pipeline runs a few frames
     * behind the encoded stream). */
    if (cfg.face_mosaic && face_count > 0) {
        for (uint32_t i = 0; i < face_count; i++) {
            float cx = faces[i].cx;
            float cy = faces[i].cy;
            uint32_t dt = now - faces[i].last_tick;
            if ((faces[i].vx != 0.0f || faces[i].vy != 0.0f) && dt > 0 && dt < 300) {
                cx += faces[i].vx * (float)dt;
                cy += faces[i].vy * (float)dt;
            }
            /* Keep the extrapolated box inside the frame */
            float hw = faces[i].w * 0.5f;
            float hh = faces[i].h * 0.5f;
            if (cx < hw) cx = hw;
            if (cx > 1.0f - hw) cx = 1.0f - hw;
            if (cy < hh) cy = hh;
            if (cy > 1.0f - hh) cy = 1.0f - hh;

            float expand = 1.0f + (float)cfg.mosaic_expand / 100.0f;
            float fw_n = faces[i].w * expand;
            float fh_n = faces[i].h * expand;
            int x0 = (int)((cx - fw_n * 0.5f) * (float)width);
            int y0 = (int)((cy - fh_n * 0.5f) * (float)height);
            int x1 = (int)((cx + fw_n * 0.5f) * (float)width);
            int y1 = (int)((cy + fh_n * 0.5f) * (float)height);
            pc_mosaic_region_rgb565((uint16_t *)fb, width, height,
                                    x0, y0, x1, y1, cfg.mosaic_block);
#if PC_DEBUG_FACE_BOXES
            /* Raw detection box (extrapolated, pre-expansion): box off-face
             * means a detection/coordinate problem; box on-face but mosaic
             * off means an expansion/mapping problem. */
            pc_draw_rect_hw(&g_pc, fb, width, height,
                            (int)((cx - hw) * (float)width),
                            (int)((cy - hh) * (float)height),
                            (int)(faces[i].w * (float)width),
                            (int)(faces[i].h * (float)height),
                            2, COLOR_CYAN);
#endif
        }
    }

    /* 2) Counting line + direction arrow */
    if (cfg.show_line) {
        int lx1 = (int)((float)cfg.line_x1 / PC_COORD_MAX * (float)width);
        int ly1 = (int)((float)cfg.line_y1 / PC_COORD_MAX * (float)height);
        int lx2 = (int)((float)cfg.line_x2 / PC_COORD_MAX * (float)width);
        int ly2 = (int)((float)cfg.line_y2 / PC_COORD_MAX * (float)height);

        pc_draw_line_hw(&g_pc, fb, width, height, lx1, ly1, lx2, ly2,
                        cfg.line_thickness, COLOR_YELLOW);

        /* Direction arrow at the line midpoint pointing toward the "in" side.
         * Side sign comes from cross = dx*(py-y1) - dy*(px-x1), so the
         * positive-side direction is (-dy, dx) (normalized below). */
        float dx = (float)(lx2 - lx1);
        float dy = (float)(ly2 - ly1);
        float len = dx * dx + dy * dy;
        if (len > 4.0f) {
            len = sqrtf(len);
            dx /= len;
            dy /= len;
            float nx = -dy;
            float ny = dx;
            int sign = cfg.invert_direction ? -1 : 1;
            int mx = (lx1 + lx2) / 2;
            int my = (ly1 + ly2) / 2;
            int arrow_len = 48;
            int head = 16;
            int tip_x = mx + (int)(nx * sign * arrow_len);
            int tip_y = my + (int)(ny * sign * arrow_len);
            /* Shaft */
            pc_draw_line_hw(&g_pc, fb, width, height, mx, my, tip_x, tip_y,
                            cfg.line_thickness, COLOR_GREEN);
            /* Head barbs: step back along the shaft, flare sideways along
             * the counting line (u = arrow direction, h = line direction) */
            int ux = (int)(nx * sign * head);
            int uy = (int)(ny * sign * head);
            int hx = (int)(dx * head);
            int hy = (int)(dy * head);
            int back_x = tip_x - ux;
            int back_y = tip_y - uy;
            pc_draw_line_hw(&g_pc, fb, width, height, tip_x, tip_y, back_x + hx, back_y + hy,
                            cfg.line_thickness, COLOR_GREEN);
            pc_draw_line_hw(&g_pc, fb, width, height, tip_x, tip_y, back_x - hx, back_y - hy,
                            cfg.line_thickness, COLOR_GREEN);
        }
    }

    /* 3) Count OSD (top-right) */
    if (cfg.show_osd) {
        char str[MAX_LINE_CHAR];
        snprintf(str, sizeof(str), "IN:%lu OUT:%lu NOW:%lu",
                 (unsigned long)stats.in_count,
                 (unsigned long)stats.out_count,
                 (unsigned long)stats.current_count);
        uint32_t text_w = ctx_font_width(&g_pc, str);
        uint32_t text_h = g_pc.font16.height;
        int x = (int)width - (int)text_w - 16;
        int y = 12;
        if (x < 0) x = 0;
        pc_blend_rect_hw(&g_pc, fb, width, height, x - 6, y - 4,
                         (int)text_w + 12, (int)text_h + 8, COLOR_BLACK, 140);
        pc_draw_text_hw(&g_pc, fb, width, height, x, y, str);
    }
}

/* Text width helper (font16 per-char advance) */
static uint32_t ctx_font_width(const people_counting_ctx_t *ctx, const char *str)
{
    uint32_t w = 0;
    for (const char *p = str; *p != '\0'; p++) {
        w += ctx->font16.width;
    }
    return w;
}

/* ==================== Lifecycle & Config API ==================== */

void people_counting_get_default_config(people_counting_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(people_counting_config_t));
    config->enabled = AICAM_TRUE;
    config->show_line = AICAM_TRUE;
    config->show_osd = AICAM_TRUE;
    config->face_mosaic = AICAM_TRUE;
    config->line_x1 = 600;
    config->line_y1 = 200;
    config->line_x2 = 600;
    config->line_y2 = 800;
    config->line_thickness = 3;
    config->invert_direction = AICAM_FALSE;
    config->track_iou_threshold = 30;
    config->track_max_missed = 10;
    config->cross_debounce = 2;
    config->min_box_area = 2000;
    config->mosaic_block = 16;
    config->mosaic_expand = 20;
    config->face_conf_threshold = 50;
    config->face_nms_threshold = 30;
}

aicam_result_t people_counting_init(void)
{
    if (g_pc.initialized) {
        return AICAM_OK;
    }

    g_pc.mutex = osMutexNew(NULL);
    if (!g_pc.mutex) {
        LOG_CORE_ERROR("People counting: failed to create mutex");
        return AICAM_ERROR;
    }

    people_counting_get_default_config(&g_pc.config);
    memset(&g_pc.stats, 0, sizeof(g_pc.stats));
    memset(g_pc.tracks, 0, sizeof(g_pc.tracks));
    memset(g_pc.faces, 0, sizeof(g_pc.faces));
    g_pc.next_track_id = 0;
    g_pc.last_person_frame_id = 0;
    g_pc.face_count = 0;
    g_pc.stats.enabled = AICAM_TRUE;

    g_pc.initialized = AICAM_TRUE;
    LOG_CORE_INFO("People counting module initialized");
    return AICAM_OK;
}

aicam_result_t people_counting_deinit(void)
{
    if (!g_pc.initialized) {
        return AICAM_OK;
    }
    g_pc.initialized = AICAM_FALSE;

    if (g_pc.font16.data) {
        buffer_free(g_pc.font16.data);
        g_pc.font16.data = NULL;
    }
    g_pc.font_ready = AICAM_FALSE;
    g_pc.draw_dev = NULL;

    if (g_pc.mutex) {
        osMutexDelete(g_pc.mutex);
        g_pc.mutex = NULL;
    }
    return AICAM_OK;
}

aicam_bool_t people_counting_is_initialized(void)
{
    return g_pc.initialized;
}

aicam_result_t people_counting_get_config(people_counting_config_t *config)
{
    if (!config) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!g_pc.initialized) {
        people_counting_get_default_config(config);
        return AICAM_OK;
    }
    if (osMutexAcquire(g_pc.mutex, osWaitForever) == osOK) {
        memcpy(config, &g_pc.config, sizeof(people_counting_config_t));
        osMutexRelease(g_pc.mutex);
        return AICAM_OK;
    }
    return AICAM_ERROR;
}

aicam_result_t people_counting_set_config(const people_counting_config_t *config)
{
    if (!config || !g_pc.initialized) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (osMutexAcquire(g_pc.mutex, osWaitForever) == osOK) {
        memcpy(&g_pc.config, config, sizeof(people_counting_config_t));
        g_pc.stats.enabled = config->enabled;
        osMutexRelease(g_pc.mutex);
        return AICAM_OK;
    }
    return AICAM_ERROR;
}

aicam_result_t people_counting_get_stats(people_counting_stats_t *stats)
{
    if (!stats) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!g_pc.initialized) {
        memset(stats, 0, sizeof(people_counting_stats_t));
        people_counting_config_t cfg;
        people_counting_get_default_config(&cfg);
        stats->enabled = cfg.enabled;
        return AICAM_OK;
    }
    if (osMutexAcquire(g_pc.mutex, 20) == osOK) {
        memcpy(stats, &g_pc.stats, sizeof(people_counting_stats_t));
        stats->enabled = g_pc.config.enabled;
        osMutexRelease(g_pc.mutex);
        return AICAM_OK;
    }
    return AICAM_ERROR_TIMEOUT;
}

aicam_result_t people_counting_reset_stats(void)
{
    if (!g_pc.initialized) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }
    if (osMutexAcquire(g_pc.mutex, osWaitForever) == osOK) {
        g_pc.stats.in_count = 0;
        g_pc.stats.out_count = 0;
        g_pc.stats.net_count = 0;
        memset(g_pc.tracks, 0, sizeof(g_pc.tracks));
        osMutexRelease(g_pc.mutex);
        LOG_CORE_INFO("People counting statistics reset");
        return AICAM_OK;
    }
    return AICAM_ERROR;
}

void people_counting_set_face_model_loaded(aicam_bool_t loaded)
{
    if (!g_pc.initialized) {
        return;
    }
    if (osMutexAcquire(g_pc.mutex, 20) == osOK) {
        g_pc.stats.face_model_loaded = loaded;
        osMutexRelease(g_pc.mutex);
    }
}

void people_counting_motion_compensate(float cx, float cy, float *dx, float *dy)
{
    if (dx) {
        *dx = 0.0f;
    }
    if (dy) {
        *dy = 0.0f;
    }
    if (!g_pc.initialized || !dx || !dy) {
        return;
    }

    if (osMutexAcquire(g_pc.mutex, 10) != osOK) {
        return;
    }

    uint32_t now = osKernelGetTickCount();
    int best = -1;
    float best_d2 = 0.3f * 0.3f;
    for (int t = 0; t < PC_MAX_TRACKS; t++) {
        const pc_track_t *track = &g_pc.tracks[t];
        if (!track->active) {
            continue;
        }
        float ddx = cx - track->cx;
        float ddy = cy - track->cy;
        float d2 = ddx * ddx + ddy * ddy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = t;
        }
    }
    if (best >= 0) {
        const pc_track_t *track = &g_pc.tracks[best];
        uint32_t dt = now - track->last_tick;
        if (track->last_tick != 0 && dt > 500) {
            dt = 500; /* cap the extrapolation horizon */
        }
        float sx = track->vx * (float)dt;
        float sy = track->vy * (float)dt;
        if (sx > 0.12f) sx = 0.12f;
        if (sx < -0.12f) sx = -0.12f;
        if (sy > 0.12f) sy = 0.12f;
        if (sy < -0.12f) sy = -0.12f;
        *dx = sx;
        *dy = sy;
    }

    osMutexRelease(g_pc.mutex);
}

