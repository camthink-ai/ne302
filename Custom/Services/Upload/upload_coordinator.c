/**
 * @file upload_coordinator.c
 * @brief Capture persistence, upload dispatch, retry, cleanup.
 *
 * On-disk layout (root is /captures/ on the chosen FS):
 *   /captures/data/cap_<unix_ts>_<seq>_p.jpg          primary image
 *   /captures/data/cap_<unix_ts>_<seq>_i.jpg          inference image (optional)
 *   /captures/data/cap_<unix_ts>_<seq>_a.json         AI result (optional)
 *   /captures/pending/cap_<unix_ts>_<seq>.json        record metadata, awaiting upload
 *   /captures/sent/cap_<unix_ts>_<seq>.json           record metadata, uploaded ok
 *   /captures/failed/cap_<unix_ts>_<seq>.json         record metadata, retry budget spent
 *   /captures/local/cap_<unix_ts>_<seq>.json          record metadata, LOCAL_ONLY mode
 */

#include "upload_coordinator.h"

#include "cJSON.h"
#include "cmsis_os2.h"
#include "tx_api.h"
#include "debug.h"
#include "buffer_mgr.h"
#include "common_utils.h"
#include "generic_file.h"
#include "sd_file.h"
#include "storage.h"
#include "drtc.h"
#include "device_service.h"
#include "mqtt_service.h"
#include "webhook_service.h"
#include "service_init.h"
#include "wake_scheduler.h"
#include "nn.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

/* Verbose [UPLOAD] diagnostic logs. Default OFF — set to 1 to trace init,
 * space checks, enqueue/dispatch, network waits, and flush passes. Key
 * operational info still goes through LOG_SVC_* (the log system) regardless. */
#define UPLOAD_DEBUG 0
#if UPLOAD_DEBUG
#define UPLOAD_LOG(...) printf("[UPLOAD] " __VA_ARGS__)
#else
#define UPLOAD_LOG(...) ((void)0)
#endif

/* ==================== Configuration ==================== */

#define UPLOAD_TASK_STACK       (4096 * 4)
#define UPLOAD_TASK_PRIORITY    osPriorityBelowNormal
#define UPLOAD_QUEUE_DEPTH      8

#define CAPTURES_ROOT           "/captures"
#define CAPTURES_DIR_DATA       CAPTURES_ROOT "/data"
#define CAPTURES_DIR_PENDING    CAPTURES_ROOT "/pending"
#define CAPTURES_DIR_SENT       CAPTURES_ROOT "/sent"
#define CAPTURES_DIR_FAILED     CAPTURES_ROOT "/failed"
#define CAPTURES_DIR_LOCAL      CAPTURES_ROOT "/local"

/* Cleanup watermark (bytes) — keep this much head-room when wrapping. */
#define CLEANUP_HEADROOM_BYTES  (512u * 1024u)

/* Max wait for SD media to finish opening before a capture-store attempt.
 * sdProcess opens the media asynchronously; on a cold boot the first capture
 * may arrive before fx_media_open completes, so we wait rather than fail. */
#define SD_STORE_READY_TIMEOUT_MS  3000u

/* Minimum free bytes required before calling ensure_dirs at init time.
 * Creating 6 directories on littlefs requires metadata-pair allocations
 * (~8 KB per dir + parent commits). We add margin so lfs GC never spins. */
#define ENSURE_DIRS_MIN_FREE_BYTES (512u * 1024u)

/* Event message types delivered to the worker queue. */
typedef enum {
    EV_KICK         = 1,
    EV_DRAIN        = 2,
    EV_RELOAD       = 3,
    EV_SHUTDOWN     = 4,
} upload_event_kind_t;

/* Queue carries a single ULONG per slot (matches webhook_service's pattern —
 * avoids -fshort-enums shrinking an enum-typed struct and breaking the
 * mq_size >= count*msg_size check in the CMSIS-RTOS2 wrapper). */
typedef uint32_t upload_event_t;

/* ==================== State ==================== */

typedef struct {
    aicam_bool_t       initialized;
    aicam_bool_t       running;
    service_state_t    state;
    osMutexId_t        mutex;            /* protects do_flush_pass re-entrancy */
    osMessageQueueId_t queue;
    osThreadId_t       task_handle;
    volatile aicam_bool_t flush_active;  /* true while do_flush_pass is running — lets the
                                          * sleep path wait for an in-progress flush to finish
                                          * before cutting power (avoids abandoning an upload) */

    capture_upload_config_t cfg;            /* cached copy, reloaded via EV_RELOAD */
    FS_Type_t          active_fs;           /* AUTO resolves here */
    aicam_bool_t       storage_full;        /* set when STOP policy rejects a write */
} upload_state_t;

static upload_state_t g_up;

static uint8_t upload_task_stack[UPLOAD_TASK_STACK] __attribute__((aligned(32))) IN_PSRAM;

static TX_QUEUE upload_queue_cb __attribute__((aligned(8)));
static uint8_t  upload_queue_mem[UPLOAD_QUEUE_DEPTH * sizeof(ULONG)] __attribute__((aligned(8)));
static const osMessageQueueAttr_t upload_queue_attr = {
    .name = "upload_q",
    .cb_mem = &upload_queue_cb,
    .cb_size = sizeof(upload_queue_cb),
    .mq_mem = upload_queue_mem,
    .mq_size = sizeof(upload_queue_mem),
};

/* ==================== Forward Decls ==================== */

static void        upload_task(void *arg);
static aicam_result_t ensure_dirs(FS_Type_t fs);
static void        ensure_dirs_with_space_check(FS_Type_t fs);
static FS_Type_t   resolve_storage_target(capture_storage_t cs);
static const char *trigger_str(aicam_capture_trigger_t t);
static const char *wakeup_src_str(wakeup_source_type_t w);
static const char *state_dir(record_state_t s);
static void        path_for_record(char *out, size_t n, const char *dir, const char *id);
static void        path_for_data(char *out, size_t n, const char *id, char suffix);
static aicam_result_t persist_record(FS_Type_t fs, const char *id,
                                      const uint8_t *jpeg, uint32_t jpeg_size,
                                      const uint8_t *inf, uint32_t inf_size,
                                      const char *ai_json,
                                      const mqtt_image_metadata_t *meta,
                                      aicam_capture_trigger_t trigger,
                                      wakeup_source_type_t wakeup_src,
                                      record_state_t state);
static aicam_result_t move_record(FS_Type_t fs, const char *id,
                                   record_state_t from, record_state_t to);
static aicam_result_t parse_meta_file(FS_Type_t fs, const char *path, cJSON **out_json);
static aicam_result_t upload_one_record(FS_Type_t fs, const char *id);
static aicam_result_t cleanup_for_space(FS_Type_t fs, uint64_t need_bytes);
static aicam_result_t purge_old_sent(FS_Type_t fs);
static uint32_t      count_in_dir(FS_Type_t fs, const char *dir);
static aicam_result_t get_storage_free_bytes(FS_Type_t fs, uint64_t *out_free, uint64_t *out_total);

/* ==================== Helpers ==================== */

static uint64_t now_unix(void)
{
    return rtc_get_timeStamp();
}

static uint32_t next_seq(void)
{
    /* RAM-only counter — resets to 0 on each cold boot (wake). Different wakes
     * have different timestamps (minutes apart), so cross-boot id collision is
     * impossible. Within a single wake, the counter increments for same-second
     * captures (e.g. PIR + button simultaneously). No NVS write — eliminates
     * per-capture flash wear that the previous NVS-persisted counter caused. */
    static uint32_t s = 0;
    return ++s;
}

static const char *trigger_str(aicam_capture_trigger_t t)
{
    switch (t) {
    case AICAM_CAPTURE_TRIGGER_RTC:      return "rtc";
    case AICAM_CAPTURE_TRIGGER_PIR:      return "pir";
    case AICAM_CAPTURE_TRIGGER_WEB:      return "web";
    case AICAM_CAPTURE_TRIGGER_REMOTE:   return "remote";
    case AICAM_CAPTURE_TRIGGER_GPIO:     return "gpio";
    case AICAM_CAPTURE_TRIGGER_BUTTON:   return "button";
    case AICAM_CAPTURE_TRIGGER_SCHEDULE: return "schedule";
    default:                             return "unknown";
    }
}

static const char *wakeup_src_str(wakeup_source_type_t w)
{
    switch (w) {
    case WAKEUP_SOURCE_IO:                return "io";
    case WAKEUP_SOURCE_RTC:               return "rtc";
    case WAKEUP_SOURCE_PIR:               return "pir";
    case WAKEUP_SOURCE_BUTTON:            return "button";
    case WAKEUP_SOURCE_BUTTON_LONG:       return "button_long";
    case WAKEUP_SOURCE_BUTTON_SUPER_LONG: return "button_xlong";
    case WAKEUP_SOURCE_REMOTE:            return "remote";
    case WAKEUP_SOURCE_WUFI:              return "wufi";
    default:                              return "other";
    }
}

static const char *state_dir(record_state_t s)
{
    switch (s) {
    case RECORD_STATE_PENDING: return CAPTURES_DIR_PENDING;
    case RECORD_STATE_SENT:    return CAPTURES_DIR_SENT;
    case RECORD_STATE_FAILED:  return CAPTURES_DIR_FAILED;
    case RECORD_STATE_LOCAL:   return CAPTURES_DIR_LOCAL;
    default:                   return CAPTURES_DIR_PENDING;
    }
}

/* ==================== Date-partitioned paths ==================== */

/* Parse the unix-timestamp field from a record id ("cap_<ts>_<seq>"). The ts
 * is zero-padded for lexicographic sort. Returns 0 on parse failure. */
static unsigned long ts_from_id(const char *id)
{
    if (!id || strncmp(id, "cap_", 4) != 0) return 0;
    unsigned long ts = 0;
    if (sscanf(id + 4, "%lu_", &ts) != 1) return 0;
    return ts;
}

/* Format a record id's timestamp as a UTC date directory name "YYYY-MM-DD".
 * On parse failure returns "unknown" so the record still lands somewhere
 * deterministic and listable. */
static void date_dir_from_id(const char *id, char *out, size_t n)
{
    if (!out || n == 0) return;
    unsigned long ts = ts_from_id(id);
    if (ts == 0) { snprintf(out, n, "unknown"); return; }
    time_t t = (time_t)ts;
    struct tm *tmv = gmtime(&t);
    if (!tmv) { snprintf(out, n, "unknown"); return; }
    strftime(out, n, "%Y-%m-%d", tmv);
}

/* Format an arbitrary unix timestamp as "YYYY-MM-DD" (UTC). */
static void date_str_from_ts(uint64_t ts, char *out, size_t n)
{
    if (!out || n == 0) return;
    if (ts == 0) { snprintf(out, n, "0000-00-00"); return; }
    time_t t = (time_t)ts;
    struct tm *tmv = gmtime(&t);
    if (!tmv) { snprintf(out, n, "0000-00-00"); return; }
    strftime(out, n, "%Y-%m-%d", tmv);
}

/* Ensure "<top_dir>/<date>/" exists, deriving date from id. stat-first to
 * avoid lfs_mkdir's expensive lfs_fs_forceconsistency() on the common path
 * (date dir usually already exists from a prior capture that day). */
static void ensure_date_dir(FS_Type_t fs, const char *top_dir, const char *id)
{
    char date[16];
    date_dir_from_id(id, date, sizeof(date));
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", top_dir, date);
    struct stat st = {0};
    if (disk_file_stat(fs, path, &st) == 0) return;  /* date dir exists */
    (void)disk_file_mkdir(fs, path);
}

static void path_for_record(char *out, size_t n, const char *dir, const char *id)
{
    char date[16];
    date_dir_from_id(id, date, sizeof(date));
    snprintf(out, n, "%s/%s/%s.json", dir, date, id);
}

static void path_for_data(char *out, size_t n, const char *id, char suffix)
{
    char date[16];
    date_dir_from_id(id, date, sizeof(date));
    snprintf(out, n, "%s/%s/%s_%c.%s", CAPTURES_DIR_DATA, date, id, suffix,
             suffix == 'a' ? "json" : "jpg");
}

/* ==================== Storage selection ==================== */

static FS_Type_t resolve_storage_target(capture_storage_t cs)
{
    switch (cs) {
    case CAPTURE_STORE_SD:
        return FS_SD;
    case CAPTURE_STORE_FLASH:
        return FS_FLASH;
    case CAPTURE_STORE_NONE:
        return FS_MAX; /* signal "no persistence" */
    case CAPTURE_STORE_AUTO:
    default:
        if (device_service_storage_is_sd_connected()) return FS_SD;
        return FS_FLASH;
    }
}

/* Non-blocking FS readiness check for ensure_dirs. Returns true only when the
 * backing FS is actually mounted/open — so the init-time call (SD media may
 * still be opening asynchronously in sdProcess) does NOT cache a failed mkdir
 * run and block the real mkdir from happening later in persist_record. */
static bool fs_ready_for_dirs(FS_Type_t fs)
{
    if (fs == FS_SD) {
        return sd_is_media_open() ? true : false;
    }
    if (fs == FS_FLASH) {
        /* Just read the RAM mounted flag — do NOT call storage_get_disk_info
         * (that runs lfs_fs_size, a full 8192-block traverse, just to read a
         * boolean). The caller (ensure_dirs_with_space_check / enqueue_capture)
         * already did a real space probe, so the volume is known accessible. */
        return storage_is_lfs_mounted();
    }
    return false;
}

static aicam_result_t ensure_dirs(FS_Type_t fs)
{
    if (fs == FS_MAX) return AICAM_OK;
    /* If the backing FS isn't ready yet (e.g. SD media still opening at init
     * time), bail without touching s_ensured so persist_record retries the
     * real mkdir once the FS is up. Caching a failure here is what previously
     * caused /captures/data to never get created and every write to fail. */
    if (!fs_ready_for_dirs(fs)) return AICAM_OK;

    /* mkdir is expensive on SD (166ms+ per call). Once we've created the
     * captures tree on a given filesystem this wake, skip — the dirs persist
     * across sleeps anyway. Track per-filesystem so switching storage (e.g.
     * SD→FLASH via web) still creates dirs on the new target. */
    static FS_Type_t s_ensured_fs = FS_MAX;
    if (s_ensured_fs == fs) return AICAM_OK;

    /* NOTE: the caller (ensure_dirs_with_space_check or enqueue_capture) has
     * already verified free space against the ENSURE_DIRS_MIN_FREE_BYTES /
     * CLEANUP_HEADROOM threshold. Do NOT re-probe here — a second
     * storage_get_disk_info would run another full lfs_fs_size traverse
     * (~120 ms holding the storage mutex) and contend with the log task. */

    uint64_t te = rtc_get_uptime_ms();
    const char *dirs[] = {
        CAPTURES_ROOT,
        CAPTURES_DIR_DATA,
        CAPTURES_DIR_PENDING,
        CAPTURES_DIR_SENT,
        CAPTURES_DIR_FAILED,
        CAPTURES_DIR_LOCAL,
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        /* stat first: lfs_mkdir is NOT cheaply idempotent — every call runs
         * lfs_fs_forceconsistency() (a full deorphan traverse) before checking
         * existence, so calling it on already-existing dirs is expensive and
         * can hang if the FS has a borderline state (deorphan loops with
         * LFS_ASSERT disabled). Only mkdir when the dir is actually missing. */
        struct stat st = {0};
        if (disk_file_stat(fs, dirs[i], &st) == 0) continue;  /* exists */
        (void)disk_file_mkdir(fs, dirs[i]);
    }
    s_ensured_fs = fs;
    uint64_t dt = rtc_get_uptime_ms() - te;
    if (dt > 5) {
        UPLOAD_LOG("ensure_dirs time=%lu ms\r\n", (unsigned long)dt);
    }
    return AICAM_OK;
}

/* Check free space, run WRAP-policy cleanup if needed, and call ensure_dirs
 * only when it's safe.  Shared by init, reload_config (sync), and EV_RELOAD. */
static void ensure_dirs_with_space_check(FS_Type_t fs)
{
    uint64_t free_bytes = 0, total = 0;
    if (get_storage_free_bytes(fs, &free_bytes, &total) == AICAM_OK) {
        UPLOAD_LOG("ensure_dirs_with_space_check: free=%lu KB total=%lu KB "
               "policy=%d\r\n",
               (unsigned long)(free_bytes / 1024),
               (unsigned long)(total / 1024),
               (int)g_up.cfg.policy);

        if (free_bytes < ENSURE_DIRS_MIN_FREE_BYTES) {
            UPLOAD_LOG("ensure_dirs_with_space_check: low space "
                   "(%lu KB < %lu KB min)\r\n",
                   (unsigned long)(free_bytes / 1024),
                   (unsigned long)(ENSURE_DIRS_MIN_FREE_BYTES / 1024));

            if (g_up.cfg.policy == STORAGE_POLICY_WRAP) {
                uint64_t need = ENSURE_DIRS_MIN_FREE_BYTES - free_bytes
                                + CLEANUP_HEADROOM_BYTES;
                UPLOAD_LOG("ensure_dirs_with_space_check: WRAP policy, "
                       "cleaning up (need=%lu KB)...\r\n",
                       (unsigned long)(need / 1024));
                if (cleanup_for_space(fs, need) == AICAM_OK) {
                    get_storage_free_bytes(fs, &free_bytes, &total);
                    UPLOAD_LOG("ensure_dirs_with_space_check: "
                           "after cleanup free=%lu KB\r\n",
                           (unsigned long)(free_bytes / 1024));
                } else {
                    UPLOAD_LOG("ensure_dirs_with_space_check: "
                           "cleanup insufficient\r\n");
                }
            }

            if (free_bytes < ENSURE_DIRS_MIN_FREE_BYTES) {
                UPLOAD_LOG("ensure_dirs_with_space_check: "
                       "still low on space — skipping ensure_dirs, "
                       "storage marked full\r\n");
                g_up.storage_full = AICAM_TRUE;
                return;
            }
        }
    } else {
        UPLOAD_LOG("ensure_dirs_with_space_check: "
               "get_storage_free_bytes failed, proceeding anyway\r\n");
    }

    ensure_dirs(fs);
}

static aicam_result_t get_storage_free_bytes(FS_Type_t fs, uint64_t *out_free, uint64_t *out_total)
{
    if (!out_free || !out_total) return AICAM_ERROR_INVALID_PARAM;
    *out_free = 0;
    *out_total = 0;

    if (fs == FS_SD) {
        sd_disk_info_t info;
        if (sd_get_disk_info(&info) != 0) return AICAM_ERROR;
        *out_free = (uint64_t)info.free_KBytes * 1024u;
        *out_total = (uint64_t)info.total_KBytes * 1024u;
        return AICAM_OK;
    }
    if (fs == FS_FLASH) {
        storage_disk_info_t info;
        if (storage_get_disk_info(&info) != 0) return AICAM_ERROR;
        *out_free = (uint64_t)info.free_KBytes * 1024u;
        *out_total = (uint64_t)info.total_KBytes * 1024u;
        return AICAM_OK;
    }
    return AICAM_ERROR_INVALID_PARAM;
}

/* ==================== File-write helpers ==================== */

static aicam_result_t write_file(FS_Type_t fs, const char *path,
                                  const uint8_t *buf, uint32_t size)
{
    void *fd = disk_file_fopen(fs, path, "w");
    if (!fd) {
        LOG_SVC_ERROR("write_file: fopen failed path=%s", path);
        return AICAM_ERROR;
    }
    uint32_t off = 0;
    aicam_result_t r = AICAM_OK;
    while (off < size) {
        uint32_t chunk = (size - off > 4096) ? 4096 : (size - off);
        int wn = disk_file_fwrite(fs, fd, buf + off, chunk);
        if (wn != (int)chunk) {
            LOG_SVC_ERROR("write_file: fwrite short path=%s off=%lu chunk=%u wn=%d",
                          path, (unsigned long)off, (unsigned)chunk, wn);
            r = AICAM_ERROR;
            break;
        }
        off += chunk;
    }
    disk_file_fclose(fs, fd);
    return r;
}

static aicam_result_t read_text_file(FS_Type_t fs, const char *path, char **out, uint32_t *out_size)
{
    if (!out || !out_size) return AICAM_ERROR_INVALID_PARAM;
    *out = NULL;
    *out_size = 0;
    struct stat st = {0};
    if (disk_file_stat(fs, path, &st) != 0 || st.st_size <= 0) return AICAM_ERROR;
    if (st.st_size > 1024 * 1024) return AICAM_ERROR;  /* sanity */
    char *buf = (char *)buffer_calloc(1, (size_t)st.st_size + 1);
    if (!buf) return AICAM_ERROR_NO_MEMORY;
    void *fd = disk_file_fopen(fs, path, "r");
    if (!fd) { buffer_free(buf); return AICAM_ERROR; }
    int rn = disk_file_fread(fs, fd, buf, (size_t)st.st_size);
    disk_file_fclose(fs, fd);
    if (rn <= 0) { buffer_free(buf); return AICAM_ERROR; }
    buf[rn] = '\0';
    *out = buf;
    *out_size = (uint32_t)rn;
    return AICAM_OK;
}

/* ==================== Metadata JSON ==================== */

static cJSON *build_record_meta_json(const char *id,
                                     const mqtt_image_metadata_t *meta,
                                     aicam_capture_trigger_t trigger,
                                     wakeup_source_type_t wakeup_src,
                                     const char *primary_name,
                                     const char *inference_name,
                                     const char *ai_name,
                                     upload_proto_t protocol)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) return NULL;
    cJSON_AddNumberToObject(r, "version", 1);
    cJSON_AddStringToObject(r, "id", id);
    cJSON_AddStringToObject(r, "image_id", meta->image_id);
    cJSON_AddNumberToObject(r, "timestamp", (double)meta->timestamp);
    cJSON_AddNumberToObject(r, "width", meta->width);
    cJSON_AddNumberToObject(r, "height", meta->height);
    cJSON_AddNumberToObject(r, "size", meta->size);
    cJSON_AddNumberToObject(r, "quality", meta->quality);
    cJSON_AddNumberToObject(r, "format", meta->format);
    cJSON_AddStringToObject(r, "trigger", trigger_str(trigger));
    cJSON_AddStringToObject(r, "wakeup_source", wakeup_src_str(wakeup_src));
    cJSON_AddNumberToObject(r, "trigger_type_id", (double)trigger);
    cJSON_AddStringToObject(r, "primary", primary_name ? primary_name : "");
    cJSON_AddStringToObject(r, "inference_image", inference_name ? inference_name : "");
    cJSON_AddStringToObject(r, "ai_result", ai_name ? ai_name : "");
    cJSON_AddStringToObject(r, "upload_protocol",
                            protocol == UPLOAD_PROTO_WEBHOOK ? "webhook" : "mqtt");
    cJSON_AddNumberToObject(r, "retry_count", 0);
    cJSON_AddStringToObject(r, "last_error", "");

    /* battery snapshot (best-effort).
     * Only need battery %, device name, serial — do NOT call
     * device_service_get_info() which internally calls update_storage_info()
     * → storage_get_disk_info() → lfs_fs_size() (230 ms on 8189-block flash).
     * These fields are updated at boot and change rarely. */
    {
        device_info_config_t dev_buf;
        if (device_service_get_cached_info(&dev_buf) == AICAM_OK) {
            cJSON *bat = cJSON_AddObjectToObject(r, "battery");
            if (bat) {
                cJSON_AddNumberToObject(bat, "percent", dev_buf.battery_percent);
            }
            cJSON_AddStringToObject(r, "device_name", dev_buf.device_name);
            cJSON_AddStringToObject(r, "serial_number", dev_buf.serial_number);
        }
    }
    return r;
}

static aicam_result_t parse_meta_file(FS_Type_t fs, const char *path, cJSON **out_json)
{
    if (!out_json) return AICAM_ERROR_INVALID_PARAM;
    *out_json = NULL;
    char *text = NULL;
    uint32_t size = 0;
    aicam_result_t r = read_text_file(fs, path, &text, &size);
    if (r != AICAM_OK) return r;
    cJSON *json = cJSON_Parse(text);
    buffer_free(text);
    if (!json) return AICAM_ERROR;
    *out_json = json;
    return AICAM_OK;
}

static aicam_result_t write_meta_json(FS_Type_t fs, const char *path, cJSON *json)
{
    char *str = cJSON_PrintUnformatted(json);
    if (!str) return AICAM_ERROR;
    aicam_result_t r = write_file(fs, path, (const uint8_t *)str, (uint32_t)strlen(str));
    cJSON_free(str);
    return r;
}

/* ==================== Persist a record ==================== */

static aicam_result_t persist_record(FS_Type_t fs, const char *id,
                                      const uint8_t *jpeg, uint32_t jpeg_size,
                                      const uint8_t *inf, uint32_t inf_size,
                                      const char *ai_json,
                                      const mqtt_image_metadata_t *meta,
                                      aicam_capture_trigger_t trigger,
                                      wakeup_source_type_t wakeup_src,
                                      record_state_t state)
{
    if (fs == FS_MAX) return AICAM_ERROR_INVALID_PARAM;
    if (ensure_dirs(fs) != AICAM_OK) return AICAM_ERROR;

    /* Ensure the per-date subdirectories exist for this record's capture date
     * (derived from id). Created on demand rather than in ensure_dirs() so the
     * top-level tree stays small and old date dirs are never pre-created. */
    ensure_date_dir(fs, state_dir(state), id);
    ensure_date_dir(fs, CAPTURES_DIR_DATA, id);

    char pri_path[128], inf_path[128], ai_path[128], meta_path[128];
    path_for_data(pri_path, sizeof(pri_path), id, 'p');
    path_for_data(inf_path, sizeof(inf_path), id, 'i');
    path_for_data(ai_path,  sizeof(ai_path),  id, 'a');
    path_for_record(meta_path, sizeof(meta_path), state_dir(state), id);

    aicam_result_t r = write_file(fs, pri_path, jpeg, jpeg_size);
    if (r != AICAM_OK) {
        LOG_SVC_ERROR("upload: write primary image failed: %s", pri_path);
        return r;
    }

    if (inf && inf_size > 0) {
        if (write_file(fs, inf_path, inf, inf_size) != AICAM_OK) {
            LOG_SVC_WARN("upload: write inference image failed: %s", inf_path);
            inf = NULL;
        }
    }
    if (ai_json && ai_json[0]) {
        if (write_file(fs, ai_path, (const uint8_t *)ai_json, (uint32_t)strlen(ai_json)) != AICAM_OK) {
            LOG_SVC_WARN("upload: write ai_result failed: %s", ai_path);
            ai_json = NULL;
        }
    }

    char pri_name[64], inf_name[64], ai_name[64];
    snprintf(pri_name, sizeof(pri_name), "%s_p.jpg", id);
    snprintf(inf_name, sizeof(inf_name), inf ? "%s_i.jpg" : "", id);
    snprintf(ai_name,  sizeof(ai_name),  ai_json ? "%s_a.json" : "", id);

    cJSON *j = build_record_meta_json(id, meta, trigger, wakeup_src,
                                       pri_name, inf ? inf_name : "",
                                       ai_json ? ai_name : "",
                                       g_up.cfg.upload_protocol);
    if (!j) return AICAM_ERROR_NO_MEMORY;
    aicam_result_t rr = write_meta_json(fs, meta_path, j);
    cJSON_Delete(j);
    return rr;
}

static aicam_result_t move_record(FS_Type_t fs, const char *id,
                                   record_state_t from, record_state_t to)
{
    char from_path[128], to_path[128];
    path_for_record(from_path, sizeof(from_path), state_dir(from), id);
    path_for_record(to_path,   sizeof(to_path),   state_dir(to),   id);

    /* Target date subdir (same capture date as source) may not exist yet on
     * the destination state tree — create it before rename. */
    ensure_date_dir(fs, state_dir(to), id);

    /* FileX fx_file_rename returns FX_ALREADY_CREATED (0x0B) if the target
     * exists — this happens when a prior flush renamed the record to sent/
     * but a later enqueue (pre-id-fix bug) overwrote pending/ with the same
     * id. Remove the stale target first so the rename succeeds. */
    struct stat st = {0};
    if (disk_file_stat(fs, to_path, &st) == 0) {
        (void)disk_file_remove(fs, to_path);
    }

    if (disk_file_rename(fs, from_path, to_path) != 0) {
        /* Fallback: copy content + delete source (handles FS that can't
         * rename across directories atomically). Metadata is small JSON. */
        char *content = NULL;
        uint32_t size = 0;
        if (read_text_file(fs, from_path, &content, &size) == AICAM_OK) {
            aicam_result_t wr = write_file(fs, to_path, (const uint8_t *)content, size);
            buffer_free(content);
            if (wr == AICAM_OK) {
                (void)disk_file_remove(fs, from_path);
                if (to == RECORD_STATE_SENT) LOG_SVC_INFO("upload ok: %s", id);
                return AICAM_OK;
            }
        }
        return AICAM_ERROR;
    }
    if (to == RECORD_STATE_SENT) LOG_SVC_INFO("upload ok: %s", id);
    return AICAM_OK;
}

static void delete_record_files(FS_Type_t fs, const char *id, record_state_t state)
{
    /* Only remove files that actually exist — calling remove on a non-existent
     * path makes the SD FileX driver log "fx_file_delete failed: 0x04" for
     * every absent file (4 states × 4 files = 16 attempts, most miss). */
    char path[128];
    struct stat st = {0};

    path_for_record(path, sizeof(path), state_dir(state), id);
    if (disk_file_stat(fs, path, &st) == 0) (void)disk_file_remove(fs, path);

    path_for_data(path, sizeof(path), id, 'p');
    if (disk_file_stat(fs, path, &st) == 0) (void)disk_file_remove(fs, path);

    path_for_data(path, sizeof(path), id, 'i');
    if (disk_file_stat(fs, path, &st) == 0) (void)disk_file_remove(fs, path);

    path_for_data(path, sizeof(path), id, 'a');
    if (disk_file_stat(fs, path, &st) == 0) (void)disk_file_remove(fs, path);
}

/* ==================== Directory iteration ==================== */

/* Compatible readdir entry — must be a superset of both lfs_info and sd_info
 * layouts. type(1) + pad(3) + size(4) + name(256) + mtime(4) + short_name(14).
 */
typedef struct {
    uint8_t  type;
    uint8_t  _pad[3];
    uint32_t size;
    char     name[256];
    uint32_t mtime;
    char     short_name[14];
} dir_entry_t;

/* Iterate records in a state dir. Returns count of records visited (passing offset/limit filter). */
typedef aicam_result_t (*record_visitor_t)(FS_Type_t fs, const char *id, void *user);

/* Iterate records in a state dir, traversing per-date subdirectories.
 *
 *   dir        — top-level state dir (e.g. /captures/pending)
 *   offset/limit — pagination (visit mode)
 *   from_ts/to_ts — inclusive time range (exact, per-record); 0 / UINT64_MAX = unbounded
 *   sort_desc  — newest date first
 *   fn/user    — visitor; NULL = count mode (return matching total, no alloc)
 *
 * Records live at "<dir>/<YYYY-MM-DD>/cap_<ts>_<seq>.json". Date subdirs are
 * enumerated once (small set), filtered at day granularity via lexicographic
 * date-string compare, then each is scanned. Visit mode collects at most
 * `limit` ids into a small fixed buffer and stops early once the page is
 * full — no full-volume readdir, no giant alloc. Count mode is single-pass
 * readdir with no allocation.
 *
 * Returns: count mode → matching total; visit mode → records passed to fn. */
static int iterate_records(FS_Type_t fs, const char *dir,
                            uint32_t offset, uint32_t limit,
                            uint64_t from_ts, uint64_t to_ts,
                            aicam_bool_t sort_desc,
                            record_visitor_t fn, void *user)
{
    /* --- Phase 1: enumerate date subdirs (YYYY-MM-DD, 10 chars) --- */
    #define MAX_DATE_DIRS 128
    char (*dates)[12] = (char (*)[12])buffer_calloc(MAX_DATE_DIRS, 12);
    if (!dates) return 0;
    int ndates = 0;
    void *dd = disk_file_opendir(fs, dir);
    if (dd) {
        dir_entry_t e;
        while (ndates < MAX_DATE_DIRS && disk_file_readdir(fs, dd, (char *)&e) > 0) {
            const char *name = e.name;
            if (name[0] == '.') continue;
            size_t nlen = strlen(name);
            if (nlen != 10) continue;
            if (name[4] != '-' || name[7] != '-') continue;
            int ok = 1;
            for (int i = 0; i < 10; i++) {
                if (i == 4 || i == 7) continue;
                if (name[i] < '0' || name[i] > '9') { ok = 0; break; }
            }
            if (!ok) continue;
            memcpy(dates[ndates], name, 11);
            ndates++;
        }
        disk_file_closedir(fs, dd);
    }
    if (ndates == 0) { buffer_free(dates); return 0; }

    /* Insertion sort lexicographically (= chronological). n is small. */
    for (int i = 1; i < ndates; i++) {
        char tmp[12];
        memcpy(tmp, dates[i], 12);
        int j = i - 1;
        while (j >= 0 && strcmp(dates[j], tmp) > 0) {
            memcpy(dates[j+1], dates[j], 12);
            j--;
        }
        memcpy(dates[j+1], tmp, 12);
    }
    if (sort_desc) {
        for (int i = 0; i < ndates/2; i++) {
            char tmp[12];
            memcpy(tmp, dates[i], 12);
            memcpy(dates[i], dates[ndates-1-i], 12);
            memcpy(dates[ndates-1-i], tmp, 12);
        }
    }

    /* Day-granularity range filter: compare date dir name to [from_date, to_date]
     * as strings. Lexicographic = chronological for YYYY-MM-DD. */
    char from_date[12], to_date[12];
    date_str_from_ts(from_ts > 0 ? from_ts : 0, from_date, sizeof(from_date));
    if (to_ts == UINT64_MAX) {
        snprintf(to_date, sizeof(to_date), "9999-99-99");
    } else {
        date_str_from_ts(to_ts, to_date, sizeof(to_date));
    }

    /* --- Visit mode: pre-allocate page buffer (≤ limit ids) --- */
    char (*page_ids)[64] = NULL;
    if (fn != NULL) {
        uint32_t pcap = (limit > 0 && limit <= 200) ? limit : 200;
        page_ids = (char (*)[64])buffer_calloc(pcap, 64);
        if (!page_ids) { buffer_free(dates); return 0; }
    }

    uint32_t collected = 0;   /* matching records seen (for offset) */
    uint32_t emitted = 0;     /* records passed to fn */
    int total = 0;            /* count-mode total */
    int day_page = 0;
    uint32_t pcap = (limit > 0 && limit <= 200) ? limit : 200;

    for (int di = 0; di < ndates; di++) {
        const char *dn = dates[di];
        if (strcmp(dn, from_date) < 0) continue;
        if (strcmp(dn, to_date) > 0) continue;

        char subpath[96];
        snprintf(subpath, sizeof(subpath), "%s/%s", dir, dn);

        /* ---- Pass 1: count matching records in this date dir ---- */
        int day_count = 0;
        dd = disk_file_opendir(fs, subpath);
        if (!dd) continue;
        dir_entry_t e;
        while (disk_file_readdir(fs, dd, (char *)&e) > 0) {
            const char *name = e.name;
            if (name[0] == '.') continue;
            size_t nlen = strlen(name);
            if (nlen < 6 || strcmp(name + nlen - 5, ".json") != 0) continue;
            if (strncmp(name, "cap_", 4) != 0) continue;
            char idbuf[64];
            size_t copy = nlen - 5; if (copy >= 63) copy = 63;
            memcpy(idbuf, name, copy); idbuf[copy] = 0;
            unsigned long rts = ts_from_id(idbuf);
            if (rts != 0) {
                if (from_ts > 0 && rts < from_ts) continue;
                if (to_ts != UINT64_MAX && rts > to_ts) continue;
            }
            day_count++;
        }
        disk_file_closedir(fs, dd);

        if (day_count == 0) continue;

        /* Count mode: done with this dir */
        if (fn == NULL) { total += day_count; continue; }

        /* ---- Pass 2: collect matching ids into a per-day array ---- */
        char (*day_ids)[64] = (char (*)[64])buffer_calloc(day_count, 64);
        if (!day_ids) continue;
        int j = 0;
        dd = disk_file_opendir(fs, subpath);
        if (dd) {
            while (disk_file_readdir(fs, dd, (char *)&e) > 0 && j < day_count) {
                const char *name = e.name;
                if (name[0] == '.') continue;
                size_t nlen = strlen(name);
                if (nlen < 6 || strcmp(name + nlen - 5, ".json") != 0) continue;
                if (strncmp(name, "cap_", 4) != 0) continue;
                char idbuf[64];
                size_t copy = nlen - 5; if (copy >= 63) copy = 63;
                memcpy(idbuf, name, copy); idbuf[copy] = 0;
                unsigned long rts = ts_from_id(idbuf);
                if (rts != 0) {
                    if (from_ts > 0 && rts < from_ts) continue;
                    if (to_ts != UINT64_MAX && rts > to_ts) continue;
                }
                memcpy(day_ids[j], idbuf, copy + 1);
                j++;
            }
            disk_file_closedir(fs, dd);
        }
        day_count = j;
        if (day_count == 0) { buffer_free(day_ids); continue; }

        /* Sort ascending by id string — lexicographic = chronological for
         * "cap_<zero-padded-ts>_<zero-padded-seq>". littlefs readdir order is
         * NOT guaranteed chronological (entries can move during compaction),
         * so we must sort explicitly. Insertion sort (day_count is typically
         * small, bounded by one day's captures). */
        for (int a = 1; a < day_count; a++) {
            char tmp[64];
            memcpy(tmp, day_ids[a], 64);
            int b = a - 1;
            while (b >= 0 && strcmp(day_ids[b], tmp) > 0) {
                memcpy(day_ids[b+1], day_ids[b], 64);
                b--;
            }
            memcpy(day_ids[b+1], tmp, 64);
        }
        /* For desc (newest-first), reverse within the day. Date dirs are
         * already reversed above, so the overall order is newest-date /
         * newest-record first. */
        if (sort_desc) {
            for (int a = 0; a < day_count / 2; a++) {
                char tmp[64];
                memcpy(tmp, day_ids[a], 64);
                memcpy(day_ids[a], day_ids[day_count-1-a], 64);
                memcpy(day_ids[day_count-1-a], tmp, 64);
            }
        }

        /* ---- Feed sorted ids to the page buffer with offset/limit + chunked flush ---- */
        for (int k = 0; k < day_count; k++) {
            if (collected < offset) { collected++; continue; }
            if (limit > 0 && emitted >= limit) break;

            if (day_page < (int)pcap) {
                memcpy(page_ids[day_page], day_ids[k], 64);
                day_page++;
            }
            collected++;
            emitted++;

            if (day_page >= (int)pcap && (limit == 0 || emitted < limit)) {
                for (int m = 0; m < day_page; m++) {
                    if (fn(fs, page_ids[m], user) != AICAM_OK) {
                        buffer_free(day_ids); buffer_free(page_ids); buffer_free(dates);
                        return (int)emitted;
                    }
                }
                day_page = 0;
            }
        }
        buffer_free(day_ids);

        /* flush remaining buffered ids for this day */
        if (day_page > 0) {
            for (int m = 0; m < day_page; m++) {
                if (fn(fs, page_ids[m], user) != AICAM_OK) {
                    buffer_free(page_ids); buffer_free(dates);
                    return (int)emitted;
                }
            }
            day_page = 0;
        }
        if (limit > 0 && emitted >= limit) break;
    }

    buffer_free(page_ids);
    buffer_free(dates);
    return fn ? (int)emitted : total;
}

static uint32_t count_in_dir(FS_Type_t fs, const char *dir)
{
    if (fs == FS_MAX) return 0;
    uint64_t tc = rtc_get_uptime_ms();
    uint32_t n = (uint32_t)iterate_records(fs, dir, 0, 0, 0, UINT64_MAX, AICAM_FALSE, NULL, NULL);
    uint64_t dt = rtc_get_uptime_ms() - tc;
    if (dt > 5) {  /* only log if suspiciously slow (>5ms) */
        UPLOAD_LOG("count_in_dir(%s)=%u time=%lu ms\r\n", dir, (unsigned)n, (unsigned long)dt);
    }
    return n;
}

/* ==================== Cleanup / wrap-around ==================== */

typedef struct {
    FS_Type_t fs;
    record_state_t state;
    uint64_t target_freed;
    uint64_t freed_so_far;
    aicam_bool_t enough;
} cleanup_ctx_t;

static aicam_result_t cleanup_visitor(FS_Type_t fs, const char *id, void *user)
{
    cleanup_ctx_t *ctx = (cleanup_ctx_t *)user;
    if (ctx->enough) return AICAM_ERROR; /* stop iteration */

    /* Approximate freed size = sum of primary + inference + ai. We don't bother
       reading stat for accuracy; the loop terminates by free-space recheck. */
    char path[128];
    struct stat st;
    uint64_t freed = 0;

    path_for_data(path, sizeof(path), id, 'p');
    if (disk_file_stat(fs, path, &st) == 0) freed += st.st_size;
    path_for_data(path, sizeof(path), id, 'i');
    if (disk_file_stat(fs, path, &st) == 0) freed += st.st_size;
    path_for_data(path, sizeof(path), id, 'a');
    if (disk_file_stat(fs, path, &st) == 0) freed += st.st_size;

    delete_record_files(fs, id, ctx->state);

    ctx->freed_so_far += freed;
    if (ctx->freed_so_far >= ctx->target_freed) ctx->enough = AICAM_TRUE;
    return AICAM_OK;
}

static aicam_result_t cleanup_for_space(FS_Type_t fs, uint64_t need_bytes)
{
    /* Priority order: sent -> local -> failed -> pending */
    const record_state_t order[] = {
        RECORD_STATE_SENT, RECORD_STATE_LOCAL, RECORD_STATE_FAILED, RECORD_STATE_PENDING
    };

    cleanup_ctx_t ctx = { .fs = fs, .target_freed = need_bytes };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        ctx.state = order[i];
        /* Oldest date first (sort_desc=false) so wrap policy evicts the
         * oldest records first. limit=0 = unlimited (chunked flush inside
         * iterate keeps memory bounded); visitor stops via return code. */
        iterate_records(fs, state_dir(order[i]), 0, 0,
                        0, UINT64_MAX, AICAM_FALSE,
                        cleanup_visitor, &ctx);
        if (ctx.enough) break;

        /* Re-check actual free space — visitor's estimate is approximate */
        uint64_t free_bytes = 0, total = 0;
        if (get_storage_free_bytes(fs, &free_bytes, &total) == AICAM_OK &&
            free_bytes >= need_bytes + CLEANUP_HEADROOM_BYTES) {
            return AICAM_OK;
        }
    }
    uint64_t free_bytes = 0, total = 0;
    if (get_storage_free_bytes(fs, &free_bytes, &total) == AICAM_OK &&
        free_bytes >= need_bytes + CLEANUP_HEADROOM_BYTES) {
        return AICAM_OK;
    }
    return AICAM_ERROR;
}

/* Drop sent records older than keep_sent_hours. */
typedef struct {
    FS_Type_t fs;
    uint64_t  cutoff_ts;
} purge_ctx_t;

static aicam_result_t purge_visitor(FS_Type_t fs, const char *id, void *user)
{
    purge_ctx_t *ctx = (purge_ctx_t *)user;
    /* id format: cap_<ts>_<seq> — parse the timestamp portion */
    unsigned long ts = 0;
    if (sscanf(id, "cap_%lu_", &ts) != 1) return AICAM_OK;
    if (ts < ctx->cutoff_ts) {
        delete_record_files(fs, id, RECORD_STATE_SENT);
    }
    return AICAM_OK;
}

static aicam_result_t purge_old_sent(FS_Type_t fs)
{
    uint64_t now = now_unix();
    uint64_t cutoff_secs = (uint64_t)g_up.cfg.keep_sent_hours * 3600u;
    /* keep_sent_hours=0 → cutoff_ts = now → delete all sent.
     * Otherwise cutoff_ts = now - keep window; records older than that are
     * purged. Pass to_ts=cutoff_ts so iterate skips recent date dirs entirely
     * (no readdir of dirs that can't contain purgeable records). */
    uint64_t cutoff_ts = (now > cutoff_secs) ? (now - cutoff_secs) : now;
    purge_ctx_t ctx = { .fs = fs, .cutoff_ts = cutoff_ts };
    iterate_records(fs, CAPTURES_DIR_SENT, 0, 0,
                    0, cutoff_ts, AICAM_FALSE,
                    purge_visitor, &ctx);
    return AICAM_OK;
}

/* ==================== Upload one record ==================== */

/* Shared loader: parse metadata + load JPEG + reconstruct mqtt_image_metadata_t.
 * Used by both the MQTT pipeline path and the webhook serial path. */
static aicam_result_t record_load(FS_Type_t fs, const char *id,
                                   cJSON **out_meta,
                                   uint8_t **out_jpeg, uint32_t *out_jpeg_size,
                                   mqtt_image_metadata_t *out_m,
                                   upload_proto_t *out_proto)
{
    if (!out_meta || !out_jpeg || !out_jpeg_size || !out_m || !out_proto) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    *out_meta = NULL; *out_jpeg = NULL; *out_jpeg_size = 0;

    char meta_path[128];
    path_for_record(meta_path, sizeof(meta_path), CAPTURES_DIR_PENDING, id);
    if (parse_meta_file(fs, meta_path, out_meta) != AICAM_OK) return AICAM_ERROR;

    char img_path[128];
    path_for_data(img_path, sizeof(img_path), id, 'p');
    struct stat st = {0};
    if (disk_file_stat(fs, img_path, &st) != 0 || st.st_size <= 0) {
        cJSON_Delete(*out_meta); *out_meta = NULL;
        return AICAM_ERROR;
    }
    uint8_t *jpeg = (uint8_t *)buffer_calloc(1, (size_t)st.st_size);
    if (!jpeg) { cJSON_Delete(*out_meta); *out_meta = NULL; return AICAM_ERROR_NO_MEMORY; }
    void *fd = disk_file_fopen(fs, img_path, "r");
    if (!fd) { buffer_free(jpeg); cJSON_Delete(*out_meta); *out_meta = NULL; return AICAM_ERROR; }
    int rn = disk_file_fread(fs, fd, jpeg, (size_t)st.st_size);
    disk_file_fclose(fs, fd);
    if (rn != (int)st.st_size) {
        buffer_free(jpeg); cJSON_Delete(*out_meta); *out_meta = NULL;
        return AICAM_ERROR;
    }

    *out_jpeg = jpeg;
    *out_jpeg_size = (uint32_t)st.st_size;

    memset(out_m, 0, sizeof(*out_m));
    cJSON *t;
    t = cJSON_GetObjectItem(*out_meta, "image_id");
    if (t && cJSON_IsString(t)) snprintf(out_m->image_id, sizeof(out_m->image_id), "%s", t->valuestring);
    t = cJSON_GetObjectItem(*out_meta, "timestamp"); if (t) out_m->timestamp = (uint64_t)t->valuedouble;
    t = cJSON_GetObjectItem(*out_meta, "width");     if (t) out_m->width    = (uint32_t)t->valueint;
    t = cJSON_GetObjectItem(*out_meta, "height");    if (t) out_m->height   = (uint32_t)t->valueint;
    t = cJSON_GetObjectItem(*out_meta, "size");      if (t) out_m->size     = (uint32_t)t->valueint;
    t = cJSON_GetObjectItem(*out_meta, "quality");   if (t) out_m->quality  = (uint8_t)t->valueint;
    t = cJSON_GetObjectItem(*out_meta, "format");    if (t) out_m->format   = (mqtt_image_format_t)t->valueint;
    t = cJSON_GetObjectItem(*out_meta, "trigger_type_id");
    if (t) out_m->trigger_type = (aicam_capture_trigger_t)t->valueint;

    *out_proto = UPLOAD_PROTO_MQTT;
    t = cJSON_GetObjectItem(*out_meta, "upload_protocol");
    if (t && cJSON_IsString(t) && strcmp(t->valuestring, "webhook") == 0) *out_proto = UPLOAD_PROTO_WEBHOOK;

    return AICAM_OK;
}

/* Failure path: bump retry_count, write back, maybe promote to failed/. */
static void record_mark_failed(FS_Type_t fs, const char *id, const char *err)
{
    char meta_path[128];
    path_for_record(meta_path, sizeof(meta_path), CAPTURES_DIR_PENDING, id);
    cJSON *meta = NULL;
    if (parse_meta_file(fs, meta_path, &meta) != AICAM_OK) return;

    uint8_t retry = 0;
    cJSON *t = cJSON_GetObjectItem(meta, "retry_count");
    if (t) retry = (uint8_t)t->valueint;
    retry++;
    cJSON_ReplaceItemInObject(meta, "retry_count", cJSON_CreateNumber(retry));
    cJSON_ReplaceItemInObject(meta, "last_error", cJSON_CreateString(err ? err : "upload_failed"));

    char *str = cJSON_PrintUnformatted(meta);
    if (str) {
        write_file(fs, meta_path, (const uint8_t *)str, (uint32_t)strlen(str));
        cJSON_free(str);
    }
    cJSON_Delete(meta);

    if (g_up.cfg.retry_max_attempts > 0 && retry >= g_up.cfg.retry_max_attempts) {
        (void)move_record(fs, id, RECORD_STATE_PENDING, RECORD_STATE_FAILED);
    }
}

/* MQTT pipeline publish: load + publish (no ack wait). On success returns
 * AICAM_OK and *out_msg_id = the MQTT packet id. On failure bumps retry and
 * returns the error. Caller frees nothing — jpeg is freed internally. */
static aicam_result_t record_publish_mqtt(FS_Type_t fs, const char *id, int *out_msg_id)
{
    if (out_msg_id) *out_msg_id = -1;

    cJSON *meta = NULL;
    uint8_t *jpeg = NULL;
    uint32_t jpeg_size = 0;
    mqtt_image_metadata_t m = {0};
    upload_proto_t proto = UPLOAD_PROTO_MQTT;

    aicam_result_t r = record_load(fs, id, &meta, &jpeg, &jpeg_size, &m, &proto);
    if (r != AICAM_OK) return r;

    aicam_result_t result = AICAM_ERROR;
    int msg_id = -1;

    if (mqtt_service_is_running() && mqtt_service_is_connected()) {
        const uint32_t threshold = 1024u * 1024u;
        int rc;
        if (m.size < threshold) {
            rc = mqtt_service_publish_image_with_ai(NULL, jpeg, jpeg_size, &m, NULL);
        } else {
            rc = mqtt_service_publish_image_chunked(NULL, jpeg, jpeg_size, &m, NULL, 100 * 1024);
        }
        if (rc >= 0) {
            msg_id = rc;
            result = AICAM_OK;
        } else {
            result = AICAM_ERROR;
        }
    } else {
        result = AICAM_ERROR_UNAVAILABLE;
    }

    buffer_free(jpeg);
    cJSON_Delete(meta);

    if (result != AICAM_OK) {
        record_mark_failed(fs, id,
            result == AICAM_ERROR_UNAVAILABLE ? "network_unavailable" : "publish_failed");
    } else if (out_msg_id) {
        *out_msg_id = msg_id;
    }
    
    return result;
}

/* Webhook serial path: load + push + wait_pending + move/retry. */
static aicam_result_t upload_one_record_webhook(FS_Type_t fs, const char *id)
{
    cJSON *meta = NULL;
    uint8_t *jpeg = NULL;
    uint32_t jpeg_size = 0;
    mqtt_image_metadata_t m = {0};
    upload_proto_t proto = UPLOAD_PROTO_WEBHOOK;

    aicam_result_t r = record_load(fs, id, &meta, &jpeg, &jpeg_size, &m, &proto);
    if (r != AICAM_OK) return r;

    aicam_result_t result = AICAM_ERROR;
    if (webhook_service_is_enabled()) {
        uint8_t *copy = (uint8_t *)buffer_calloc(1, jpeg_size);
        if (copy) {
            memcpy(copy, jpeg, jpeg_size);
            aicam_result_t pr = webhook_service_push_capture(copy, jpeg_size, &m, NULL);
            if (pr == AICAM_OK) {
                result = (webhook_service_wait_pending(30000) == AICAM_OK) ? AICAM_OK : AICAM_ERROR;
            } else {
                buffer_free(copy);
                result = AICAM_ERROR;
            }
        } else {
            result = AICAM_ERROR_NO_MEMORY;
        }
    } else {
        result = AICAM_ERROR_UNAVAILABLE;
    }

    buffer_free(jpeg);
    cJSON_Delete(meta);

    if (result == AICAM_OK) {
        return move_record(fs, id, RECORD_STATE_PENDING, RECORD_STATE_SENT);
    }
    record_mark_failed(fs, id,
        result == AICAM_ERROR_UNAVAILABLE ? "network_unavailable" : "upload_failed");
    return result;
}

static aicam_result_t upload_one_record(FS_Type_t fs, const char *id)
{
    char meta_path[128];
    path_for_record(meta_path, sizeof(meta_path), CAPTURES_DIR_PENDING, id);

    cJSON *meta = NULL;
    if (parse_meta_file(fs, meta_path, &meta) != AICAM_OK) return AICAM_ERROR;

    /* Load primary image into RAM */
    char img_path[128];
    path_for_data(img_path, sizeof(img_path), id, 'p');
    struct stat st = {0};
    if (disk_file_stat(fs, img_path, &st) != 0 || st.st_size <= 0) {
        cJSON_Delete(meta);
        return AICAM_ERROR;
    }
    uint8_t *jpeg = (uint8_t *)buffer_calloc(1, (size_t)st.st_size);
    if (!jpeg) { cJSON_Delete(meta); return AICAM_ERROR_NO_MEMORY; }
    void *fd = disk_file_fopen(fs, img_path, "r");
    if (!fd) { buffer_free(jpeg); cJSON_Delete(meta); return AICAM_ERROR; }
    int rn = disk_file_fread(fs, fd, jpeg, (size_t)st.st_size);
    disk_file_fclose(fs, fd);
    if (rn != (int)st.st_size) {
        buffer_free(jpeg); cJSON_Delete(meta);
        return AICAM_ERROR;
    }

    /* Reconstruct mqtt_image_metadata_t from JSON */
    mqtt_image_metadata_t m = {0};
    cJSON *t;
    t = cJSON_GetObjectItem(meta, "image_id");
    if (t && cJSON_IsString(t)) snprintf(m.image_id, sizeof(m.image_id), "%s", t->valuestring);
    t = cJSON_GetObjectItem(meta, "timestamp"); if (t) m.timestamp = (uint64_t)t->valuedouble;
    t = cJSON_GetObjectItem(meta, "width");     if (t) m.width    = (uint32_t)t->valueint;
    t = cJSON_GetObjectItem(meta, "height");    if (t) m.height   = (uint32_t)t->valueint;
    t = cJSON_GetObjectItem(meta, "size");      if (t) m.size     = (uint32_t)t->valueint;
    t = cJSON_GetObjectItem(meta, "quality");   if (t) m.quality  = (uint8_t)t->valueint;
    t = cJSON_GetObjectItem(meta, "format");    if (t) m.format   = (mqtt_image_format_t)t->valueint;
    t = cJSON_GetObjectItem(meta, "trigger_type_id");
    if (t) m.trigger_type = (aicam_capture_trigger_t)t->valueint;

    /* Determine protocol from record (frozen at capture time) */
    upload_proto_t proto = UPLOAD_PROTO_MQTT;
    t = cJSON_GetObjectItem(meta, "upload_protocol");
    if (t && cJSON_IsString(t) && strcmp(t->valuestring, "webhook") == 0) proto = UPLOAD_PROTO_WEBHOOK;

    aicam_result_t result = AICAM_ERROR;

    if (proto == UPLOAD_PROTO_MQTT) {
        /* INSTANT mode may fire before the 4G/WiFi link + MQTT broker
         * connection are fully established. Wait (up to a budget) instead
         * of failing immediately — the record is already persisted to
         * pending/ and we'd rather get it out now than leave a retry
         * backlog. Matches the 40 s timeout historically used by
         * system_service for the same purpose.
         *
         * Wake fast-fail: also listen for SERVICE_READY_NETIF_ALL_FAILED so
         * that when all attempted netifs fail (e.g. selected 4G with no SIM),
         * we break out immediately instead of waiting the full budget. */
        if (!mqtt_service_is_running() || !mqtt_service_is_connected()) {
            uint32_t wait_ms = 30000;
            UPLOAD_LOG("upload_one_record: MQTT not ready, waiting up to %lu ms...\r\n",
                   (unsigned long)wait_ms);
            /* Wait for link-up OR all-failed (whichever first). */
            (void)service_wait_for_ready(
                SERVICE_READY_STA | SERVICE_READY_NETIF_ALL_FAILED,
                AICAM_FALSE, wait_ms);
            if (service_get_ready_flags() & SERVICE_READY_NETIF_ALL_FAILED) {
                UPLOAD_LOG("upload_one_record: all netifs failed, aborting upload\r\n");
                buffer_free(jpeg);
                cJSON_Delete(meta);
                return AICAM_ERROR_UNAVAILABLE;
            }
            if (mqtt_service_is_running()) {
                (void)mqtt_service_wait_for_event(MQTT_EVENT_CONNECTED, AICAM_FALSE, wait_ms);
            }
        }
        if (mqtt_service_is_running() && mqtt_service_is_connected()) {
            const uint32_t threshold = 1024u * 1024u;
            int rc;
            if (m.size < threshold) {
                rc = mqtt_service_publish_image_with_ai(NULL, jpeg, m.size, &m, NULL);
            } else {
                rc = mqtt_service_publish_image_chunked(NULL, jpeg, m.size, &m, NULL, 100 * 1024);
            }
            if (rc >= 0) {
                /* publish_image_with_ai just queues the message — wait for the
                 * MQTT puback so we know the broker actually received it.
                 * Without this, the last record of a flush pass can be lost
                 * when we sleep and tear down the network immediately after
                 * publish. Dynamic timeout matches system_service Step 6. */
                uint32_t puback_timeout = 5000 + (m.size / 10240) * 2000;
                if (puback_timeout > 60000) puback_timeout = 60000;
                if (mqtt_service_wait_for_event(MQTT_EVENT_PUBLISHED, AICAM_TRUE, puback_timeout) == AICAM_OK) {
                    result = AICAM_OK;
                } else {
                    result = AICAM_ERROR;   /* puback timeout → leave in pending for retry */
                }
            } else {
                result = AICAM_ERROR;
            }
        } else {
            result = AICAM_ERROR_UNAVAILABLE;
        }
    } else {
        /* Webhook path — keep MVP simple: call existing async push and wait. */
        if (webhook_service_is_enabled()) {
            /* webhook task takes ownership of the buffer, allocate a fresh copy */
            uint8_t *copy = (uint8_t *)buffer_calloc(1, m.size);
            if (copy) {
                memcpy(copy, jpeg, m.size);
                aicam_result_t pr = webhook_service_push_capture(copy, m.size, &m, NULL);
                if (pr == AICAM_OK) {
                    /* Wait up to 30s for the push to complete */
                    aicam_result_t wr = webhook_service_wait_pending(30000);
                    result = (wr == AICAM_OK) ? AICAM_OK : AICAM_ERROR;
                } else {
                    buffer_free(copy);
                    result = AICAM_ERROR;
                }
            } else {
                result = AICAM_ERROR_NO_MEMORY;
            }
        } else {
            result = AICAM_ERROR_UNAVAILABLE;
        }
    }

    buffer_free(jpeg);

    if (result == AICAM_OK) {
        cJSON_Delete(meta);
        return move_record(fs, id, RECORD_STATE_PENDING, RECORD_STATE_SENT);
    }

    /* Failure path: bump retry_count, optionally promote to failed/ */
    uint8_t retry = 0;
    t = cJSON_GetObjectItem(meta, "retry_count");
    if (t) retry = (uint8_t)t->valueint;
    retry++;
    cJSON_ReplaceItemInObject(meta, "retry_count", cJSON_CreateNumber(retry));
    cJSON_ReplaceItemInObject(meta, "last_error", cJSON_CreateString(
        result == AICAM_ERROR_UNAVAILABLE ? "network_unavailable" : "upload_failed"));

    /* Persist updated meta */
    char *str = cJSON_PrintUnformatted(meta);
    if (str) {
        write_file(fs, meta_path, (const uint8_t *)str, (uint32_t)strlen(str));
        cJSON_free(str);
    }
    cJSON_Delete(meta);

    /* 0 = unlimited retries — never promote to failed/ */
    if (g_up.cfg.retry_max_attempts > 0 && retry >= g_up.cfg.retry_max_attempts) {
        return move_record(fs, id, RECORD_STATE_PENDING, RECORD_STATE_FAILED);
    }
    return result;
}

/* ==================== Worker pass ==================== */

typedef struct {
    FS_Type_t fs;
    int       attempts;
    int       successes;
    int       failures;
} flush_ctx_t;

/* Visitor: collect record ids into an array. Used by do_flush_pass to snapshot
 * pending ids before publishing — iterate_records traverses date subdirs
 * (direct readdir of /captures/pending only returns date dirs, not .json). */
typedef struct { char (*ids)[64]; uint32_t n; uint32_t cap; } collect_ctx_t;
static aicam_result_t collect_visitor(FS_Type_t fs, const char *id, void *user)
{
    collect_ctx_t *ctx = (collect_ctx_t *)user;
    if (ctx->n < ctx->cap) {
        snprintf(ctx->ids[ctx->n], 64, "%s", id);
        ctx->n++;
    }
    return AICAM_OK;
}

static aicam_result_t flush_visitor(FS_Type_t fs, const char *id, void *user)
{
    flush_ctx_t *ctx = (flush_ctx_t *)user;
    ctx->attempts++;
    aicam_result_t r = upload_one_record(fs, id);
    if (r == AICAM_OK) ctx->successes++;
    else               ctx->failures++;
    return AICAM_OK;
}

/* Check whether the configured upload channel is currently reachable.
 * We only sweep pending/ when this returns true — retrying with the network
 * down just burns IO and battery for no gain. */
static aicam_bool_t upload_channel_ready(void)
{
    if (g_up.cfg.upload_protocol == UPLOAD_PROTO_WEBHOOK) {
        return webhook_service_is_enabled() ? AICAM_TRUE : AICAM_FALSE;
    }
    /* MQTT: need both the service running and an active broker connection */
    return (mqtt_service_is_running() && mqtt_service_is_connected()) ? AICAM_TRUE : AICAM_FALSE;
}

static void do_flush_pass(void)
{
    if (g_up.active_fs == FS_MAX) return;
    if (!upload_channel_ready()) {
        UPLOAD_LOG("flush skipped, channel not ready\r\n");
        return;
    }

    g_up.flush_active = AICAM_TRUE;
    FS_Type_t fs = g_up.active_fs;

    /* Phase 1: collect all pending record ids into an array WITHOUT touching
     * the directory. FileX's readdir cursor gets corrupted if the directory is
     * modified (rename/remove) mid-iteration, causing later entries to be
     * skipped — that's why we previously saw pending=7 but attempts=1. */
    uint32_t cap = count_in_dir(fs, CAPTURES_DIR_PENDING);
    if (cap == 0) {
        purge_old_sent(fs);
        return;
    }
    char (*ids)[64] = (char (*)[64])buffer_calloc(cap, 64);
    if (!ids) {
        purge_old_sent(fs);
        return;
    }
    /* Collect pending ids via iterate_records — it traverses date subdirs
     * (direct readdir of /captures/pending returns date dirs, not .json files,
     * so the old approach collected 0 records after the date-subdir refactor). */
    collect_ctx_t cctx = { .ids = ids, .n = 0, .cap = cap };
    iterate_records(fs, CAPTURES_DIR_PENDING, 0, cap,
                    0, UINT64_MAX, AICAM_FALSE,
                    collect_visitor, &cctx);
    uint32_t n = cctx.n;

    /* Phase 2: process each id. Now it's safe to rename/remove — the dir is
     * closed and we're walking our own snapshot. */
    flush_ctx_t ctx = { .fs = fs };

    if (g_up.cfg.upload_protocol == UPLOAD_PROTO_MQTT) {
        /* MQTT pipeline: batch-publish all records (no ack wait), then drain
         * acks from mqtt_service_get_acked_msg_id and match by msg_id. This
         * is much faster than serial publish→ack per record, and per-msg_id
         * tracking means we know exactly which upload failed → retry only
         * that one next flush. */
        typedef struct { char id[64]; int msg_id; } pub_entry_t;
        pub_entry_t *pub = (pub_entry_t *)buffer_calloc(n, sizeof(pub_entry_t));
        if (pub) {
            uint32_t n_pub = 0;
            /* 2a: publish all */
            for (uint32_t i = 0; i < n; i++) {
                ctx.attempts++;
                int mid = -1;
                if (record_publish_mqtt(fs, ids[i], &mid) == AICAM_OK) {
                    snprintf(pub[n_pub].id, sizeof(pub[n_pub].id), "%s", ids[i]);
                    pub[n_pub].msg_id = mid;
                    n_pub++;
                } else {
                    ctx.failures++;
                }
            }
            /* 2b: drain acks, match by msg_id, move to sent */
            uint32_t ack_timeout_ms = 10000;  /* per-ack wait; acks usually arrive fast */
            uint32_t acked = 0;
            while (acked < n_pub) {
                int acked_mid = 0;
                aicam_result_t ar = mqtt_service_get_acked_msg_id(&acked_mid, ack_timeout_ms);
                if (ar != AICAM_OK) break;   /* no more acks in time */
                if (acked_mid == -1) {
                    /* QoS 0: all publishes are considered confirmed */
                    for (uint32_t i = 0; i < n_pub; i++) {
                        if (pub[i].id[0] != '\0') {
                            (void)move_record(fs, pub[i].id, RECORD_STATE_PENDING, RECORD_STATE_SENT);
                            ctx.successes++;
                            pub[i].id[0] = '\0';
                            acked++;
                        }
                    }
                    break;
                }
                /* find the published record with this msg_id */
                for (uint32_t i = 0; i < n_pub; i++) {
                    if (pub[i].msg_id == acked_mid && pub[i].id[0] != '\0') {
                        (void)move_record(fs, pub[i].id, RECORD_STATE_PENDING, RECORD_STATE_SENT);
                        ctx.successes++;
                        pub[i].id[0] = '\0';  /* consumed */
                        acked++;
                        break;
                    }
                }
            }
            /* 2c: published but not acked within timeout → mark failed (retry) */
            for (uint32_t i = 0; i < n_pub; i++) {
                if (pub[i].id[0] != '\0') {
                    record_mark_failed(fs, pub[i].id, "ack_timeout");
                    ctx.failures++;
                }
            }
            buffer_free(pub);
        } else {
            /* OOM fallback: serial */
            for (uint32_t i = 0; i < n; i++) {
                ctx.attempts++;
                int mid = -1;
                if (record_publish_mqtt(fs, ids[i], &mid) == AICAM_OK) {
                    int am = 0;
                    if (mqtt_service_get_acked_msg_id(&am, 10000) == AICAM_OK) {
                        (void)move_record(fs, ids[i], RECORD_STATE_PENDING, RECORD_STATE_SENT);
                        ctx.successes++;
                    } else {
                        record_mark_failed(fs, ids[i], "ack_timeout");
                        ctx.failures++;
                    }
                } else {
                    ctx.failures++;
                }
            }
        }
    } else {
        /* Webhook: serial (push + wait_pending + move per record) */
        for (uint32_t i = 0; i < n; i++) {
            ctx.attempts++;
            if (upload_one_record_webhook(fs, ids[i]) == AICAM_OK) ctx.successes++;
            else ctx.failures++;
        }
    }

    buffer_free(ids);

    if (ctx.attempts > 0) {
        UPLOAD_LOG("flush pass attempts=%d ok=%d fail=%d\r\n",
               ctx.attempts, ctx.successes, ctx.failures);
        LOG_SVC_INFO("upload: flush pass attempts=%d ok=%d fail=%d",
                     ctx.attempts, ctx.successes, ctx.failures);
    }
    /* Housekeeping at the tail of every pass */
    purge_old_sent(fs);
}

static void upload_task(void *arg)
{
    (void)arg;
    UPLOAD_LOG("coordinator task started\r\n");
    LOG_SVC_INFO("upload coordinator task started");

    while (g_up.running) {
        upload_event_t ev = 0;
        osStatus_t st = osMessageQueueGet(g_up.queue, &ev, NULL, osWaitForever);
        if (st != osOK) continue;
        if (!g_up.running) break;

        switch ((upload_event_kind_t)ev) {
        case EV_KICK:
            /* Flush pass is mutex-guarded so it won't race with a synchronous
             * upload_coordinator_drain() call from the sleep path. */
            if (osMutexAcquire(g_up.mutex, osWaitForever) == osOK) {
                do_flush_pass();
                g_up.flush_active = AICAM_FALSE;
                osMutexRelease(g_up.mutex);
            }
            break;
        case EV_RELOAD:
            json_config_get_capture_upload_config(&g_up.cfg);
            g_up.active_fs = resolve_storage_target(g_up.cfg.storage);
            if (g_up.active_fs != FS_MAX) ensure_dirs_with_space_check(g_up.active_fs);
            UPLOAD_LOG("config reloaded (mode=%d storage=%d fs=%d)\r\n",
                   g_up.cfg.mode, g_up.cfg.storage, g_up.active_fs);
            LOG_SVC_INFO("upload: config reloaded (mode=%d storage=%d)",
                         g_up.cfg.mode, g_up.cfg.storage);
            break;
        case EV_SHUTDOWN:
            g_up.running = AICAM_FALSE;
            break;
        case EV_DRAIN:
            /* Handled synchronously by upload_coordinator_drain() — no queue path. */
            break;
        default: break;
        }
    }
    LOG_SVC_INFO("upload coordinator task exited");
}

/* ==================== Service Lifecycle ==================== */

aicam_result_t upload_coordinator_init(void *config)
{
    (void)config;
    uint64_t t0 = rtc_get_uptime_ms();
    memset(&g_up, 0, sizeof(g_up));
    g_up.state = SERVICE_STATE_UNINITIALIZED;

    g_up.mutex = osMutexNew(NULL);
    if (!g_up.mutex) {
        LOG_SVC_ERROR("upload_coordinator init: osMutexNew failed");
        return AICAM_ERROR;
    }

    g_up.queue = osMessageQueueNew(UPLOAD_QUEUE_DEPTH, sizeof(ULONG), &upload_queue_attr);
    if (!g_up.queue) {
        LOG_SVC_ERROR("upload_coordinator init: osMessageQueueNew failed");
        osMutexDelete(g_up.mutex); g_up.mutex = NULL;
        return AICAM_ERROR;
    }
    uint64_t t1 = rtc_get_uptime_ms();

    json_config_capture_upload_defaults(&g_up.cfg);
    json_config_get_capture_upload_config(&g_up.cfg);
    uint64_t t2 = rtc_get_uptime_ms();

    g_up.active_fs = resolve_storage_target(g_up.cfg.storage);
    uint64_t t3 = rtc_get_uptime_ms();

    if (g_up.active_fs != FS_MAX) {
        ensure_dirs_with_space_check(g_up.active_fs);
    }
    uint64_t t4 = rtc_get_uptime_ms();

    g_up.initialized = AICAM_TRUE;
    g_up.state = SERVICE_STATE_INITIALIZED;
    UPLOAD_LOG("init ok (mode=%d storage=%d fs=%d) times: mutex/queue=%lu cfg=%lu resolve=%lu dirs=%lu total=%lu ms\r\n",
           g_up.cfg.mode, g_up.cfg.storage, g_up.active_fs,
           (unsigned long)(t1-t0), (unsigned long)(t2-t1), (unsigned long)(t3-t2),
           (unsigned long)(t4-t3), (unsigned long)(t4-t0));
    return AICAM_OK;
}

aicam_result_t upload_coordinator_start(void)
{
    UPLOAD_LOG("start() called, initialized=%d running=%d\r\n",
           (int)g_up.initialized, (int)g_up.running);
    if (!g_up.initialized) return AICAM_ERROR_NOT_INITIALIZED;
    if (g_up.running) return AICAM_OK;

    g_up.running = AICAM_TRUE;
    osThreadAttr_t attr = {
        .name = "upload_co",
        .stack_size = UPLOAD_TASK_STACK,
        .stack_mem = upload_task_stack,
        .priority = UPLOAD_TASK_PRIORITY,
    };
    g_up.task_handle = osThreadNew(upload_task, NULL, &attr);
    UPLOAD_LOG("osThreadNew returned %p\r\n", (void*)g_up.task_handle);
    if (!g_up.task_handle) {
        g_up.running = AICAM_FALSE;
        UPLOAD_LOG("start FAILED — osThreadNew returned NULL\r\n");
        return AICAM_ERROR;
    }
    g_up.state = SERVICE_STATE_RUNNING;

    /* Initial sweep: any leftover pending from prior boot can flush now. */
    upload_coordinator_kick();
    UPLOAD_LOG("start ok, state=RUNNING\r\n");
    return AICAM_OK;
}

aicam_result_t upload_coordinator_stop(void)
{
    if (!g_up.running) return AICAM_OK;

    upload_event_t ev = EV_SHUTDOWN;
    osMessageQueuePut(g_up.queue, &ev, 0, 0);

    if (g_up.task_handle) {
        osThreadJoin(g_up.task_handle);
        osThreadTerminate(g_up.task_handle);
        g_up.task_handle = NULL;
    }
    g_up.running = AICAM_FALSE;
    g_up.state = SERVICE_STATE_INITIALIZED;
    return AICAM_OK;
}

aicam_result_t upload_coordinator_deinit(void)
{
    if (g_up.running) upload_coordinator_stop();
    if (g_up.queue) { osMessageQueueDelete(g_up.queue); g_up.queue = NULL; }
    if (g_up.mutex) { osMutexDelete(g_up.mutex); g_up.mutex = NULL; }
    g_up.initialized = AICAM_FALSE;
    g_up.state = SERVICE_STATE_UNINITIALIZED;
    return AICAM_OK;
}

service_state_t upload_coordinator_get_state(void) { return g_up.state; }

/* ==================== Public Operations ==================== */

communication_type_t upload_coordinator_get_required_comm_type(void)
{
    /* Only restrict the netif when the capture actually uploads. LOCAL_ONLY
     * never uploads; storage=NONE + INSTANT has no retry and uploads direct,
     * so it still needs a netif ( honour the setting there too). */
    if (g_up.cfg.mode == CAPTURE_MODE_LOCAL_ONLY) return COMM_TYPE_NONE;
    return (communication_type_t)g_up.cfg.upload_comm_type;
}

aicam_result_t upload_coordinator_kick(void)
{
    if (!g_up.running) return AICAM_ERROR_NOT_INITIALIZED;
    upload_event_t ev = EV_KICK;
    osStatus_t st = osMessageQueuePut(g_up.queue, &ev, 0, 0);
    return (st == osOK) ? AICAM_OK : AICAM_ERROR;
}

aicam_bool_t upload_coordinator_needs_network(void)
{
    uint64_t tn0 = rtc_get_uptime_ms();
    /* If coordinator isn't up yet, be conservative: bring up the network.
     * This only happens during the very first boot before init runs. */
    if (!g_up.initialized || g_up.active_fs == FS_MAX) {
        UPLOAD_LOG("needs_network=YES (not initialized)\r\n");
        return AICAM_TRUE;
    }

    /* Counting pending/failed is only needed for the BATCH threshold decision.
     * INSTANT always uploads, LOCAL_ONLY never, SCHEDULED only at the flush
     * node (wake_scheduler decides) — skip the directory traverses for those
     * modes to cut wake latency. */
    capture_mode_t mode = g_up.cfg.mode;
    uint32_t pending = 0, failed = 0;
    if (mode == CAPTURE_MODE_BATCH) {
        pending = count_in_dir(g_up.active_fs, CAPTURES_DIR_PENDING);
        failed  = count_in_dir(g_up.active_fs, CAPTURES_DIR_FAILED);
    }
    uint64_t tn1 = rtc_get_uptime_ms();
    aicam_bool_t decision = AICAM_TRUE;

    switch (mode) {
    case CAPTURE_MODE_LOCAL_ONLY:
        /* Never uploads — no network needed. */
        decision = AICAM_FALSE;
        break;

    case CAPTURE_MODE_INSTANT:
        /* Snap-and-upload: every capture goes out immediately. */
        decision = AICAM_TRUE;
        break;

    case CAPTURE_MODE_BATCH:
        /* Bring up network only when this capture would cross the threshold
         * (pending + 1 >= batch_count) or there's a failed backlog to retry.
         * Otherwise just enqueue and go back to sleep — saves the multi-second
         * network bring-up. */
        if (failed > 0) {
            decision = AICAM_TRUE;
        } else {
            decision = (pending + 1 >= g_up.cfg.batch_count) ? AICAM_TRUE : AICAM_FALSE;
        }
        break;

    case CAPTURE_MODE_SCHEDULED: {
        /* Network ONLY at the scheduled upload-flush node. pending/failed
         * backlog waits for the next flush node — bringing the network up on
         * every capture-only wake just because there's a backlog wastes multi-
         * seconds of bring-up time + battery. A pure capture wake (timer only,
         * no upload node due) just enqueues and sleeps. */
        uint64_t now = now_unix();
        wake_event_t evs[WAKE_DUTY_MAX];
        int n = wake_scheduler_due_events(
            now > WAKE_TOLERANCE_SEC ? now - WAKE_TOLERANCE_SEC : 0,
            now + WAKE_TOLERANCE_SEC,
            evs, WAKE_DUTY_MAX);
        decision = AICAM_FALSE;
        for (int i = 0; i < n; i++) {
            if (evs[i].duty == WAKE_DUTY_UPLOAD_FLUSH) { decision = AICAM_TRUE; break; }
        }
        break;
    }

    default:
        decision = AICAM_TRUE;
        break;
    }

    UPLOAD_LOG("needs_network=%d (mode=%d pending=%u failed=%u batch=%u) count_time=%lu ms\r\n",
           (int)decision, (int)g_up.cfg.mode,
           (unsigned)pending, (unsigned)failed, (unsigned)g_up.cfg.batch_count,
           (unsigned long)(tn1 - tn0));
    return decision;
}

/* Wait for the upload channel (MQTT broker connection or webhook link) to be
 * ready, with fast-fail on all-netifs-failed. Used by drain before flushing —
 * on a wake, wifi is brought up async in service_start, and by the time drain
 * runs (after capture) MQTT may still be connecting. Without this wait,
 * do_flush_pass sees channel-not-ready and skips, so the scheduled upload
 * never fires. Mirrors the wait in upload_one_record/direct_publish_capture. */
static aicam_result_t wait_for_upload_channel(uint32_t timeout_ms)
{
    if (g_up.cfg.upload_protocol == UPLOAD_PROTO_WEBHOOK) {
        if (!webhook_service_is_enabled()) return AICAM_ERROR_UNAVAILABLE;
        if (!(service_get_ready_flags() & SERVICE_READY_STA)) {
            (void)service_wait_for_ready(
                SERVICE_READY_STA | SERVICE_READY_NETIF_ALL_FAILED,
                AICAM_FALSE, timeout_ms);
            if (service_get_ready_flags() & SERVICE_READY_NETIF_ALL_FAILED)
                return AICAM_ERROR_UNAVAILABLE;
        }
        return AICAM_OK;
    }
    /* MQTT */
    if (mqtt_service_is_running() && mqtt_service_is_connected()) return AICAM_OK;
    UPLOAD_LOG("drain: MQTT not ready, waiting up to %lu ms...\r\n",
           (unsigned long)timeout_ms);
    (void)service_wait_for_ready(
        SERVICE_READY_STA | SERVICE_READY_NETIF_ALL_FAILED,
        AICAM_FALSE, timeout_ms);
    if (service_get_ready_flags() & SERVICE_READY_NETIF_ALL_FAILED) {
        UPLOAD_LOG("drain: all netifs failed, aborting\r\n");
        return AICAM_ERROR_UNAVAILABLE;
    }
    if (mqtt_service_is_running()) {
        (void)mqtt_service_wait_for_event(MQTT_EVENT_CONNECTED, AICAM_FALSE, timeout_ms);
    }
    return (mqtt_service_is_running() && mqtt_service_is_connected())
           ? AICAM_OK : AICAM_ERROR_TIMEOUT;
}

aicam_result_t upload_coordinator_drain(uint32_t timeout_ms)
{
    if (!g_up.initialized) return AICAM_ERROR_NOT_INITIALIZED;
    /* Synchronous: run a flush pass on the caller's thread, guarded by the
     * same mutex the worker uses for EV_KICK. This avoids osEventFlags (which
     * is unreliable on this ThreadX port) and guarantees the caller that
     * pending/ has been swept before we return. */
    if (osMutexAcquire(g_up.mutex, timeout_ms) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    /* Wait for the channel before flushing — wifi is brought up async and may
     * still be connecting. Without this, do_flush_pass skips on "channel not
     * ready" and the scheduled upload is silently dropped. */
    if (wait_for_upload_channel(timeout_ms) == AICAM_OK) {
        do_flush_pass();
    } else {
        UPLOAD_LOG("drain: channel not ready after wait, skipping flush\r\n");
    }
    g_up.flush_active = AICAM_FALSE;
    osMutexRelease(g_up.mutex);
    return AICAM_OK;
}

aicam_bool_t upload_coordinator_is_flushing(void)
{
    return g_up.flush_active;
}

aicam_result_t upload_coordinator_reload_config(void)
{
    if (!g_up.initialized) return AICAM_ERROR_NOT_INITIALIZED;
    if (!g_up.running) {
        /* Apply synchronously when worker isn't running */
        json_config_get_capture_upload_config(&g_up.cfg);
        g_up.active_fs = resolve_storage_target(g_up.cfg.storage);
        if (g_up.active_fs != FS_MAX) ensure_dirs_with_space_check(g_up.active_fs);
        wake_scheduler_invalidate();
        return AICAM_OK;
    }
    upload_event_t ev = EV_RELOAD;
    osMessageQueuePut(g_up.queue, &ev, 0, 0);
    wake_scheduler_invalidate();
    return AICAM_OK;
}

/* ==================== Enqueue ==================== */

/* Direct in-memory upload (no persistence). Used by INSTANT+NONE and as a
 * fallback when INSTANT+storage can't persist (storage full). On success
 * returns AICAM_OK; on failure the capture is lost (no retry possible without
 * persistence). Waits for network readiness with fast-fail on all-netifs-failed. */
static aicam_result_t direct_publish_capture(const uint8_t *jpeg_buffer,
                                             uint32_t jpeg_size,
                                             const mqtt_image_metadata_t *meta_in)
{
    aicam_result_t r = AICAM_ERROR;
    uint32_t wait_ms = 30000;

    if (g_up.cfg.upload_protocol == UPLOAD_PROTO_MQTT) {
        if (!mqtt_service_is_running() || !mqtt_service_is_connected()) {
            UPLOAD_LOG("direct publish: MQTT not ready, waiting up to %lu ms...\r\n",
                   (unsigned long)wait_ms);
            (void)service_wait_for_ready(
                SERVICE_READY_STA | SERVICE_READY_NETIF_ALL_FAILED,
                AICAM_FALSE, wait_ms);
            if (service_get_ready_flags() & SERVICE_READY_NETIF_ALL_FAILED) {
                UPLOAD_LOG("direct publish: all netifs failed, aborting\r\n");
                return AICAM_ERROR_UNAVAILABLE;
            }
            if (mqtt_service_is_running()) {
                (void)mqtt_service_wait_for_event(MQTT_EVENT_CONNECTED, AICAM_FALSE, wait_ms);
            }
        }
        if (mqtt_service_is_running() && mqtt_service_is_connected()) {
            const uint32_t threshold = 1024u * 1024u;
            int rc = (jpeg_size < threshold)
                ? mqtt_service_publish_image_with_ai(NULL, jpeg_buffer, jpeg_size, meta_in, NULL)
                : mqtt_service_publish_image_chunked(NULL, jpeg_buffer, jpeg_size, meta_in, NULL, 100 * 1024);
            if (rc >= 0) {
                uint32_t puback_timeout = 5000 + (jpeg_size / 10240) * 2000;
                if (puback_timeout > 60000) puback_timeout = 60000;
                r = (mqtt_service_wait_for_event(MQTT_EVENT_PUBLISHED, AICAM_TRUE, puback_timeout) == AICAM_OK)
                    ? AICAM_OK : AICAM_ERROR;
            }
        }
    } else if (g_up.cfg.upload_protocol == UPLOAD_PROTO_WEBHOOK) {
        UPLOAD_LOG("direct publish: network not ready, waiting up to %lu ms...\r\n",
               (unsigned long)wait_ms);
        (void)service_wait_for_ready(
            SERVICE_READY_STA | SERVICE_READY_NETIF_ALL_FAILED,
            AICAM_FALSE, wait_ms);
        if (service_get_ready_flags() & SERVICE_READY_NETIF_ALL_FAILED) {
            UPLOAD_LOG("direct publish: all netifs failed, aborting\r\n");
            return AICAM_ERROR_UNAVAILABLE;
        }
        if (webhook_service_is_enabled()) {
            uint8_t *copy = (uint8_t *)buffer_calloc(1, jpeg_size);
            if (copy) {
                memcpy(copy, jpeg_buffer, jpeg_size);
                aicam_result_t pr = webhook_service_push_capture(copy, jpeg_size, meta_in, NULL);
                if (pr == AICAM_OK) {
                    r = webhook_service_wait_pending(30000);
                } else {
                    buffer_free(copy);
                }
            }
        }
    }
    if (r == AICAM_OK) LOG_SVC_INFO("upload ok (direct): %s", meta_in->image_id);
    return r;
}

aicam_result_t upload_coordinator_enqueue_capture(
    const uint8_t *jpeg_buffer, uint32_t jpeg_size,
    const uint8_t *inference_jpeg, uint32_t inference_jpeg_size,
    const char *ai_result_json,
    const mqtt_image_metadata_t *meta_in,
    aicam_capture_trigger_t trigger,
    wakeup_source_type_t wakeup_src)
{
    if (!jpeg_buffer || jpeg_size == 0 || !meta_in) return AICAM_ERROR_INVALID_PARAM;
    if (!g_up.initialized) return AICAM_ERROR_NOT_INITIALIZED;

    /* Build record id: cap_<timestamp>_<seq>, both zero-padded for correct
     * lexicographic ordering (cleanup deletes oldest by filename sort).
     * Avoid PRIu64 — on this toolchain it prints "lu" literally; %lu with
     * (unsigned long) is safe for timestamps < 2038. */
    uint64_t ts = meta_in->timestamp ? meta_in->timestamp : now_unix();
    uint32_t seq = next_seq();
    char id[64];
    snprintf(id, sizeof(id), "cap_%08lu_%05u", (unsigned long)ts, (unsigned)seq);

    capture_mode_t mode = g_up.cfg.mode;
    FS_Type_t fs = g_up.active_fs;

    /* INSTANT + storage=NONE → in-memory upload only, no persistence needed. */
    if (mode == CAPTURE_MODE_INSTANT && g_up.cfg.storage == CAPTURE_STORE_NONE) {
        return direct_publish_capture(jpeg_buffer, jpeg_size, meta_in);
    }

    /* All persistent modes (LOCAL_ONLY / BATCH / SCHEDULED / INSTANT-with-storage)
     * need a real FS and a space budget check. We MUST check free space BEFORE
     * writing — otherwise littlefs on a full flash will trigger GC/compact
     * internally and hang for tens of seconds trying to make room. */
    if (fs == FS_MAX) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    /* SD media is opened asynchronously by sdProcess; on a cold boot the first
     * capture may arrive before fx_media_open completes. Wait (up to
     * SD_STORE_READY_TIMEOUT_MS) so store-only captures actually get persisted
     * instead of failing with "write primary image failed". For FS_FLASH the
     * littlefs is mounted synchronously during storage_init, so no wait needed. */
    if (fs == FS_SD) {
        if (sd_wait_ready_for_open(SD_STORE_READY_TIMEOUT_MS) != AICAM_OK) {
            LOG_SVC_ERROR("upload: SD media not ready after %u ms, cannot store capture",
                          SD_STORE_READY_TIMEOUT_MS);
            return AICAM_ERROR_TIMEOUT;
        }
    }

    {
        uint64_t need = (uint64_t)jpeg_size + (uint64_t)inference_jpeg_size +
                        (ai_result_json ? strlen(ai_result_json) : 0) + 2048u;
        uint64_t free_bytes = 0, total = 0;
        if (get_storage_free_bytes(fs, &free_bytes, &total) == AICAM_OK &&
            free_bytes < need + CLEANUP_HEADROOM_BYTES) {
            if (g_up.cfg.policy == STORAGE_POLICY_WRAP) {
                if (cleanup_for_space(fs, need + CLEANUP_HEADROOM_BYTES) != AICAM_OK) {
                    UPLOAD_LOG("storage full (free=%luKB need=%luKB)\r\n",
                           (unsigned long)(free_bytes / 1024),
                           (unsigned long)((need + CLEANUP_HEADROOM_BYTES) / 1024));
                    g_up.storage_full = AICAM_TRUE;
                    /* INSTANT = 即拍即传: even with no space to persist, the JPEG
                     * is still in RAM — attempt a direct upload rather than dropping
                     * the capture. Failure is final (no retry without persistence). */
                    if (mode == CAPTURE_MODE_INSTANT) {
                        UPLOAD_LOG("storage full — INSTANT fallback to direct publish\r\n");
                        return direct_publish_capture(jpeg_buffer, jpeg_size, meta_in);
                    }
                    return AICAM_ERROR_NO_MEMORY;
                }
            } else {
                UPLOAD_LOG("storage full (STOP policy)\r\n");
                g_up.storage_full = AICAM_TRUE;
                if (mode == CAPTURE_MODE_INSTANT) {
                    UPLOAD_LOG("storage full (STOP) — INSTANT fallback to direct publish\r\n");
                    return direct_publish_capture(jpeg_buffer, jpeg_size, meta_in);
                }
                return AICAM_ERROR_NO_MEMORY;
            }
        }
        g_up.storage_full = AICAM_FALSE;
    }

    /* LOCAL_ONLY → persist into local/, no upload (space already checked above) */
    if (mode == CAPTURE_MODE_LOCAL_ONLY) {
        return persist_record(fs, id, jpeg_buffer, jpeg_size,
                              inference_jpeg, inference_jpeg_size,
                              ai_result_json, meta_in, trigger, wakeup_src,
                              RECORD_STATE_LOCAL);
    }
    aicam_result_t pr = persist_record(fs, id, jpeg_buffer, jpeg_size,
                                        inference_jpeg, inference_jpeg_size,
                                        ai_result_json, meta_in, trigger, wakeup_src,
                                        RECORD_STATE_PENDING);
    if (pr != AICAM_OK) return pr;

    UPLOAD_LOG("enqueue id=%s mode=%d storage=%d fs=%d\r\n",
           id, mode, g_up.cfg.storage, fs);

    /* Dispatch */
    switch (mode) {
    case CAPTURE_MODE_INSTANT:
        /* Sync attempt. Only on success do we kick a sweep of pending/ —
         * a successful upload means the channel is up, so earlier-failed
         * records are worth retrying now. On failure the channel is down
         * and a sweep would just waste IO; leave pending/ untouched. */
        {
            aicam_result_t r = upload_one_record(fs, id);
            UPLOAD_LOG("INSTANT upload_one_record=%d\r\n", r);
            if (r == AICAM_OK) {
                upload_coordinator_kick();
            }
        }
        return AICAM_OK;

    case CAPTURE_MODE_BATCH: {
        uint32_t pending = count_in_dir(fs, CAPTURES_DIR_PENDING);
        UPLOAD_LOG("BATCH pending=%u batch_count=%u\r\n",
               (unsigned)pending, (unsigned)g_up.cfg.batch_count);
        if (pending >= g_up.cfg.batch_count) {
            upload_coordinator_kick();
        }
        return AICAM_OK;
    }
    case CAPTURE_MODE_SCHEDULED:
        /* wake_scheduler handles flushing at configured minutes */
        return AICAM_OK;
    default:
        return AICAM_OK;
    }
}

/* ==================== Status / Records ==================== */

aicam_result_t upload_coordinator_get_status(upload_coordinator_status_t *out)
{
    if (!out) return AICAM_ERROR_INVALID_PARAM;
    memset(out, 0, sizeof(*out));
    out->mode = g_up.cfg.mode;
    out->storage = g_up.cfg.storage;
    out->initialized = g_up.initialized;
    out->running = g_up.running;
    out->storage_full = g_up.storage_full;
    out->actual_fs = g_up.active_fs;

    FS_Type_t fs = g_up.active_fs;
    if (fs != FS_MAX) {
        out->pending_count = count_in_dir(fs, CAPTURES_DIR_PENDING);
        out->sent_count    = count_in_dir(fs, CAPTURES_DIR_SENT);
        out->failed_count  = count_in_dir(fs, CAPTURES_DIR_FAILED);
        out->local_count   = count_in_dir(fs, CAPTURES_DIR_LOCAL);
        uint64_t free_b = 0, total_b = 0;
        if (get_storage_free_bytes(fs, &free_b, &total_b) == AICAM_OK) {
            out->bytes_used_kb = (total_b - free_b) / 1024u;
            out->bytes_available_kb = free_b / 1024u;
        }
    }
    wake_event_t ev;
    out->next_scheduled_at = wake_scheduler_next_event(now_unix(), 24 * 3600, &ev);
    return AICAM_OK;
}

typedef struct {
    record_info_t *out;
    int max;
    int written;
    int skipped;   /* parse-failed count for diagnostics */
    record_state_t state;
    FS_Type_t fs;
} list_ctx_t;

static aicam_result_t list_visitor(FS_Type_t fs, const char *id, void *user)
{
    list_ctx_t *ctx = (list_ctx_t *)user;
    if (ctx->written >= ctx->max) return AICAM_ERROR;

    char path[128];
    path_for_record(path, sizeof(path), state_dir(ctx->state), id);
    cJSON *meta = NULL;
    if (parse_meta_file(fs, path, &meta) != AICAM_OK) {
        /* Diagnose WHY parse failed: file absent / empty / corrupt content */
        struct stat st = {0};
        int stat_ret = disk_file_stat(fs, path, &st);
        long sz = (stat_ret == 0) ? (long)st.st_size : -1;
        UPLOAD_LOG("list: SKIP %s (stat=%d size=%ld path=%s)\r\n",
               id, stat_ret, sz, path);
        ctx->skipped++;
        return AICAM_OK;
    }
    UPLOAD_LOG("list: ok %s\r\n", id);

    record_info_t *r = &ctx->out[ctx->written];
    memset(r, 0, sizeof(*r));
    snprintf(r->id, sizeof(r->id), "%s", id);
    r->state = ctx->state;

    cJSON *t;
    t = cJSON_GetObjectItem(meta, "timestamp");  if (t) r->timestamp = (uint64_t)t->valuedouble;
    t = cJSON_GetObjectItem(meta, "size");       if (t) r->size = (uint32_t)t->valueint;
    t = cJSON_GetObjectItem(meta, "retry_count"); if (t) r->retry_count = (uint8_t)t->valueint;
    t = cJSON_GetObjectItem(meta, "trigger");
    if (t && cJSON_IsString(t)) snprintf(r->trigger, sizeof(r->trigger), "%s", t->valuestring);
    t = cJSON_GetObjectItem(meta, "last_error");
    if (t && cJSON_IsString(t)) snprintf(r->last_error, sizeof(r->last_error), "%s", t->valuestring);
    t = cJSON_GetObjectItem(meta, "inference_image");
    r->has_inference = (t && cJSON_IsString(t) && t->valuestring[0] != '\0');

    cJSON_Delete(meta);
    ctx->written++;
    return AICAM_OK;
}

int upload_coordinator_list_records(record_state_t filter,
                                     uint32_t offset, uint32_t limit,
                                     uint64_t from_ts, uint64_t to_ts,
                                     aicam_bool_t sort_desc,
                                     record_info_t *out, int max)
{
    if (!out || max <= 0 || g_up.active_fs == FS_MAX) return 0;
    list_ctx_t ctx = { .out = out, .max = max, .written = 0, .skipped = 0,
                       .state = filter, .fs = g_up.active_fs };
    iterate_records(g_up.active_fs, state_dir(filter), offset, limit,
                    from_ts, to_ts, sort_desc, list_visitor, &ctx);
    UPLOAD_LOG("list_records state=%d ok=%d skipped=%d\r\n",
           (int)filter, ctx.written, ctx.skipped);
    return ctx.written;
}

uint32_t upload_coordinator_count_records(record_state_t filter,
                                          uint64_t from_ts, uint64_t to_ts)
{
    if (g_up.active_fs == FS_MAX) return 0;
    /* Count mode: no visitor, no alloc; iterate skips date dirs outside range. */
    return (uint32_t)iterate_records(g_up.active_fs, state_dir(filter),
                                     0, 0, from_ts, to_ts, AICAM_FALSE, NULL, NULL);
}

aicam_result_t upload_coordinator_retry_record(const char *id)
{
    if (!id) return AICAM_ERROR_INVALID_PARAM;
    if (g_up.active_fs == FS_MAX) return AICAM_ERROR_INVALID_PARAM;

    char from[128];
    path_for_record(from, sizeof(from), CAPTURES_DIR_FAILED, id);
    cJSON *meta = NULL;
    if (parse_meta_file(g_up.active_fs, from, &meta) != AICAM_OK) return AICAM_ERROR;
    cJSON_ReplaceItemInObject(meta, "retry_count", cJSON_CreateNumber(0));
    cJSON_ReplaceItemInObject(meta, "last_error", cJSON_CreateString(""));
    write_meta_json(g_up.active_fs, from, meta);
    cJSON_Delete(meta);

    aicam_result_t r = move_record(g_up.active_fs, id, RECORD_STATE_FAILED, RECORD_STATE_PENDING);
    if (r == AICAM_OK) upload_coordinator_kick();
    return r;
}

typedef struct { int reset; FS_Type_t fs; } retry_all_ctx_t;
static aicam_result_t retry_all_visitor(FS_Type_t fs, const char *id, void *user)
{
    retry_all_ctx_t *ctx = (retry_all_ctx_t *)user;
    if (upload_coordinator_retry_record(id) == AICAM_OK) ctx->reset++;
    (void)fs;
    return AICAM_OK;
}

int upload_coordinator_retry_all_failed(void)
{
    if (g_up.active_fs == FS_MAX) return 0;
    retry_all_ctx_t ctx = { .reset = 0, .fs = g_up.active_fs };
    iterate_records(g_up.active_fs, CAPTURES_DIR_FAILED, 0, 0,
                    0, UINT64_MAX, AICAM_FALSE, retry_all_visitor, &ctx);
    return ctx.reset;
}

aicam_result_t upload_coordinator_delete_record(const char *id)
{
    if (!id) return AICAM_ERROR_INVALID_PARAM;
    if (g_up.active_fs == FS_MAX) return AICAM_ERROR_INVALID_PARAM;
    /* Try each state dir — only one should hit */
    const record_state_t order[] = {
        RECORD_STATE_PENDING, RECORD_STATE_SENT, RECORD_STATE_FAILED, RECORD_STATE_LOCAL
    };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        delete_record_files(g_up.active_fs, id, order[i]);
    }
    return AICAM_OK;
}
