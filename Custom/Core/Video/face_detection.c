/**
 * @file face_detection.c
 * @brief Face detection demo implementation
 * @details IoU tracker + face mosaic + expanded-crop JPEG reporting +
 *          recognition-result cache, drawn as burned-in overlay on the
 *          camera PIPE1 RGB565 frames. See face_detection.h for the
 *          threading model and Docs/Face_Detection_Demo.md for the protocol.
 */

#include "face_detection.h"
#include "buffer_mgr.h"
#include "debug.h"
#include "dev_manager.h"
#include "draw.h"
#include "fonts.h"
#include "mem.h"
#include "pixel_format_map.h"
#include "image_utils.h"
#include "ai_service.h"
#include "webhook_service.h"
#include "upload_coordinator.h"
#include "mqtt_service.h"
#include "device_service.h"
#include "json_config_mgr.h"
#include "cJSON.h"
#include "cmsis_os2.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ==================== Internal Definitions ==================== */

/* Stop drawing when the last AI result is older than this (ms) */
#define FD_STALE_MS 1200U

/* Default keep-alive for the last boxes when a batch comes back empty
 * (anti-blink). Overridable per config (face_hold_ms). */
#define FD_HOLD_MS_DEFAULT 400U

/* Report worker queue depth (JPEGs are heap copies; small depth by design) */
#define FD_REPORT_QUEUE_DEPTH 4U

/* Report worker stack */
#define FD_WORKER_STACK_SIZE (1024U * 12U)

/* RGB565 grey used for the letterbox padding */
#define FD_LETTERBOX_GREY 0x8410U

typedef struct {
    aicam_bool_t active;
    uint32_t id;                          /* Track id (drawing/reporting) */
    float x, y, w, h;                     /* Last matched box (normalized) */
    float cx, cy;                         /* Centroid (normalized) */
    float vx, vy;                         /* Centroid velocity (norm/ms, EMA) */
    uint32_t last_tick;                   /* Tick of the last match */
    uint32_t missed;                      /* Consecutive frames unmatched */
    uint32_t hits;                        /* Consecutive frames matched */
    aicam_bool_t confirmed;               /* Survived track_confirm_frames */
    float det_conf;                       /* Last detection confidence */
    float lm[FD_MAX_LANDMARKS][2];        /* Landmarks (normalized) */
    uint32_t nb_landmarks;                /* 0 for keypoint-less models */
    /* Reporting state */
    aicam_bool_t need_report;             /* Crop+queue on the next frame */
    uint32_t report_reason;               /* fd_report_reason_t */
    uint32_t last_report_tick;            /* Tick when a report was queued */
    char request_id[24];                  /* In-flight request (protocol) */
    uint32_t request_tick;
    aicam_bool_t request_in_flight;
    /* Recognition result cache */
    fd_result_status_t result_status;
    char result_name[FD_RESULT_NAME_MAX];
    float result_conf;
    uint32_t result_tick;
} fd_track_t;

/* Report handed from the camera thread (fast crop only) to the worker,
 * which does the CPU-heavy JPEG encode off the frame path */
typedef struct {
    uint8_t *raw;                         /* RGB565 crop, camera thread allocs */
    uint32_t raw_w, raw_h;                /* Encode dimensions */
    uint8_t *jpeg;                        /* Encoded by the worker */
    uint32_t jpeg_size;
    uint32_t img_w, img_h;
    aicam_bool_t resized;                 /* Fixed-size letterbox applied */
    uint32_t quality;
    uint32_t track_id;
    uint32_t frame_id;
    uint32_t timestamp_ms;                /* Kernel tick at crop time */
    char request_id[24];
    uint32_t reason;
    float bbox[4];                        /* Original normalized box */
    float ebbox[4];                       /* Expanded + clamped */
    float det_conf;
    uint32_t nb_landmarks;
    float landmarks[FD_MAX_LANDMARKS][2];
} fd_report_msg_t;

typedef struct {
    aicam_bool_t initialized;
    osMutexId_t mutex;
    face_detection_config_t config;
    face_detection_stats_t stats;
    fd_track_t tracks[FD_MAX_TRACKS];
    uint32_t next_track_id;
    uint32_t request_counter;
    uint32_t last_frame_id;
    uint32_t last_result_tick;            /* Tick of the last AI result */
    uint32_t empty_since;                 /* Empty-batch hold anchor */
    /* Report worker */
    osMessageQueueId_t report_queue;
    /* One-shot verification slot (request ids "fdv-") */
    struct {
        char request_id[24];
        volatile aicam_bool_t done;
        char name[FD_RESULT_NAME_MAX];
        float conf;
        uint32_t status; /* 0 ok 1 unknown 2 no_face 3 error */
    } verify;
    osThreadId_t worker_thread;
    aicam_bool_t worker_running;
    /* Draw state (camera thread only) */
    device_t *draw_dev;
    DRAW_Font_t font16;
    aicam_bool_t font_ready;
} face_detection_ctx_t;

static face_detection_ctx_t g_fd = {0};
static uint8_t fd_worker_stack[FD_WORKER_STACK_SIZE] __attribute__((aligned(32)));

/* ==================== Internal Function Declarations ==================== */

static void fd_ensure_init(void);
static float fd_box_iou(const fd_track_t *t, float x, float y, float w, float h);
static aicam_bool_t fd_class_is_face(const char *class_name);
static void fd_age_tracks(face_detection_ctx_t *ctx, uint32_t now);
static void fd_decide_reports(face_detection_ctx_t *ctx, uint32_t now);
static void fd_mosaic_region_rgb565(uint16_t *fb, uint32_t fw, uint32_t fh,
                                    int x0, int y0, int x1, int y1, uint32_t block);
static void fd_draw_line_hw(const face_detection_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                            int x1, int y1, int x2, int y2, uint32_t thickness, uint32_t color);
static void fd_draw_rect_hw(const face_detection_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                            int x, int y, int w, int h, uint32_t thickness, uint32_t color);
static void fd_draw_text_hw(const face_detection_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                            int x, int y, const char *str);
static void fd_blend_rect_hw(const face_detection_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                             int x, int y, int w, int h, uint32_t color, uint8_t alpha);
static uint32_t fd_font_width(const face_detection_ctx_t *ctx, const char *str);
static aicam_bool_t fd_lazy_draw_init(face_detection_ctx_t *ctx);
static void fd_queue_reports(face_detection_ctx_t *ctx, const fd_track_t *tracks,
                             uint32_t count, uint8_t *fb, uint32_t fw, uint32_t fh,
                             uint32_t frame_now);
static void fd_report_worker(void *arg);
static void fd_worker_resurrect(void);
static void fd_archive_record(const fd_report_msg_t *msg);

static void fd_send_report(fd_report_msg_t *msg);
static int fd_base64_encode(const uint8_t *in, uint32_t in_len, char *out, uint32_t out_size);
static void fd_ascii_name(const char *in, char *out, uint32_t size);
static const char *fd_reason_str(uint32_t reason);

/* ==================== Helpers ==================== */

static float fd_box_iou(const fd_track_t *t, float x, float y, float w, float h)
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

/* Face models emit "face"; class-agnostic detectors emit an empty name.
 * Anything else (e.g. a person OD model) is rejected so the demo does not
 * treat persons as faces. */
static aicam_bool_t fd_class_is_face(const char *class_name)
{
    if (!class_name || class_name[0] == '\0') {
        return AICAM_TRUE;
    }
    for (const char *p = class_name; *p != '\0'; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c == 'f' || c == 'a' || c == 'c' || c == 'e') {
            continue;
        }
        return AICAM_FALSE;
    }
    return AICAM_TRUE;
}

static const char *fd_reason_str(uint32_t reason)
{
    switch (reason) {
        case FD_REASON_NEW_TRACK: return "new_track";
        case FD_REASON_PERIODIC:  return "periodic";
        case FD_REASON_MANUAL:    return "manual";
        default:                  return "unknown";
    }
}

/* Replace non-ASCII bytes with '?' so Font16 rendering stays sane; an empty
 * result means the name was not renderable at all. */
static void fd_ascii_name(const char *in, char *out, uint32_t size)
{
    uint32_t o = 0;
    for (const char *p = in; p && *p != '\0' && o + 1 < size; p++) {
        char c = *p;
        if (c < 0x20) {
            c = ' ';
        } else if ((uint8_t)c >= 0x80) {
            c = '?';
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

/* ==================== Lazy Init / Lifecycle ==================== */

static void fd_ensure_init(void)
{
    if (g_fd.initialized) {
        return;
    }
    if (g_fd.mutex == NULL) {
        g_fd.mutex = osMutexNew(NULL);
        if (g_fd.mutex == NULL) {
            return;
        }
    }
    if (osMutexAcquire(g_fd.mutex, osWaitForever) != osOK) {
        return;
    }
    if (!g_fd.initialized) {
        face_detection_config_t cfg;
        if (json_config_get_face_detection_config(&cfg) != AICAM_OK) {
            face_detection_get_default_config(&cfg);
        }
        g_fd.config = cfg;

        memset(g_fd.tracks, 0, sizeof(g_fd.tracks));
        memset(&g_fd.stats, 0, sizeof(g_fd.stats));
        g_fd.next_track_id = 0;
        g_fd.request_counter = 0;
        g_fd.last_frame_id = 0;
        g_fd.last_result_tick = 0;
        g_fd.empty_since = 0;

        /* TX_QUEUE messages are limited to 16 ULONGs: queue POINTERS to
         * heap-allocated fd_report_msg_t, never the struct itself. */
        g_fd.report_queue = osMessageQueueNew(FD_REPORT_QUEUE_DEPTH,
                                              sizeof(fd_report_msg_t *), NULL);
        if (!g_fd.report_queue) {
            LOG_CORE_ERROR("Face detection: report queue creation failed");
        }
        if (g_fd.report_queue) {
            osThreadAttr_t attr = {
                .name = "fd_report",
                .stack_mem = fd_worker_stack,
                .stack_size = sizeof(fd_worker_stack),
                .priority = osPriorityBelowNormal,
            };
            g_fd.worker_running = AICAM_TRUE;
            g_fd.worker_thread = osThreadNew(fd_report_worker, NULL, &attr);
            if (!g_fd.worker_thread) {
                g_fd.worker_running = AICAM_FALSE;
                LOG_CORE_ERROR("Face detection: report worker thread creation failed");
            }
        }

        g_fd.initialized = AICAM_TRUE;
        LOG_CORE_INFO("Face detection module initialized (enabled=%d, queue=%p, worker=%d)",
                      g_fd.config.enabled, (void *)g_fd.report_queue,
                      g_fd.worker_running ? 1 : 0);
    }
    osMutexRelease(g_fd.mutex);
}

/* ==================== Detection Input (AI node thread) ==================== */

void face_detection_on_result(const nn_result_t *result, uint32_t frame_id,
                              uint32_t inference_time_ms, void *user_data)
{
    (void)inference_time_ms;
    (void)user_data;
    fd_ensure_init();
    if (!g_fd.initialized || !result) {
        return;
    }

    uint32_t now = osKernelGetTickCount();

    /* Collect faces from both face-model output flavors: OD (yunet,
     * keypoint-less) and MPE (blazeface-style with keypoints). */
    float nx[FD_MAX_TRACKS], ny[FD_MAX_TRACKS], nw[FD_MAX_TRACKS], nh[FD_MAX_TRACKS];
    float nconf[FD_MAX_TRACKS];
    float nlm[FD_MAX_TRACKS][FD_MAX_LANDMARKS][2];
    uint32_t nbl[FD_MAX_TRACKS];
    uint32_t count = 0;

    if (result->is_valid && (result->type == PP_TYPE_OD || result->type == PP_TYPE_MPE)) {
        uint32_t nb = (result->type == PP_TYPE_OD) ? result->od.nb_detect
                                                   : result->mpe.nb_detect;
        if (nb > FD_MAX_TRACKS) {
            nb = FD_MAX_TRACKS;
        }
        for (uint32_t i = 0; i < nb && count < FD_MAX_TRACKS; i++) {
            float x, y, w, h, conf;
            uint32_t kpl = 0;
            if (result->type == PP_TYPE_OD) {
                const od_detect_t *d = &result->od.detects[i];
                if (!fd_class_is_face(d->class_name)) {
                    continue;
                }
                x = d->x; y = d->y; w = d->width; h = d->height; conf = d->conf;
            } else {
                const mpe_detect_t *d = &result->mpe.detects[i];
                if (!fd_class_is_face(d->class_name)) {
                    continue;
                }
                x = d->x; y = d->y; w = d->width; h = d->height; conf = d->conf;
                kpl = d->nb_keypoints;
                if (kpl > FD_MAX_LANDMARKS) {
                    kpl = FD_MAX_LANDMARKS;
                }
            }
            if (w <= 0.0f || h <= 0.0f || conf <= 0.0f) {
                continue;
            }
            if (x < 0.0f) x = 0.0f;
            if (y < 0.0f) y = 0.0f;
            if (x + w > 1.0f) w = 1.0f - x;
            if (y + h > 1.0f) h = 1.0f - y;
            if (w <= 0.0f || h <= 0.0f) {
                continue;
            }

            nx[count] = x; ny[count] = y; nw[count] = w; nh[count] = h;
            nconf[count] = conf;
            nbl[count] = kpl;
            memset(nlm[count], 0, sizeof(nlm[count]));
            if (kpl > 0 && result->type == PP_TYPE_MPE) {
                const mpe_detect_t *d = &result->mpe.detects[i];
                for (uint32_t k = 0; k < kpl; k++) {
                    nlm[count][k][0] = d->keypoints[k].x;
                    nlm[count][k][1] = d->keypoints[k].y;
                }
            }
            count++;
        }
    }

    if (osMutexAcquire(g_fd.mutex, 50) != osOK) {
        return;
    }

    face_detection_config_t *cfg = &g_fd.config;

    if (!cfg->enabled) {
        g_fd.last_result_tick = now;
        osMutexRelease(g_fd.mutex);
        return;
    }

    /* Guard against a stale/duplicated result delivery */
    if (frame_id != 0 && frame_id == g_fd.last_frame_id) {
        osMutexRelease(g_fd.mutex);
        return;
    }
    g_fd.last_frame_id = frame_id;
    g_fd.last_result_tick = now;

    /* Detection-confidence + min-area filter (permille^2). The confidence
     * gate drops background false positives before they spawn (unconfirmed)
     * tracks. */
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t area_pm2 = (uint32_t)(nw[i] * 1000.0f * nh[i] * 1000.0f);
        if (nconf[i] * 100.0f >= (float)cfg->detection_conf_threshold &&
            area_pm2 >= cfg->min_face_area) {
            nx[kept] = nx[i]; ny[kept] = ny[i];
            nw[kept] = nw[i]; nh[kept] = nh[i];
            nconf[kept] = nconf[i];
            nbl[kept] = nbl[i];
            memcpy(nlm[kept], nlm[i], sizeof(nlm[i]));
            kept++;
        }
    }
    count = kept;
    g_fd.stats.last_detections = count;

    /* Empty batch: hold the last tracks briefly (anti-blink) then age them
     * through the normal missed counter. */
    if (count == 0 && g_fd.empty_since == 0) {
        g_fd.empty_since = now;
    }
    if (count > 0) {
        g_fd.empty_since = 0;
    }
    if (count == 0 && (now - g_fd.empty_since) < cfg->face_hold_ms &&
        g_fd.empty_since != 0) {
        fd_age_tracks(&g_fd, now);
        fd_decide_reports(&g_fd, now);
        osMutexRelease(g_fd.mutex);
        return;
    }

    /* Greedy IoU matching (best pair first), one match per track/detection */
    aicam_bool_t det_matched[FD_MAX_TRACKS] = {AICAM_FALSE};
    aicam_bool_t track_matched[FD_MAX_TRACKS] = {AICAM_FALSE};
    float iou_thr = (float)cfg->track_iou_threshold / 100.0f;
    uint32_t confirmed_after = cfg->track_confirm_frames ? cfg->track_confirm_frames : 1;

    for (uint32_t round = 0; round < count; round++) {
        float best_iou = iou_thr;
        int best_track = -1;
        int best_det = -1;
        for (int t = 0; t < FD_MAX_TRACKS; t++) {
            if (!g_fd.tracks[t].active || track_matched[t]) {
                continue;
            }
            for (uint32_t d = 0; d < count; d++) {
                if (det_matched[d]) {
                    continue;
                }
                float iou = fd_box_iou(&g_fd.tracks[t], nx[d], ny[d], nw[d], nh[d]);
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
        fd_track_t *track = &g_fd.tracks[best_track];
        float mcx = nx[best_det] + nw[best_det] * 0.5f;
        float mcy = ny[best_det] + nh[best_det] * 0.5f;
        uint32_t vdt = now - track->last_tick;
        if (track->last_tick != 0 && vdt > 0 && vdt < 500) {
            float ivx = (mcx - track->cx) / (float)vdt;
            float ivy = (mcy - track->cy) / (float)vdt;
            track->vx = (track->vx == 0.0f) ? ivx : track->vx * 0.5f + ivx * 0.5f;
            track->vy = (track->vy == 0.0f) ? ivy : track->vy * 0.5f + ivy * 0.5f;
        }
        track->x = nx[best_det];
        track->y = ny[best_det];
        track->w = nw[best_det];
        track->h = nh[best_det];
        track->cx = mcx;
        track->cy = mcy;
        track->det_conf = nconf[best_det];
        track->nb_landmarks = nbl[best_det];
        memcpy(track->lm, nlm[best_det], sizeof(track->lm));
        track->missed = 0;
        if (track->hits < confirmed_after) {
            track->hits++;
        }
        track->last_tick = now;
        det_matched[best_det] = AICAM_TRUE;
        track_matched[best_track] = AICAM_TRUE;

        if (!track->confirmed && track->hits >= confirmed_after) {
            track->confirmed = AICAM_TRUE;
            track->id = ++g_fd.next_track_id; /* dense: ids == confirmed count */
            g_fd.stats.total_tracks++; /* cumulative CONFIRMED faces only */
            /* A freshly confirmed face must be reported exactly once,
             * regardless of cooldown (doc section 5 / decision #3). */
            if (cfg->report_enabled) {
                track->need_report = AICAM_TRUE;
                track->report_reason = FD_REASON_NEW_TRACK;
            }
        }
    }

    /* Create tracks for unmatched detections */
    for (uint32_t d = 0; d < count; d++) {
        if (det_matched[d]) {
            continue;
        }
        for (int t = 0; t < FD_MAX_TRACKS; t++) {
            fd_track_t *track = &g_fd.tracks[t];
            if (track->active) {
                continue;
            }
            memset(track, 0, sizeof(fd_track_t));
            track->active = AICAM_TRUE; /* id assigned on confirmation */
            track->x = nx[d];
            track->y = ny[d];
            track->w = nw[d];
            track->h = nh[d];
            track->cx = track->x + track->w * 0.5f;
            track->cy = track->y + track->h * 0.5f;
            track->vx = 0.0f;
            track->vy = 0.0f;
            track->det_conf = nconf[d];
            track->nb_landmarks = nbl[d];
            memcpy(track->lm, nlm[d], sizeof(track->lm));
            track->hits = 1;
            track->last_tick = now;
            track->result_status = FD_RESULT_NONE;
            if (confirmed_after <= 1) {
                track->confirmed = AICAM_TRUE;
                track->id = ++g_fd.next_track_id; /* dense: ids == confirmed count */
                g_fd.stats.total_tracks++; /* cumulative CONFIRMED faces only */
                if (cfg->report_enabled) {
                    track->need_report = AICAM_TRUE;
                    track->report_reason = FD_REASON_NEW_TRACK;
                }
            }
            break;
        }
    }

    /* Age unmatched tracks and drop expired ones */
    uint32_t active_confirmed = 0;
    for (int t = 0; t < FD_MAX_TRACKS; t++) {
        fd_track_t *track = &g_fd.tracks[t];
        if (!track->active) {
            continue;
        }
        if (!track_matched[t]) {
            track->missed++;
            track->hits = 0;
            if (track->missed > cfg->track_max_missed) {
                memset(track, 0, sizeof(fd_track_t)); /* deactivate */
                continue;
            }
        }
        if (track->confirmed) {
            active_confirmed++;
        }
    }
    g_fd.stats.confirmed_tracks = active_confirmed;
    g_fd.stats.last_update_ms = now;

    fd_age_tracks(&g_fd, now);
    fd_decide_reports(&g_fd, now);

    osMutexRelease(g_fd.mutex);
}

/* Result timeouts and expiry aging (mutex held) */
static void fd_age_tracks(face_detection_ctx_t *ctx, uint32_t now)
{
    for (int t = 0; t < FD_MAX_TRACKS; t++) {
        fd_track_t *track = &ctx->tracks[t];
        if (!track->active) {
            continue;
        }
        /* Pending request past the wait window -> timeout. Covers both a
         * queued report awaiting its answer and a manual trigger whose
         * crop/encode/send never completed (no in-flight request). */
        if (track->result_status == FD_RESULT_PENDING &&
            (now - track->request_tick) > ctx->config.result_timeout_ms) {
            track->request_in_flight = AICAM_FALSE;
            track->result_status = FD_RESULT_TIMEOUT;
            track->result_tick = now;
            ctx->stats.results_timeout++;
        }
        /* Result retention on a live track (doc 5.4-4) */
        if (track->result_status >= FD_RESULT_OK && track->result_status != FD_RESULT_PENDING &&
            (now - track->result_tick) > ctx->config.result_expire_ms) {
            track->result_status = FD_RESULT_NONE;
        }
    }
}

/* Periodic re-report decisions (mutex held) */
static void fd_decide_reports(face_detection_ctx_t *ctx, uint32_t now)
{
    if (!ctx->config.report_enabled || !ctx->config.report_periodic_enabled) {
        return;
    }
    for (int t = 0; t < FD_MAX_TRACKS; t++) {
        fd_track_t *track = &ctx->tracks[t];
        if (!track->active || !track->confirmed || track->need_report) {
            continue;
        }
        /* Only re-report once the previous result expired (or never came) */
        if (track->result_status != FD_RESULT_NONE) {
            continue;
        }
        if (track->last_report_tick != 0 &&
            (now - track->last_report_tick) < ctx->config.report_cooldown_ms) {
            continue;
        }
        track->need_report = AICAM_TRUE;
        track->report_reason = FD_REASON_PERIODIC;
    }
}

/* ==================== Overlay Drawing + Report Crops (camera thread) ==================== */

static void fd_mosaic_region_rgb565(uint16_t *fb, uint32_t fw, uint32_t fh,
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
            /* Sampled average: at most 4x4 samples per block instead of
             * every pixel — visually equivalent for blocks >= 8 and an
             * order of magnitude cheaper on the camera thread. */
            int bw = xe - bx;
            int bh = ye - by;
            int step_x = (bw > 4) ? (bw + 3) / 4 : 1;
            int step_y = (bh > 4) ? (bh + 3) / 4 : 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = by; y < ye; y += step_y) {
                const uint16_t *row = fb + (size_t)y * fw + bx;
                for (int x = bx; x < xe; x += step_x) {
                    uint16_t p = row[x - bx];
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

static void fd_draw_line_hw(const face_detection_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                            int x1, int y1, int x2, int y2, uint32_t thickness, uint32_t color)
{
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

static void fd_draw_rect_hw(const face_detection_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                            int x, int y, int w, int h, uint32_t thickness, uint32_t color)
{
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

static void fd_draw_text_hw(const face_detection_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
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

static void fd_blend_rect_hw(const face_detection_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
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

static uint32_t fd_font_width(const face_detection_ctx_t *ctx, const char *str)
{
    uint32_t w = 0;
    for (const char *p = str; *p != '\0'; p++) {
        w += ctx->font16.width;
    }
    return w;
}

static aicam_bool_t fd_lazy_draw_init(face_detection_ctx_t *ctx)
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

/**
 * @brief Compute the motion-compensated center of a track ("now")
 * @param extrapolate AICAM_FALSE returns the last matched center as-is
 *                    (fast models: inference barely lags the stream)
 */
static void fd_track_center_now(const fd_track_t *track, uint32_t now,
                                aicam_bool_t extrapolate,
                                float *cx, float *cy)
{
    float x = track->cx, y = track->cy;
    if (extrapolate) {
        uint32_t dt = now - track->last_tick;
        if ((track->vx != 0.0f || track->vy != 0.0f) && dt > 0 && dt < 300) {
            x += track->vx * (float)dt;
            y += track->vy * (float)dt;
        }
    }
    /* Keep the box inside the frame */
    float hw = track->w * 0.5f;
    float hh = track->h * 0.5f;
    if (x < hw) x = hw;
    if (x > 1.0f - hw) x = 1.0f - hw;
    if (y < hh) y = hh;
    if (y > 1.0f - hh) y = 1.0f - hh;
    *cx = x;
    *cy = y;
}

/**
 * @brief Expanded+clamped pixel rect around a track's extrapolated box
 * @return AICAM_FALSE when the rect is smaller than crop_min_size
 */
static aicam_bool_t fd_expanded_rect(const face_detection_config_t *cfg,
                                     const fd_track_t *track, uint32_t now,
                                     uint32_t fw, uint32_t fh,
                                     int *rx, int *ry, int *rw, int *rh)
{
    float cx, cy;
    fd_track_center_now(track, now, cfg->extrapolation_enabled, &cx, &cy);

    float expand = 1.0f + (float)cfg->crop_expand_ratio / 100.0f;
    float ew = track->w * expand;
    float eh = track->h * expand;
    if (ew > 1.0f) ew = 1.0f;
    if (eh > 1.0f) eh = 1.0f;

    int x0 = (int)((cx - ew * 0.5f) * (float)fw);
    int y0 = (int)((cy - eh * 0.5f) * (float)fh);
    int x1 = (int)((cx + ew * 0.5f) * (float)fw);
    int y1 = (int)((cy + eh * 0.5f) * (float)fh);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)fw) x1 = (int)fw;
    if (y1 > (int)fh) y1 = (int)fh;

    *rx = x0;
    *ry = y0;
    *rw = x1 - x0;
    *rh = y1 - y0;
    return (*rw >= (int)cfg->crop_min_size && *rh >= (int)cfg->crop_min_size)
               ? AICAM_TRUE
               : AICAM_FALSE;
}

/**
 * @brief Crop one face on the camera thread (fast, no JPEG here)
 * @details Produces the raw RGB565 encode source: native 16-aligned crop, or
 *          the fixed-size letterbox square. JPEG encoding happens on the
 *          report worker so multi-face frames do not stall the pipeline.
 * @param raw_out Heap RGB565 buffer (w*h*2), caller frees via buffer_free
 * @return AICAM_OK on success
 */
static aicam_result_t fd_crop_face(const face_detection_config_t *cfg,
                                   uint8_t *fb, uint32_t fw, uint32_t fh,
                                   int rx, int ry, int rw, int rh,
                                   uint8_t **raw_out, uint32_t *out_w,
                                   uint32_t *out_h, aicam_bool_t *resized_out)
{
    uint32_t src_format = fmt_dcmipp_to_dma2d(DCMIPP_PIXEL_PACKER_FORMAT_RGB565_1);
    /* Pad the crop to multiples of 16 and always use 420 subsampling — the
     * exact geometry of the known-good capture path (1280x720/420). The 444
     * fallback for 8-multiple sizes made the JPEG HW stall indefinitely.
     * 1280x720 are multiples of 16, so frame clamping keeps alignment. */
    uint32_t enc_w = ((uint32_t)rw + 15U) & ~15U;
    uint32_t enc_h = ((uint32_t)rh + 15U) & ~15U;
    int ex = rx, ey = ry;
    if (ex + (int)enc_w > (int)fw) ex = (int)fw - (int)enc_w;
    if (ex < 0) ex = 0;
    if (ey + (int)enc_h > (int)fh) ey = (int)fh - (int)enc_h;
    if (ey < 0) ey = 0;

    uint8_t *tmp = (uint8_t *)buffer_calloc(1, (size_t)enc_w * enc_h * 2);
    if (!tmp) {
        return AICAM_ERROR_NO_MEMORY;
    }

    if (cfg->report_fixed_size == 0) {
        if (image_crop_roi(fb, fw, fh, src_format, ex, ey, enc_w, enc_h,
                           tmp, DMA2D_OUTPUT_RGB565) != AICAM_OK) {
            buffer_free(tmp);
            return AICAM_ERROR;
        }
        *out_w = enc_w;
        *out_h = enc_h;
        *resized_out = AICAM_FALSE;
    } else {
        /* Fixed square: scale to fit (no stretch), then grey pad. Round N up
         * to a multiple of 16 so 420 subsampling is always accepted. */
        uint32_t N = (cfg->report_fixed_size + 15U) & ~15U;
        if (N > FD_REPORT_FIXED_SIZE_MAX) N = FD_REPORT_FIXED_SIZE_MAX;
        float scale_x = (float)N / (float)rw;
        float scale_y = (float)N / (float)rh;
        float scale = (scale_x < scale_y) ? scale_x : scale_y;
        if (scale > 1.0f) {
            scale = 1.0f; /* do not upscale beyond the crop */
        }
        uint32_t fit_w = (uint32_t)((float)rw * scale);
        uint32_t fit_h = (uint32_t)((float)rh * scale);
        if (fit_w == 0) fit_w = 1;
        if (fit_h == 0) fit_h = 1;

        od_detect_t bbox = {
            .x = (float)rx / (float)fw,
            .y = (float)ry / (float)fh,
            .width = (float)rw / (float)fw,
            .height = (float)rh / (float)fh,
            .conf = 1.0f,
            .class_name = NULL,
        };
        uint8_t *fit = NULL;
        uint32_t fit_size = 0;
        if (image_extract_and_resize_roi(fb, fw, fh, src_format, &bbox,
                                         fit_w, fit_h, DMA2D_OUTPUT_RGB565,
                                         &fit, &fit_size) != AICAM_OK || !fit) {
            buffer_free(tmp);
            return AICAM_ERROR;
        }

        uint16_t *square = (uint16_t *)tmp;
        uint16_t grey = FD_LETTERBOX_GREY;
        for (uint32_t i = 0; i < N * N; i++) {
            square[i] = grey;
        }
        uint32_t off_x = (N - fit_w) / 2;
        uint32_t off_y = (N - fit_h) / 2;
        for (uint32_t row = 0; row < fit_h; row++) {
            memcpy(square + (size_t)(off_y + row) * N + off_x,
                   fit + (size_t)row * fit_w * 2, (size_t)fit_w * 2);
        }
        buffer_free(fit);

        *out_w = N;
        *out_h = N;
        *resized_out = AICAM_TRUE;
    }

    *raw_out = tmp;
    return AICAM_OK;
}

/**
 * @brief JPEG-encode a raw RGB565 crop (worker thread) with one retry
 * @details jpegc's completion semaphore accounting can be poisoned by a
 *          previous HW stall (timeout leaves a stray token), so a single
 *          retry recovers those cases.
 */
static aicam_result_t fd_encode_raw(const face_detection_config_t *cfg,
                                    uint8_t *raw, uint32_t w, uint32_t h,
                                    uint8_t **jpeg_out, uint32_t *jpeg_size_out)
{
    ai_jpeg_encode_config_t enc = {
        .width = w,
        .height = h,
        .chroma_subsampling = JPEG_420_SUBSAMPLING, /* 16-multiple, known-good */
        .quality = cfg->jpeg_quality,
    };
    uint8_t *jpeg = NULL;
    uint32_t jpeg_size = 0;
    aicam_result_t ret = AICAM_ERROR;

    /* Retry harder: a single jpegc HW stall leaves a stray completion token
     * that makes the next OUTPUT fail instantly; each retry drains one. */
    for (int attempt = 0; attempt < 4; attempt++) {
        jpeg = NULL;
        ret = ai_jpeg_encode(raw, w * h * 2, &enc, &jpeg, &jpeg_size);
        if (ret == AICAM_OK && jpeg && jpeg_size > 0) {
            break;
        }
        ret = AICAM_ERROR;
        {
            device_t *jpeg_dev = device_find_pattern(JPEG_DEVICE_NAME, DEV_TYPE_VIDEO);
            jpegc_enc_debug_t dbg;
            memset(&dbg, 0, sizeof(dbg));
            if (jpeg_dev &&
                device_ioctl(jpeg_dev, JPEGC_CMD_GET_ENC_DEBUG,
                             (uint8_t *)&dbg, sizeof(dbg)) == AICAM_OK) {
                LOG_CORE_WARN("fd enc attempt %d: mode=%d in_paused=%d out_paused=%d "
                              "hw_end=%d sem=%lu cap=%lu out=%lu",
                              attempt, dbg.mode, dbg.input_paused, dbg.output_paused,
                              dbg.hw_end, (unsigned long)dbg.sem_count,
                              (unsigned long)dbg.out_capacity,
                              (unsigned long)dbg.out_size);
            }
        }
        osDelay(50);
    }
    if (ret != AICAM_OK) {
        static uint32_t enc_warn_tick = 0;
        uint32_t now_t = osKernelGetTickCount();
        if (now_t - enc_warn_tick > 5000U) {
            enc_warn_tick = now_t;
            LOG_CORE_WARN("Face report JPEG encode failed (%lux%lu, q%lu)",
                          (unsigned long)w, (unsigned long)h,
                          (unsigned long)cfg->jpeg_quality);
        }
        return ret;
    }

    /* The jpegc output buffer is device-owned: keep a heap copy so the send
     * is asynchronous, then recycle the device buffer. */
    uint8_t *copy = (uint8_t *)buffer_calloc(1, jpeg_size);
    if (!copy) {
        ai_jpeg_free_buffer(jpeg);
        return AICAM_ERROR_NO_MEMORY;
    }
    memcpy(copy, jpeg, jpeg_size);
    ai_jpeg_free_buffer(jpeg);

    *jpeg_out = copy;
    *jpeg_size_out = jpeg_size;
    return AICAM_OK;
}

/**
 * @brief Crop + encode + queue the reports armed by the AI thread
 * @details Runs on the camera thread where the PIPE1 RGB565 frame lives.
 * @param tracks Snapshot of confirmed tracks (already extrapolated boxes
 *               must be recomputed here for crop framing).
 */
static void fd_queue_reports(face_detection_ctx_t *ctx, const fd_track_t *tracks,
                             uint32_t count, uint8_t *fb, uint32_t fw, uint32_t fh,
                             uint32_t frame_now)
{
    const face_detection_config_t *cfg = &ctx->config;
    uint32_t conf_thr = cfg->report_confidence_threshold;

    for (uint32_t i = 0; i < count; i++) {
        const fd_track_t *track = &tracks[i];
        if (!track->need_report || track->det_conf * 100.0f < (float)conf_thr) {
            if (track->need_report &&
                track->det_conf * 100.0f < (float)conf_thr) {
                ctx->stats.reports_dropped++;
                {
                    static uint32_t last = 0;
                    uint32_t now = osKernelGetTickCount();
                    if (now - last > 3000U) {
                        last = now;
                        LOG_CORE_WARN("Face report dropped: det conf %lu < report thr %lu",
                                      (unsigned long)(track->det_conf * 100.0f),
                                      (unsigned long)conf_thr);
                    }
                }
            }
            continue;
        }

        int rx, ry, rw, rh;
        if (!fd_expanded_rect(cfg, track, frame_now, fw, fh, &rx, &ry, &rw, &rh)) {
            ctx->stats.reports_dropped++; /* crop too small */
            {
                static uint32_t last = 0;
                uint32_t now = osKernelGetTickCount();
                if (now - last > 3000U) {
                    last = now;
                    LOG_CORE_WARN("Face report dropped: crop %lux%lu below crop_min_size",
                                  (unsigned long)rw, (unsigned long)rh);
                }
            }
            continue;
        }

        fd_report_msg_t *msg = (fd_report_msg_t *)buffer_calloc(1, sizeof(fd_report_msg_t));
        if (!msg) {
            ctx->stats.reports_dropped++;
            continue;
        }
        /* Camera thread: fast crop only; the JPEG encode happens on the
         * worker so multi-face frames do not stall the frame path. */
        if (fd_crop_face(cfg, fb, fw, fh, rx, ry, rw, rh,
                         &msg->raw, &msg->raw_w, &msg->raw_h,
                         &msg->resized) != AICAM_OK) {
            ctx->stats.reports_dropped++;
            /* Back off instead of retrying (and failing) every frame */
            fd_track_t *live = NULL;
            for (int t = 0; t < FD_MAX_TRACKS; t++) {
                if (g_fd.tracks[t].active && g_fd.tracks[t].id == track->id) {
                    live = &g_fd.tracks[t];
                    break;
                }
            }
            if (live) {
                live->need_report = AICAM_FALSE;
                live->last_report_tick = frame_now;
                /* Do not leave a manual-trigger PENDING hanging forever */
                if (live->result_status == FD_RESULT_PENDING &&
                    !live->request_in_flight) {
                    live->result_status = FD_RESULT_NONE;
                }
            }
            buffer_free(msg);
            continue;
        }

        ctx->request_counter++;
        msg->track_id = track->id;
        msg->frame_id = g_fd.last_frame_id;
        msg->timestamp_ms = frame_now;
        msg->quality = cfg->jpeg_quality;
        msg->reason = track->report_reason;
        msg->det_conf = track->det_conf;
        snprintf(msg->request_id, sizeof(msg->request_id), "fd-%08lu",
                 (unsigned long)ctx->request_counter);
        /* Protocol boxes: original last box + expanded/clamped pixel rect */
        msg->bbox[0] = track->x;
        msg->bbox[1] = track->y;
        msg->bbox[2] = track->w;
        msg->bbox[3] = track->h;
        msg->ebbox[0] = (float)rx / (float)fw;
        msg->ebbox[1] = (float)ry / (float)fh;
        msg->ebbox[2] = (float)rw / (float)fw;
        msg->ebbox[3] = (float)rh / (float)fh;
        msg->nb_landmarks = track->nb_landmarks;
        memcpy(msg->landmarks, track->lm, sizeof(msg->landmarks));

        if (osMessageQueuePut(ctx->report_queue, &msg, 0, 0) != osOK) {
            buffer_free(msg->raw);
            buffer_free(msg);
            ctx->stats.reports_dropped++;
            LOG_CORE_WARN("Face report queue full, drop track %lu",
                          (unsigned long)track->id);
            continue;
        }
        ctx->stats.reports_queued++;
        LOG_CORE_INFO("Face report queued: track %lu crop %lux%lu",
                      (unsigned long)msg->track_id,
                      (unsigned long)msg->raw_w, (unsigned long)msg->raw_h);

        /* Arm the per-track request state on the shared copy */
        fd_track_t *live = NULL;
        for (int t = 0; t < FD_MAX_TRACKS; t++) {
            if (g_fd.tracks[t].active && g_fd.tracks[t].id == track->id) {
                live = &g_fd.tracks[t];
                break;
            }
        }
        if (live) {
            live->need_report = AICAM_FALSE;
            live->report_reason = 0;
            live->last_report_tick = frame_now;
            strncpy(live->request_id, msg->request_id, sizeof(live->request_id) - 1);
            live->request_id[sizeof(live->request_id) - 1] = '\0';
            live->request_tick = frame_now;
            live->request_in_flight = AICAM_TRUE;
            if (live->result_status == FD_RESULT_NONE ||
                live->result_status == FD_RESULT_TIMEOUT ||
                live->result_status == FD_RESULT_ERROR) {
                live->result_status = FD_RESULT_PENDING;
            }
        }
    }
}

void face_detection_draw_overlay(uint8_t *fb, uint32_t width, uint32_t height)
{
    fd_ensure_init();
    if (!g_fd.initialized || !fb || width == 0 || height == 0) {
        return;
    }
    if (!g_fd.config.enabled) {
        return;
    }
    if (!fd_lazy_draw_init(&g_fd)) {
        return;
    }

    /* Snapshot shared state under the mutex, draw outside it */
    face_detection_config_t cfg;
    fd_track_t tracks[FD_MAX_TRACKS];
    uint32_t track_count = 0;
    uint32_t now = osKernelGetTickCount();
    if (osMutexAcquire(g_fd.mutex, 20) == osOK) {
        memcpy(&cfg, &g_fd.config, sizeof(cfg));
        for (int t = 0; t < FD_MAX_TRACKS; t++) {
            if (g_fd.tracks[t].active && g_fd.tracks[t].confirmed) {
                tracks[track_count++] = g_fd.tracks[t];
            }
        }
        /* Staleness guard: AI stalled -> stop drawing its last state */
        if (track_count > 0 && g_fd.last_result_tick != 0 &&
            (now - g_fd.last_result_tick) > FD_STALE_MS) {
            track_count = 0;
        }
        osMutexRelease(g_fd.mutex);
    } else {
        return;
    }

    {
        static aicam_bool_t overlay_logged = AICAM_FALSE;
        if (!overlay_logged) {
            overlay_logged = AICAM_TRUE;
            LOG_CORE_INFO("Face overlay active: %lux%lu, tracks=%lu, report=%d",
                          (unsigned long)width, (unsigned long)height,
                          (unsigned long)track_count, cfg.report_enabled);
        }
    }

    /* 1) Report crops FIRST: the recognition backend must receive the
     * unblurred face, so the crop is taken before the mosaic modifies
     * the frame that continues into the encoded stream. */
    if (cfg.report_enabled) {
        if (!g_fd.report_queue || !g_fd.worker_running || !g_fd.worker_thread) {
            static uint32_t gate_last = 0;
            if (now - gate_last > 10000U) {
                gate_last = now;
                LOG_CORE_ERROR("Face report path down: queue=%p worker=%d -> resurrect",
                               (void *)g_fd.report_queue, g_fd.worker_running ? 1 : 0);
                fd_worker_resurrect();
            }
        }
        if (g_fd.report_queue && g_fd.worker_running && g_fd.worker_thread &&
            osMutexAcquire(g_fd.mutex, 20) == osOK) {
            fd_queue_reports(&g_fd, tracks, track_count, fb, width, height, now);
            osMutexRelease(g_fd.mutex);
        }
    }

    /* 2) Face mosaic (stream copy only) so boxes/labels stay on top */
    if (cfg.blur_enabled && track_count > 0) {
        for (uint32_t i = 0; i < track_count; i++) {
            const fd_track_t *track = &tracks[i];
            float cx, cy;
            fd_track_center_now(track, now, cfg.extrapolation_enabled, &cx, &cy);
            float expand = 1.0f + (float)cfg.blur_expand_ratio / 100.0f;
            float fw_n = track->w * expand;
            float fh_n = track->h * expand;
            int x0 = (int)((cx - fw_n * 0.5f) * (float)width);
            int y0 = (int)((cy - fh_n * 0.5f) * (float)height);
            int x1 = (int)((cx + fw_n * 0.5f) * (float)width);
            int y1 = (int)((cy + fh_n * 0.5f) * (float)height);
            fd_mosaic_region_rgb565((uint16_t *)fb, width, height,
                                    x0, y0, x1, y1, cfg.mosaic_block);
        }
    }

    /* 3) Boxes + labels + landmarks */
    for (uint32_t i = 0; i < track_count; i++) {
        const fd_track_t *track = &tracks[i];
        float cx, cy;
        fd_track_center_now(track, now, cfg.extrapolation_enabled, &cx, &cy);

        int bx = (int)((cx - track->w * 0.5f) * (float)width);
        int by = (int)((cy - track->h * 0.5f) * (float)height);
        int bw = (int)(track->w * (float)width);
        int bh = (int)(track->h * (float)height);
        if (bw < 4 || bh < 4) {
            continue;
        }

        uint32_t box_color = COLOR_WHITE;
        const char *result_tag = "...";
        char name_line[MAX_LINE_CHAR];
        switch (track->result_status) {
            case FD_RESULT_OK:
                box_color = COLOR_GREEN;
                {
                    char ascii[FD_RESULT_NAME_MAX];
                    fd_ascii_name(track->result_name, ascii, sizeof(ascii));
                    if (ascii[0] != '\0') {
                        snprintf(name_line, sizeof(name_line), "%s %d%%", ascii,
                                 (int)(track->result_conf * 100.0f + 0.5f));
                    } else {
                        snprintf(name_line, sizeof(name_line), "OK %d%%",
                                 (int)(track->result_conf * 100.0f + 0.5f));
                    }
                }
                break;
            case FD_RESULT_UNKNOWN: box_color = COLOR_YELLOW; result_tag = "unknown";
                snprintf(name_line, sizeof(name_line), "%s", result_tag); break;
            case FD_RESULT_TIMEOUT: box_color = COLOR_RED; result_tag = "timeout";
                snprintf(name_line, sizeof(name_line), "%s", result_tag); break;
            case FD_RESULT_ERROR:   box_color = COLOR_RED; result_tag = "error";
                snprintf(name_line, sizeof(name_line), "%s", result_tag); break;
            default: /* NONE / PENDING */
                box_color = (track->request_in_flight) ? COLOR_CYAN : COLOR_WHITE;
                snprintf(name_line, sizeof(name_line), "%s", result_tag);
                break;
        }

        if (cfg.show_box) {
            fd_draw_rect_hw(&g_fd, fb, width, height,
                            bx, by, bw, bh, 2, box_color);
        }

        /* Landmarks (model-dependent) */
        if (cfg.show_landmarks && track->nb_landmarks > 0) {
            for (uint32_t k = 0; k < track->nb_landmarks; k++) {
                int lx = (int)(track->lm[k][0] * (float)width);
                int ly = (int)(track->lm[k][1] * (float)height);
                fd_blend_rect_hw(&g_fd, fb, width, height, lx - 1, ly - 1, 3, 3,
                                 COLOR_MAGENTA, 200);
            }
        }

        /* Label: ONE line — "#3 92% Alice 93%" (parts per show switches) */
        char label[MAX_LINE_CHAR] = {0};
        size_t label_len = 0;
        if (cfg.show_id) {
            label_len += (size_t)snprintf(label + label_len, sizeof(label) - label_len,
                                          "#%lu", (unsigned long)track->id);
        }
        if (cfg.show_confidence) {
            label_len += (size_t)snprintf(label + label_len, sizeof(label) - label_len,
                                          "%s%d%%", label_len ? " " : "",
                                          (int)(track->det_conf * 100.0f + 0.5f));
        }
        if (cfg.show_recognition && name_line[0] != '\0') {
            label_len += (size_t)snprintf(label + label_len, sizeof(label) - label_len,
                                          "%s%s", label_len ? " " : "", name_line);
        }

        if (label[0] != '\0' && label_len < sizeof(label)) {
            uint32_t text_h = g_fd.font16.height;
            uint32_t block_h = text_h + 4;
            uint32_t block_w = fd_font_width(&g_fd, label);

            /* Edge-aware placement: prefer above the box, then inside top,
             * then below, then clamped to the frame (doc section 6). */
            int lx = bx;
            if (lx < 2) lx = 2;
            if (lx + (int)block_w > (int)width - 2) {
                lx = (int)width - 2 - (int)block_w;
            }
            if (lx < 2) lx = 2;

            int ly = by - (int)block_h - 4;                 /* above */
            if (ly < 2) {
                ly = by + 4;                                /* inside top */
            }
            if (ly + (int)block_h > by + bh - 2 && by + bh + (int)block_h + 4 < (int)height - 2) {
                ly = by + bh + 4;                           /* below */
            } else if (ly + (int)block_h > (int)height - 2) {
                ly = (int)height - 2 - (int)block_h;        /* clamped inside */
            }

            fd_blend_rect_hw(&g_fd, fb, width, height, lx - 4, ly - 2,
                             (int)block_w + 8, (int)block_h, COLOR_BLACK, 150);
            fd_draw_text_hw(&g_fd, fb, width, height, lx, ly + 2, label);
        }
    }
}

/* ==================== Report Worker ==================== */

static int fd_base64_encode(const uint8_t *in, uint32_t in_len, char *out, uint32_t out_size)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t o = 0;
    for (uint32_t i = 0; i < in_len; i += 3) {
        if (o + 5 > out_size) {
            return -1;
        }
        uint32_t a = in[i];
        uint32_t b = (i + 1 < in_len) ? in[i + 1] : 0;
        uint32_t c = (i + 2 < in_len) ? in[i + 2] : 0;
        uint32_t trip = (a << 16) | (b << 8) | c;
        out[o++] = table[(trip >> 18) & 0x3F];
        out[o++] = table[(trip >> 12) & 0x3F];
        out[o++] = (i + 1 < in_len) ? table[(trip >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < in_len) ? table[trip & 0x3F] : '=';
    }
    out[o] = '\0';
    return (int)o;
}

/**
 * @brief Build the protocol face_report JSON (doc section 5.2)
 * @param with_image Include image_data/encoding (MQTT flavor)
 * @return Heap JSON string (cJSON_free by caller) or NULL
 */
static char *fd_build_report_json(const fd_report_msg_t *msg, aicam_bool_t with_image)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "type", "face_report");
    cJSON_AddStringToObject(root, "request_id", msg->request_id);
    cJSON_AddNumberToObject(root, "timestamp_ms", (double)msg->timestamp_ms);
    cJSON_AddNumberToObject(root, "frame_id", (double)msg->frame_id);

    device_info_config_t dev_info;
    if (device_service_get_cached_info(&dev_info) == AICAM_OK) {
        cJSON *dev = cJSON_CreateObject();
        cJSON_AddStringToObject(dev, "device_name", dev_info.device_name);
        cJSON_AddStringToObject(dev, "serial_number", dev_info.serial_number);
        cJSON_AddItemToObject(root, "device_info", dev);
    }

    cJSON *face = cJSON_CreateObject();
    cJSON_AddNumberToObject(face, "track_id", (double)msg->track_id);
    cJSON_AddStringToObject(face, "reason", fd_reason_str(msg->reason));
    cJSON_AddNumberToObject(face, "detection_confidence",
                            (double)((int)(msg->det_conf * 1000.0f + 0.5f)) / 1000.0);
    cJSON *bbox = cJSON_CreateDoubleArray(
        (const double[]){(double)msg->bbox[0], (double)msg->bbox[1],
                         (double)msg->bbox[2], (double)msg->bbox[3]}, 4);
    cJSON_AddItemToObject(face, "bbox", bbox);
    cJSON *ebbox = cJSON_CreateDoubleArray(
        (const double[]){(double)msg->ebbox[0], (double)msg->ebbox[1],
                         (double)msg->ebbox[2], (double)msg->ebbox[3]}, 4);
    cJSON_AddItemToObject(face, "expanded_bbox", ebbox);
    if (msg->nb_landmarks > 0) {
        cJSON *lms = cJSON_CreateArray();
        for (uint32_t k = 0; k < msg->nb_landmarks; k++) {
            cJSON_AddItemToArray(lms, cJSON_CreateDoubleArray(
                (const double[]){(double)msg->landmarks[k][0],
                                 (double)msg->landmarks[k][1]}, 2));
        }
        cJSON_AddItemToObject(face, "landmarks", lms);
    } else {
        cJSON_AddNullToObject(face, "landmarks");
    }
    cJSON_AddItemToObject(root, "face", face);

    cJSON *image = cJSON_CreateObject();
    cJSON_AddStringToObject(image, "format", "jpeg");
    cJSON_AddNumberToObject(image, "width", (double)msg->img_w);
    cJSON_AddNumberToObject(image, "height", (double)msg->img_h);
    cJSON_AddNumberToObject(image, "size", (double)msg->jpeg_size);
    cJSON_AddNumberToObject(image, "quality", (double)msg->quality);
    cJSON_AddBoolToObject(image, "resized", msg->resized == AICAM_TRUE);
    cJSON_AddItemToObject(root, "image", image);

    char *json = NULL;
    if (with_image) {
        uint32_t b64_size = ((msg->jpeg_size + 2) / 3) * 4 + 64;
        char *b64 = (char *)buffer_calloc(1, b64_size);
        if (b64) {
            if (fd_base64_encode(msg->jpeg, msg->jpeg_size, b64, b64_size - 1) > 0) {
                char *data_uri = (char *)buffer_calloc(1, b64_size + 32);
                if (data_uri) {
                    snprintf(data_uri, b64_size + 32, "data:image/jpeg;base64,%s", b64);
                    cJSON_AddStringToObject(root, "image_data", data_uri);
                    cJSON_AddStringToObject(root, "encoding", "base64");
                    buffer_free(data_uri);
                    json = cJSON_PrintUnformatted(root);
                }
            }
            buffer_free(b64);
        }
    } else {
        json = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);
    return json;
}

/* Archive the reported face crop into the capture-record store (option A):
 * the record page shows it with trigger "face" even though the report path
 * itself never touches the capture pipeline. The coordinator copies the
 * bytes, so the caller keeps ownership of msg->jpeg. */
static void fd_archive_record(const fd_report_msg_t *msg)
{
    mqtt_image_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    snprintf(meta.image_id, sizeof(meta.image_id), "face_%s", msg->request_id);
    meta.timestamp = (uint64_t)msg->timestamp_ms;
    meta.format = MQTT_IMAGE_FORMAT_JPEG;
    meta.width = msg->img_w;
    meta.height = msg->img_h;
    meta.size = msg->jpeg_size;
    meta.quality = (uint8_t)msg->quality;
    meta.trigger_type = AICAM_CAPTURE_TRIGGER_FACE;

    /* Local-only: visible in the records page, never re-uploaded by the
     * capture pipeline (the recognition report already went its own way). */
    aicam_result_t rc = upload_coordinator_enqueue_capture_local(
        msg->jpeg, msg->jpeg_size, &meta, AICAM_CAPTURE_TRIGGER_FACE);
    if (rc != AICAM_OK) {
        LOG_SVC_WARN("Face record archive failed (%d)", rc);
    }
}

static void fd_send_report(fd_report_msg_t *msg)
{
    face_detection_config_t cfg;
    if (face_detection_get_config(&cfg) != AICAM_OK) {
        face_detection_get_default_config(&cfg);
    }

    if (cfg.report_channel == FD_REPORT_CHANNEL_MQTT) {
        char *json = fd_build_report_json(msg, AICAM_TRUE);
        if (!json) {
            g_fd.stats.reports_failed++;
            return;
        }
        mqtt_service_topic_config_t mqtt_cfg;
        char topic[128] = {0};
        if (mqtt_service_get_topic_config(&mqtt_cfg) == AICAM_OK) {
            strncpy(topic, mqtt_cfg.data_report_topic, sizeof(topic) - 1);
        }
        int rc = mqtt_service_publish_json(topic[0] ? topic : NULL, json, 1, 0);
        cJSON_free(json);
        LOG_SVC_INFO("Face report MQTT send: track %lu rc=%d",
                     (unsigned long)msg->track_id, rc);
        if (rc < 0) {
            g_fd.stats.reports_failed++;
            LOG_SVC_WARN("Face report MQTT publish failed (%d)", rc);
        } else {
            g_fd.stats.reports_sent++;
            fd_archive_record(msg);
        }
        /* Async result: arrives on the control/receive topic (sniffed by
         * face_detection_try_handle_mqtt) — nothing else to do here. */
    } else {
        char *json = fd_build_report_json(msg, AICAM_FALSE);
        if (!json) {
            g_fd.stats.reports_failed++;
            return;
        }
        char response[512] = {0};
        aicam_result_t rc = webhook_service_push_json_image(
            json, msg->jpeg, msg->jpeg_size, response, sizeof(response));
        cJSON_free(json);
        LOG_SVC_INFO("Face report webhook send: track %lu rc=%d resp=%s",
                     (unsigned long)msg->track_id, rc,
                     rc == AICAM_OK ? response : "");
        if (rc != AICAM_OK) {
            g_fd.stats.reports_failed++;
            LOG_SVC_WARN("Face report webhook push failed (%d)", rc);
        } else {
            g_fd.stats.reports_sent++;
            fd_archive_record(msg);
            if (response[0] != '\0') {
                face_detection_apply_result_json(response, (uint32_t)strlen(response));
            }
        }
    }
}

/* Re-create the report worker/queue if it somehow died (logged, throttled
 * by the caller). Runs on the camera thread. */
static void fd_worker_resurrect(void)
{
    if (g_fd.report_queue == NULL) {
        g_fd.report_queue = osMessageQueueNew(FD_REPORT_QUEUE_DEPTH,
                                              sizeof(fd_report_msg_t *), NULL);
        LOG_CORE_ERROR("Face report: queue re-created (%p)", (void *)g_fd.report_queue);
    }
    if (g_fd.report_queue && g_fd.worker_thread == NULL) {
        osThreadAttr_t attr = {
            .name = "fd_report",
            .stack_mem = fd_worker_stack,
            .stack_size = sizeof(fd_worker_stack),
            .priority = osPriorityBelowNormal,
        };
        g_fd.worker_running = AICAM_TRUE;
        g_fd.worker_thread = osThreadNew(fd_report_worker, NULL, &attr);
        if (!g_fd.worker_thread) {
            g_fd.worker_running = AICAM_FALSE;
            LOG_CORE_ERROR("Face report: worker re-creation FAILED");
        } else {
            LOG_CORE_ERROR("Face report: worker re-created");
        }
    }
}

static void fd_report_worker(void *arg)
{
    (void)arg;
    LOG_CORE_INFO("Face report worker started (stack %lu)",
                  (unsigned long)FD_WORKER_STACK_SIZE);
    for (;;) {
        if (!g_fd.worker_running) {
            break;
        }
        fd_report_msg_t *msg = NULL;
        osStatus_t st = osMessageQueueGet(g_fd.report_queue, &msg, NULL, 500);
        if (st == osErrorResource || st == osErrorParameter) {
            /* Queue destroyed or invalid — do not spin */
            osDelay(100);
            continue;
        }
        if (st != osOK || msg == NULL) {
            continue; /* timeout / deinit wake -> re-check worker_running */
        }
        if (!msg->raw) {
            buffer_free(msg); /* deinit wake (NULL) */
            continue;
        }
        face_detection_config_t wcfg;
        if (face_detection_get_config(&wcfg) != AICAM_OK) {
            face_detection_get_default_config(&wcfg);
        }
        if (fd_encode_raw(&wcfg, msg->raw, msg->raw_w, msg->raw_h,
                          &msg->jpeg, &msg->jpeg_size) != AICAM_OK) {
            g_fd.stats.reports_dropped++;
            buffer_free(msg->raw);
            buffer_free(msg);
            continue;
        }
        msg->img_w = msg->raw_w;
        msg->img_h = msg->raw_h;
        buffer_free(msg->raw);
        msg->raw = NULL;
        fd_send_report(msg);
        buffer_free(msg->jpeg);
        buffer_free(msg);
    }
    g_fd.worker_thread = NULL; /* allow resurrect unless deinit asked */
    LOG_CORE_WARN("Face report worker exiting (deinit requested)");
}

/* ==================== Result Application ==================== */

aicam_bool_t face_detection_apply_result_json(const char *json, uint32_t len)
{
    if (!json || len == 0) {
        return AICAM_FALSE;
    }
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return AICAM_FALSE;
    }
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || !type->valuestring ||
        strcmp(type->valuestring, "face_recognition_result") != 0) {
        cJSON_Delete(root);
        return AICAM_FALSE;
    }

    cJSON *jreq = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    cJSON *jstatus = cJSON_GetObjectItemCaseSensitive(root, "status");
    cJSON *jname = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *jconf = cJSON_GetObjectItemCaseSensitive(root, "confidence");
    if (!cJSON_IsString(jreq) || !jreq->valuestring || !cJSON_IsString(jstatus)) {
        cJSON_Delete(root);
        return AICAM_FALSE;
    }

    fd_result_status_t status = FD_RESULT_ERROR;
    const char *s = jstatus->valuestring;
    if (strcmp(s, "ok") == 0) {
        status = FD_RESULT_OK;
    } else if (strcmp(s, "unknown") == 0 || strcmp(s, "no_face") == 0) {
        status = FD_RESULT_UNKNOWN;
    }

    aicam_bool_t applied = AICAM_FALSE;
    uint32_t now = osKernelGetTickCount();

    /* One-shot verification replies bypass tracks */
    if (strncmp(jreq->valuestring, "fdv-", 4) == 0) {
        if (osMutexAcquire(g_fd.mutex, 50) == osOK) {
            if (g_fd.verify.done == AICAM_FALSE &&
                strcmp(g_fd.verify.request_id, jreq->valuestring) == 0) {
                const char *sv = jstatus->valuestring;
                g_fd.verify.status = (strcmp(sv, "ok") == 0) ? 0
                                  : (strcmp(sv, "unknown") == 0 ||
                                     strcmp(sv, "no_face") == 0) ? 1 : 3;
                if (g_fd.verify.status == 0) {
                    memset(g_fd.verify.name, 0, sizeof(g_fd.verify.name));
                    if (cJSON_IsString(jname) && jname->valuestring) {
                        strncpy(g_fd.verify.name, jname->valuestring,
                                sizeof(g_fd.verify.name) - 1);
                    }
                    g_fd.verify.conf =
                        cJSON_IsNumber(jconf) ? (float)jconf->valuedouble : 0.0f;
                }
                g_fd.verify.done = AICAM_TRUE;
            }
            osMutexRelease(g_fd.mutex);
        }
        cJSON_Delete(root);
        return AICAM_TRUE;
    }

    if (osMutexAcquire(g_fd.mutex, 50) == osOK) {
        for (int t = 0; t < FD_MAX_TRACKS; t++) {
            fd_track_t *track = &g_fd.tracks[t];
            if (!track->active || !track->request_in_flight) {
                continue;
            }
            if (strcmp(track->request_id, jreq->valuestring) != 0) {
                continue;
            }
            /* Idempotency: only the first terminal result is applied */
            track->request_in_flight = AICAM_FALSE;
            track->result_status = status;
            track->result_tick = now;
            track->result_conf =
                cJSON_IsNumber(jconf) ? (float)jconf->valuedouble : 0.0f;
            memset(track->result_name, 0, sizeof(track->result_name));
            if (cJSON_IsString(jname) && jname->valuestring) {
                strncpy(track->result_name, jname->valuestring,
                        sizeof(track->result_name) - 1);
            }
            if (status == FD_RESULT_OK) {
                g_fd.stats.results_ok++;
            } else if (status == FD_RESULT_UNKNOWN) {
                g_fd.stats.results_unknown++;
            }
            applied = AICAM_TRUE;
            LOG_CORE_INFO("Face result: track %lu -> %s (%s)",
                          (unsigned long)track->id, s, track->result_name);
            break;
        }
        osMutexRelease(g_fd.mutex);
    }

    cJSON_Delete(root);
    return AICAM_TRUE;
}

aicam_bool_t face_detection_try_handle_mqtt(const char *json, uint32_t len)
{
    if (!g_fd.initialized || !json || len == 0) {
        return AICAM_FALSE;
    }
    /* Cheap pre-check before a full parse */
    if (!strstr(json, "face_recognition_result")) {
        return AICAM_FALSE;
    }
    return face_detection_apply_result_json(json, len);
}

/* ==================== Lifecycle & Config API ==================== */

void face_detection_get_default_config(face_detection_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(face_detection_config_t));
    config->enabled = AICAM_FALSE;
    config->detection_conf_threshold = 50;    /* matches the model default */
    config->min_face_area = 1500;             /* ~37px square at 720p */
    config->track_iou_threshold = 30;
    config->track_max_missed = 15;
    config->track_confirm_frames = 3;
    config->face_hold_ms = FD_HOLD_MS_DEFAULT;
    config->extrapolation_enabled = AICAM_FALSE; /* fast models: not needed */
    config->blur_enabled = AICAM_FALSE;
    config->mosaic_block = 16;
    config->blur_expand_ratio = 10;
    config->crop_expand_ratio = 20;
    config->crop_min_size = 32;
    config->report_fixed_size = 0;            /* native crop size */
    config->jpeg_quality = 80;
    config->report_enabled = AICAM_FALSE;
    config->report_channel = FD_REPORT_CHANNEL_WEBHOOK;
    config->report_confidence_threshold = 60;
    config->report_periodic_enabled = AICAM_FALSE;
    config->report_cooldown_ms = 10000;
    config->result_timeout_ms = 5000;
    config->result_expire_ms = 30000;
    config->show_box = AICAM_TRUE;
    config->show_id = AICAM_TRUE;
    config->show_confidence = AICAM_TRUE;
    config->show_landmarks = AICAM_TRUE;
    config->show_recognition = AICAM_TRUE;
}

aicam_result_t face_detection_init(void)
{
    fd_ensure_init();
    return g_fd.initialized ? AICAM_OK : AICAM_ERROR;
}

aicam_result_t face_detection_deinit(void)
{
    if (!g_fd.initialized) {
        return AICAM_OK;
    }
    g_fd.initialized = AICAM_FALSE;

    if (g_fd.worker_running) {
        g_fd.worker_running = AICAM_FALSE;
        /* Drain-wake: the queue get blocks forever; posting an empty msg
         * lets the worker observe the flag and exit. */
        fd_report_msg_t *empty = NULL;
        osMessageQueuePut(g_fd.report_queue, &empty, 0, 0);
    }

    if (g_fd.font16.data) {
        buffer_free(g_fd.font16.data);
        g_fd.font16.data = NULL;
    }
    g_fd.font_ready = AICAM_FALSE;
    g_fd.draw_dev = NULL;

    return AICAM_OK;
}

aicam_bool_t face_detection_is_initialized(void)
{
    return g_fd.initialized;
}

aicam_result_t face_detection_get_config(face_detection_config_t *config)
{
    if (!config) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!g_fd.initialized) {
        face_detection_get_default_config(config);
        return AICAM_OK;
    }
    if (osMutexAcquire(g_fd.mutex, osWaitForever) == osOK) {
        memcpy(config, &g_fd.config, sizeof(face_detection_config_t));
        osMutexRelease(g_fd.mutex);
        return AICAM_OK;
    }
    return AICAM_ERROR;
}

aicam_result_t face_detection_set_config(const face_detection_config_t *config)
{
    if (!config || !g_fd.initialized) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (osMutexAcquire(g_fd.mutex, osWaitForever) == osOK) {
        memcpy(&g_fd.config, config, sizeof(face_detection_config_t));
        g_fd.stats.enabled = config->enabled;
        if (!config->enabled) {
            /* Leave no stale overlay state behind */
            memset(g_fd.tracks, 0, sizeof(g_fd.tracks));
        }
        osMutexRelease(g_fd.mutex);
        return AICAM_OK;
    }
    return AICAM_ERROR;
}

aicam_result_t face_detection_get_stats(face_detection_stats_t *stats)
{
    if (!stats) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    fd_ensure_init();
    if (!g_fd.initialized) {
        memset(stats, 0, sizeof(face_detection_stats_t));
        return AICAM_OK;
    }
    if (osMutexAcquire(g_fd.mutex, 20) == osOK) {
        memcpy(stats, &g_fd.stats, sizeof(face_detection_stats_t));
        stats->enabled = g_fd.config.enabled;
        osMutexRelease(g_fd.mutex);
        return AICAM_OK;
    }
    return AICAM_ERROR_TIMEOUT;
}

aicam_result_t face_detection_get_results(face_detection_results_t *results)
{
    if (!results) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    memset(results, 0, sizeof(face_detection_results_t));
    fd_ensure_init();
    if (!g_fd.initialized) {
        return AICAM_OK;
    }
    if (osMutexAcquire(g_fd.mutex, 20) == osOK) {
        for (int t = 0; t < FD_MAX_TRACKS && results->count < FD_MAX_TRACKS; t++) {
            const fd_track_t *track = &g_fd.tracks[t];
            if (!track->active || !track->confirmed) {
                continue;
            }
            face_detection_track_info_t *info = &results->tracks[results->count++];
            info->track_id = track->id;
            info->x = track->x;
            info->y = track->y;
            info->w = track->w;
            info->h = track->h;
            info->detection_confidence = track->det_conf;
            info->nb_landmarks = track->nb_landmarks;
            memcpy(info->landmarks, track->lm, sizeof(info->landmarks));
            info->result_status = track->result_status;
            strncpy(info->result_name, track->result_name,
                    sizeof(info->result_name) - 1);
            info->result_confidence = track->result_conf;
        }
        osMutexRelease(g_fd.mutex);
        return AICAM_OK;
    }
    return AICAM_ERROR_TIMEOUT;
}

aicam_result_t face_detection_reset(void)
{
    if (!g_fd.initialized) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }
    if (osMutexAcquire(g_fd.mutex, osWaitForever) == osOK) {
        memset(&g_fd.stats, 0, sizeof(g_fd.stats));
        memset(g_fd.tracks, 0, sizeof(g_fd.tracks));
        osMutexRelease(g_fd.mutex);
        LOG_CORE_INFO("Face detection statistics reset");
        return AICAM_OK;
    }
    return AICAM_ERROR;
}


/* ==================== One-shot Image Verification ==================== */

#define FD_VERIFY_STATUS_OK      0
#define FD_VERIFY_STATUS_UNKNOWN 1
#define FD_VERIFY_STATUS_ERROR   3

/**
 * @brief Push a user-uploaded face JPEG through the recognition channel and
 *        return the verdict (verification entry point for the web page).
 * @param jpeg User image bytes (JPEG)
 * @param name_out/conf_out Caller buffers for the verdict
 * @param status_out FD_VERIFY_STATUS_*
 */
aicam_result_t face_detection_verify_image(const uint8_t *jpeg, uint32_t size,
                                           char *name_out, uint32_t name_size,
                                           float *conf_out, uint32_t *status_out,
                                           uint32_t timeout_ms)
{
    if (!jpeg || !size || !name_out || !conf_out || !status_out) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    fd_ensure_init();
    if (!g_fd.initialized) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }

    face_detection_config_t cfg;
    if (face_detection_get_config(&cfg) != AICAM_OK) {
        face_detection_get_default_config(&cfg);
    }

    char request_id[24] = {0};
    uint32_t ts = osKernelGetTickCount();
    if (osMutexAcquire(g_fd.mutex, osWaitForever) == osOK) {
        g_fd.request_counter++;
        snprintf(request_id, sizeof(request_id), "fdv-%08lu",
                 (unsigned long)g_fd.request_counter);
        memset(&g_fd.verify, 0, sizeof(g_fd.verify));
        strncpy(g_fd.verify.request_id, request_id,
                sizeof(g_fd.verify.request_id) - 1);
        g_fd.verify.done = AICAM_FALSE;
        osMutexRelease(g_fd.mutex);
    } else {
        return AICAM_ERROR;
    }

    if (cfg.report_channel == FD_REPORT_CHANNEL_WEBHOOK) {
        /* Build the report envelope (metadata part) directly */
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            return AICAM_ERROR_NO_MEMORY;
        }
        cJSON_AddStringToObject(root, "type", "face_report");
        cJSON_AddStringToObject(root, "request_id", request_id);
        cJSON_AddNumberToObject(root, "timestamp_ms", (double)ts);
        cJSON *face = cJSON_CreateObject();
        cJSON_AddNumberToObject(face, "track_id", 0);
        cJSON_AddStringToObject(face, "reason", "verification");
        cJSON_AddItemToObject(root, "face", face);
        cJSON *image = cJSON_CreateObject();
        cJSON_AddStringToObject(image, "format", "jpeg");
        cJSON_AddNumberToObject(image, "size", (double)size);
        cJSON_AddNumberToObject(image, "quality", 0);
        cJSON_AddBoolToObject(image, "resized", AICAM_FALSE);
        cJSON_AddItemToObject(root, "image", image);
        char *meta_json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!meta_json) {
            return AICAM_ERROR_NO_MEMORY;
        }

        char response[512] = {0};
        aicam_result_t rc = webhook_service_push_json_image(
            meta_json, jpeg, size, response, sizeof(response));
        cJSON_free(meta_json);
        if (rc != AICAM_OK) {
            *status_out = FD_VERIFY_STATUS_ERROR;
            name_out[0] = '\0';
            *conf_out = 0.0f;
            LOG_SVC_WARN("Face verify: webhook push failed (%d)", rc);
            return AICAM_OK;
        }
        /* Reuse the shared result parser via the verify slot */
        if (response[0] != '\0') {
            (void)face_detection_apply_result_json(response,
                                                   (uint32_t)strlen(response));
        }
        if (g_fd.verify.done) {
            const char *sv = (g_fd.verify.status == 0) ? "ok" : "other";
            (void)sv;
            *status_out = g_fd.verify.status;
            strncpy(name_out, g_fd.verify.name, name_size - 1);
            name_out[name_size - 1] = '\0';
            *conf_out = g_fd.verify.conf;
        } else {
            *status_out = FD_VERIFY_STATUS_ERROR;
            name_out[0] = '\0';
            *conf_out = 0.0f;
        }
        return AICAM_OK;
    }

    /* MQTT: publish the report, then wait for the fdv reply on the
     * control topic (routed into the verify slot). */
    {
        cJSON *root = cJSON_CreateObject();
        uint32_t b64_size = ((size + 2) / 3) * 4 + 64;
        char *b64 = (char *)buffer_calloc(1, b64_size);
        char *data_uri = b64 ? (char *)buffer_calloc(1, b64_size + 32) : NULL;
        char *json = NULL;
        if (root && b64 && data_uri &&
            fd_base64_encode(jpeg, size, b64, b64_size - 1) > 0) {
            cJSON_AddStringToObject(root, "type", "face_report");
            cJSON_AddStringToObject(root, "request_id", request_id);
            cJSON_AddNumberToObject(root, "timestamp_ms", (double)ts);
            cJSON *face = cJSON_CreateObject();
            cJSON_AddNumberToObject(face, "track_id", 0);
            cJSON_AddStringToObject(face, "reason", "verification");
            cJSON_AddItemToObject(root, "face", face);
            cJSON *image = cJSON_CreateObject();
            cJSON_AddStringToObject(image, "format", "jpeg");
            cJSON_AddNumberToObject(image, "size", (double)size);
            cJSON_AddItemToObject(root, "image", image);
            snprintf(data_uri, b64_size + 32, "data:image/jpeg;base64,%s", b64);
            cJSON_AddStringToObject(root, "image_data", data_uri);
            cJSON_AddStringToObject(root, "encoding", "base64");
            json = cJSON_PrintUnformatted(root);
        }
        if (root) cJSON_Delete(root);
        if (b64) buffer_free(b64);
        if (data_uri) buffer_free(data_uri);
        if (!json) {
            return AICAM_ERROR_NO_MEMORY;
        }

        mqtt_service_topic_config_t mqtt_cfg;
        char topic[128] = {0};
        if (mqtt_service_get_topic_config(&mqtt_cfg) == AICAM_OK) {
            strncpy(topic, mqtt_cfg.data_report_topic, sizeof(topic) - 1);
        }
        int rc = mqtt_service_publish_json(topic[0] ? topic : NULL, json, 1, 0);
        cJSON_free(json);
        if (rc < 0) {
            *status_out = FD_VERIFY_STATUS_ERROR;
            name_out[0] = '\0';
            *conf_out = 0.0f;
            LOG_SVC_WARN("Face verify: MQTT publish failed (%d)", rc);
            return AICAM_OK;
        }

        for (uint32_t waited = 0; waited < timeout_ms; waited += 50) {
            if (g_fd.verify.done) {
                break;
            }
            osDelay(50);
        }
        if (g_fd.verify.done) {
            *status_out = g_fd.verify.status;
            strncpy(name_out, g_fd.verify.name, name_size - 1);
            name_out[name_size - 1] = '\0';
            *conf_out = g_fd.verify.conf;
        } else {
            *status_out = FD_VERIFY_STATUS_ERROR; /* timed out */
            name_out[0] = '\0';
            *conf_out = 0.0f;
        }
        return AICAM_OK;
    }
}

uint32_t face_detection_trigger_report(uint32_t track_id)
{
    uint32_t armed = 0;
    fd_ensure_init();
    if (!g_fd.initialized) {
        return 0;
    }
    if (osMutexAcquire(g_fd.mutex, 50) == osOK) {
        if (g_fd.config.report_enabled) {
            for (int t = 0; t < FD_MAX_TRACKS; t++) {
                fd_track_t *track = &g_fd.tracks[t];
                if (!track->active || !track->confirmed) {
                    continue;
                }
                if (track_id != 0 && track->id != track_id) {
                    continue;
                }
                track->need_report = AICAM_TRUE;
                track->report_reason = FD_REASON_MANUAL;
                /* Manual re-report: drop the cached recognition so the OSD
                 * goes back to "recognizing" and the fresh result replaces
                 * it when it arrives. */
                track->result_status = FD_RESULT_PENDING;
                track->request_tick = osKernelGetTickCount();
                track->result_name[0] = (char)0;
                track->result_conf = 0.0f;
                track->request_in_flight = AICAM_FALSE; /* re-linked on queue */
                armed++;
            }
        }
        osMutexRelease(g_fd.mutex);
    }
    LOG_CORE_INFO("Face trigger: armed %lu (report_enabled=%d)",
                  (unsigned long)armed, g_fd.config.report_enabled);
    return armed;
}
