#include "quick_storage.h"
#include "quick_trace.h"
#include "json_config_internal.h"
#include "cmsis_os2.h"
#include "mem.h"
#include "storage.h"
#include "sd_file.h"
#include "generic_file.h"
#include "common_utils.h"
#include "tx_api.h"
#include <string.h>
#include <stdio.h>

extern const aicam_global_config_t default_config;
extern aicam_result_t json_config_nvs_read_string(const char *key, char *value, size_t max_len);
extern aicam_result_t json_config_nvs_read_uint32(const char *key, uint32_t *value);
extern aicam_result_t json_config_nvs_read_uint64(const char *key, uint64_t *value);
extern aicam_result_t json_config_nvs_read_float(const char *key, float *value);
extern aicam_result_t json_config_nvs_read_uint8(const char *key, uint8_t *value);
extern aicam_result_t json_config_nvs_read_bool(const char *key, aicam_bool_t *value);
extern aicam_result_t json_config_nvs_read_int32(const char *key, int32_t *value);

typedef struct {
    qs_write_task_param_t param;  /* shallow copy of scalar fields + file_name */
} qs_write_task_item_t;

static osMessageQueueId_t s_write_q = NULL;
static osThreadId_t s_write_tid = NULL;
static aicam_bool_t s_quick_storage_inited = AICAM_FALSE;

/* Avoid MemAlloc() inside osMessageQueueNew (ThreadX CMSIS wrapper) by providing
 * both queue storage and control block statically.
 */
static TX_QUEUE s_write_q_cb ALIGN_32 IN_PSRAM;
static uint8_t s_write_q_mem[
    MAX_WRITE_TASK_QUEUE_SIZE * (((sizeof(qs_write_task_item_t) + (sizeof(ULONG) - 1U)) / sizeof(ULONG)) * sizeof(ULONG))
] ALIGN_32;
static const osMessageQueueAttr_t s_write_q_attr = {
    .name = "qs_write_q",
    .mq_mem = s_write_q_mem,
    .mq_size = sizeof(s_write_q_mem),
    .cb_mem = &s_write_q_cb,
    .cb_size = sizeof(s_write_q_cb),
};

static void u32_to_ipv4(uint32_t v, uint8_t out[4])
{
    if (!out) return;
    out[0] = (uint8_t)((v >> 24) & 0xFF);
    out[1] = (uint8_t)((v >> 16) & 0xFF);
    out[2] = (uint8_t)((v >> 8) & 0xFF);
    out[3] = (uint8_t)(v & 0xFF);
}

static int qs_check_free_space(uint8_t disk_type, size_t bytes_needed)
{
    uint32_t need_kb = (uint32_t)((bytes_needed + 1023U) / 1024U);

    if (disk_type == 2) {
        sd_disk_info_t info = {0};
        if (sd_get_disk_info(&info) != 0) return AICAM_ERROR_IO;
        if (info.mode != SD_MODE_NORMAL) return AICAM_ERROR_UNAVAILABLE;
        if (info.free_KBytes < need_kb) return AICAM_ERROR_QUOTA_EXCEEDED;
        return AICAM_OK;
    }

    storage_disk_info_t info = {0};
    if (storage_get_disk_info(&info) != 0) return AICAM_ERROR_IO;
    if (!info.mounted) return AICAM_ERROR_UNAVAILABLE;
    if (info.free_KBytes < need_kb) return AICAM_ERROR_QUOTA_EXCEEDED;
    return AICAM_OK;
}

static int qs_check_free_space_auto(size_t bytes_needed)
{
    FS_Type_t cur = file_get_current_type();
    if (cur == FS_SD) {
        return qs_check_free_space(2, bytes_needed);
    }
    if (cur == FS_FLASH) {
        return qs_check_free_space(1, bytes_needed);
    }
    return AICAM_ERROR_UNAVAILABLE;
}

static int qs_write_one(const qs_write_task_item_t *item)
{
    if (!item) return AICAM_ERROR_INVALID_PARAM;

    const qs_write_task_param_t *p = &item->param;
    if (!p->file_name[0] || !p->data || p->data_len == 0) return AICAM_ERROR_INVALID_PARAM;

    /* disk_type: 0=auto(generic), 1=flash, 2=sd */
    uint8_t disk_type = p->disk_type;
    if (disk_type > 2) return AICAM_ERROR_INVALID_PARAM;

    if (disk_type == 2) {
        sd_disk_info_t sdinfo = {0};
        if (sd_get_disk_info(&sdinfo) != 0) return AICAM_ERROR_IO;
        if (sdinfo.mode != SD_MODE_NORMAL) return AICAM_ERROR_UNAVAILABLE;
    }

    /* space check before open/write */
    if (disk_type == 0) {
        int rc = qs_check_free_space_auto(p->data_len);
        if (rc != AICAM_OK) return rc;
    } else {
        int rc = qs_check_free_space(disk_type, p->data_len);
        if (rc != AICAM_OK) return rc;
    }

    const char *mode = (p->mode == 0) ? "ab" : "wb";
    void *fd = NULL;
    int wret = AICAM_OK;

    if (disk_type == 0) {
        fd = file_fopen(p->file_name, mode);
        if (!fd) return AICAM_ERROR_IO;
        if (file_fwrite(fd, p->data, p->data_len) != (int)p->data_len) wret = AICAM_ERROR_IO;
        (void)file_fflush(fd);
        (void)file_fclose(fd);
        return wret;
    }

    FS_Type_t fs_type = (disk_type == 2) ? FS_SD : FS_FLASH;
    fd = disk_file_fopen(fs_type, p->file_name, mode);
    if (!fd) return AICAM_ERROR_IO;
    if (disk_file_fwrite(fs_type, fd, p->data, p->data_len) != (int)p->data_len) wret = AICAM_ERROR_IO;
    (void)disk_file_fflush(fs_type, fd);
    (void)disk_file_fclose(fs_type, fd);
    return wret;
}

static void qs_write_thread(void *arg)
{
    (void)arg;
    QT_TRACE("[QST] ", "write start");
    for (;;) {
        qs_write_task_item_t item = {0};
        if (osMessageQueueGet(s_write_q, &item, NULL, osWaitForever) != osOK) {
            continue;
        }

        qt_prof_t prof;
        qt_prof_init(&prof);
        QT_TRACE("[QST] ", "write %s %luB d%u m%u",
                 item.param.file_name,
                 (unsigned long)item.param.data_len,
                 (unsigned)item.param.disk_type,
                 (unsigned)item.param.mode);
        int result = qs_write_one(&item);
        if (result != AICAM_OK) {
            QT_TRACE("[QST] ", "write rc=%d", result);
        }
        qt_prof_step(&prof, "[QST] write:done ");
        if (item.param.callback) {
            item.param.callback(result, item.param.callback_param);
        }
    }
}

int quick_storage_read_snapshot_config(qs_snapshot_config_t *snapshot_config)
{
    if (!snapshot_config) return AICAM_ERROR_INVALID_PARAM;

    /* Defaults aligned with `default_config` in json_config_mgr.c */
    memset(snapshot_config, 0, sizeof(*snapshot_config));
    snapshot_config->ai_enabled = (uint8_t)default_config.ai_debug.ai_enabled;
    snapshot_config->ai_1_active = (uint8_t)default_config.ai_debug.ai_1_active;
    snapshot_config->ai_pipe_width = 0;
    snapshot_config->ai_pipe_height = 0;
    snapshot_config->confidence_threshold = default_config.ai_debug.confidence_threshold;
    snapshot_config->nms_threshold = default_config.ai_debug.nms_threshold;

    snapshot_config->light_mode = (uint8_t)default_config.device_service.light_config.mode;
    snapshot_config->light_threshold = default_config.device_service.light_config.light_threshold;
    snapshot_config->light_brightness = default_config.device_service.light_config.brightness_level;
    snapshot_config->light_start_time =
        (default_config.device_service.light_config.start_hour * 3600U) +
        (default_config.device_service.light_config.start_minute * 60U);
    snapshot_config->light_end_time =
        (default_config.device_service.light_config.end_hour * 3600U) +
        (default_config.device_service.light_config.end_minute * 60U);

    if (default_config.device_service.image_config.horizontal_flip &&
        default_config.device_service.image_config.vertical_flip) snapshot_config->mirror_flip = 3;
    else if (default_config.device_service.image_config.horizontal_flip) snapshot_config->mirror_flip = 2;
    else if (default_config.device_service.image_config.vertical_flip) snapshot_config->mirror_flip = 1;
    else snapshot_config->mirror_flip = 0;

    snapshot_config->fast_capture_skip_frames = default_config.device_service.image_config.fast_capture_skip_frames;
    snapshot_config->fast_capture_resolution = default_config.device_service.image_config.fast_capture_resolution;
    snapshot_config->fast_capture_jpeg_quality = default_config.device_service.image_config.fast_capture_jpeg_quality;
    snapshot_config->capture_storage_ai = default_config.device_service.image_config.capture_storage_ai;

    aicam_result_t result;
    aicam_bool_t temp_bool = AICAM_FALSE;
    uint8_t temp_u8 = 0;
    uint32_t temp_u32 = 0;

    result = json_config_nvs_read_bool(NVS_KEY_AI_ENABLE, &temp_bool);
    if (result == AICAM_OK) snapshot_config->ai_enabled = (uint8_t)temp_bool;

    if (snapshot_config->ai_enabled) {
        result = json_config_nvs_read_bool(NVS_KEY_AI_1_ACTIVE, &temp_bool);
        if (result == AICAM_OK) snapshot_config->ai_1_active = (uint8_t)temp_bool;

        /* AI 管道尺寸：读取失败或无效时保持 0，等待模型信息后再设置 */
        result = json_config_nvs_read_uint32(NVS_KEY_AI_PIPE_WIDTH, &temp_u32);
        if (result == AICAM_OK) snapshot_config->ai_pipe_width = temp_u32;

        result = json_config_nvs_read_uint32(NVS_KEY_AI_PIPE_HEIGHT, &temp_u32);
        if (result == AICAM_OK) snapshot_config->ai_pipe_height = temp_u32;

        result = json_config_nvs_read_uint32(NVS_KEY_CONFIDENCE, &temp_u32);
        if (result == AICAM_OK) snapshot_config->confidence_threshold = temp_u32;

        result = json_config_nvs_read_uint32(NVS_KEY_NMS_THRESHOLD, &temp_u32);
        if (result == AICAM_OK) snapshot_config->nms_threshold = temp_u32;
    }

    result = json_config_nvs_read_uint8(NVS_KEY_LIGHT_MODE, &temp_u8);
    if (result == AICAM_OK) snapshot_config->light_mode = temp_u8;

    if (snapshot_config->light_mode == LIGHT_MODE_AUTO) {
        result = json_config_nvs_read_uint32(NVS_KEY_LIGHT_THRESHOLD, &temp_u32);
        if (result == AICAM_OK) snapshot_config->light_threshold = temp_u32;
    }

    if (snapshot_config->light_mode != LIGHT_MODE_OFF) {
        result = json_config_nvs_read_uint32(NVS_KEY_LIGHT_BRIGHTNESS, &temp_u32);
        if (result == AICAM_OK) snapshot_config->light_brightness = temp_u32;
    }

    if (snapshot_config->light_mode == LIGHT_MODE_CUSTOM) {
        /* light custom schedule: store as seconds from 00:00 */
        uint32_t sh = 0, sm = 0, eh = 0, em = 0;
        if (json_config_nvs_read_uint32(NVS_KEY_LIGHT_START_HOUR, &sh) != AICAM_OK) sh = default_config.device_service.light_config.start_hour;
        if (json_config_nvs_read_uint32(NVS_KEY_LIGHT_START_MIN, &sm) != AICAM_OK) sm = default_config.device_service.light_config.start_minute;
        if (json_config_nvs_read_uint32(NVS_KEY_LIGHT_END_HOUR, &eh) != AICAM_OK) eh = default_config.device_service.light_config.end_hour;
        if (json_config_nvs_read_uint32(NVS_KEY_LIGHT_END_MIN, &em) != AICAM_OK) em = default_config.device_service.light_config.end_minute;
        snapshot_config->light_start_time = (sh * 3600U) + (sm * 60U);
        snapshot_config->light_end_time = (eh * 3600U) + (em * 60U);
    }

    {
        /* Mirror/flip: from boolean HFLIP/VFLIP */
        aicam_bool_t hflip = default_config.device_service.image_config.horizontal_flip;
        aicam_bool_t vflip = default_config.device_service.image_config.vertical_flip;
        (void)json_config_nvs_read_bool(NVS_KEY_IMAGE_HFLIP, &hflip);
        (void)json_config_nvs_read_bool(NVS_KEY_IMAGE_VFLIP, &vflip);
        if (hflip && vflip) snapshot_config->mirror_flip = 3;
        else if (hflip) snapshot_config->mirror_flip = 2;
        else if (vflip) snapshot_config->mirror_flip = 1;
        else snapshot_config->mirror_flip = 0;
    }

    result = json_config_nvs_read_uint32(NVS_KEY_IMAGE_FAST_SKIP_FRAMES, &temp_u32);
    if (result == AICAM_OK) snapshot_config->fast_capture_skip_frames = temp_u32;

    result = json_config_nvs_read_uint32(NVS_KEY_IMAGE_FAST_RESOLUTION, &temp_u32);
    if (result == AICAM_OK) snapshot_config->fast_capture_resolution = temp_u32;

    result = json_config_nvs_read_uint32(NVS_KEY_IMAGE_FAST_JPEG_QUALITY, &temp_u32);
    if (result == AICAM_OK) snapshot_config->fast_capture_jpeg_quality = temp_u32;

    result = json_config_nvs_read_bool(NVS_KEY_CAPTURE_STORAGE_AI, &temp_bool);
    if (result == AICAM_OK) snapshot_config->capture_storage_ai = (uint8_t)temp_bool;

    return AICAM_OK;
}

int quick_storage_read_work_mode_config(qs_work_mode_config_t *work_mode_config)
{
    if (!work_mode_config) return AICAM_ERROR_INVALID_PARAM;
    memset(work_mode_config, 0, sizeof(*work_mode_config));

    /* Defaults aligned with `default_config` */
    work_mode_config->work_mode = (uint8_t)default_config.work_mode_config.work_mode;
    work_mode_config->image_mode_enabled = (uint8_t)default_config.work_mode_config.image_mode.enable;

    work_mode_config->pir_trigger_enabled = (uint8_t)default_config.work_mode_config.pir_trigger.enable;
    work_mode_config->pir_trigger_type = (uint8_t)default_config.work_mode_config.pir_trigger.trigger_type;
    work_mode_config->pir_trigger_sensitivity = (uint8_t)default_config.work_mode_config.pir_trigger.sensitivity_level;
    work_mode_config->pir_trigger_ignore_time = (uint8_t)default_config.work_mode_config.pir_trigger.ignore_time_s;
    work_mode_config->pir_trigger_pulse_count = (uint8_t)default_config.work_mode_config.pir_trigger.pulse_count;
    work_mode_config->pir_trigger_window_time = (uint8_t)default_config.work_mode_config.pir_trigger.window_time_s;

    work_mode_config->timer_trigger_enabled = (uint8_t)default_config.work_mode_config.timer_trigger.enable;
    work_mode_config->timer_trigger_capture_mode = (uint8_t)default_config.work_mode_config.timer_trigger.capture_mode;
    work_mode_config->timer_trigger_interval_sec = default_config.work_mode_config.timer_trigger.interval_sec;
    work_mode_config->timer_trigger_time_node_count = default_config.work_mode_config.timer_trigger.time_node_count;
    for (uint32_t i = 0; i < 10; i++) {
        work_mode_config->timer_trigger_time_node[i] = default_config.work_mode_config.timer_trigger.time_node[i];
        work_mode_config->timer_trigger_weekdays[i] = default_config.work_mode_config.timer_trigger.weekdays[i];
    }

    work_mode_config->remote_trigger_enabled = (uint8_t)default_config.work_mode_config.remote_trigger.enable;

    aicam_result_t result;
    aicam_bool_t temp_bool = AICAM_FALSE;
    uint32_t temp_u32 = 0;
    uint8_t temp_u8 = 0;

    result = json_config_nvs_read_uint32(NVS_KEY_WORK_MODE, &temp_u32);
    if (result == AICAM_OK) work_mode_config->work_mode = (uint8_t)temp_u32;

    result = json_config_nvs_read_bool(NVS_KEY_IMAGE_MODE_ENABLE, &temp_bool);
    if (result == AICAM_OK) work_mode_config->image_mode_enabled = (uint8_t)temp_bool;

    result = json_config_nvs_read_bool(NVS_KEY_PIR_ENABLE, &temp_bool);
    if (result == AICAM_OK) work_mode_config->pir_trigger_enabled = (uint8_t)temp_bool;

    if (work_mode_config->pir_trigger_enabled) {
        result = json_config_nvs_read_uint8(NVS_KEY_PIR_TRIGGER_TYPE, &temp_u8);
        if (result == AICAM_OK) work_mode_config->pir_trigger_type = temp_u8;

        result = json_config_nvs_read_uint8(NVS_KEY_PIR_SENSITIVITY, &temp_u8);
        if (result == AICAM_OK) work_mode_config->pir_trigger_sensitivity = temp_u8;

        result = json_config_nvs_read_uint8(NVS_KEY_PIR_IGNORE_TIME, &temp_u8);
        if (result == AICAM_OK) work_mode_config->pir_trigger_ignore_time = temp_u8;

        result = json_config_nvs_read_uint8(NVS_KEY_PIR_PULSE_COUNT, &temp_u8);
        if (result == AICAM_OK) work_mode_config->pir_trigger_pulse_count = temp_u8;

        result = json_config_nvs_read_uint8(NVS_KEY_PIR_WINDOW_TIME, &temp_u8);
        if (result == AICAM_OK) work_mode_config->pir_trigger_window_time = temp_u8;
    }

    result = json_config_nvs_read_bool(NVS_KEY_TIMER_ENABLE, &temp_bool);
    if (result == AICAM_OK) work_mode_config->timer_trigger_enabled = (uint8_t)temp_bool;

    if (work_mode_config->timer_trigger_enabled) {
        result = json_config_nvs_read_uint8(NVS_KEY_TIMER_CAPTURE_MODE, &temp_u8);
        if (result == AICAM_OK) work_mode_config->timer_trigger_capture_mode = temp_u8;

        result = json_config_nvs_read_uint32(NVS_KEY_TIMER_INTERVAL, &temp_u32);
        if (result == AICAM_OK) work_mode_config->timer_trigger_interval_sec = temp_u32;

        result = json_config_nvs_read_uint32(NVS_KEY_TIMER_NODE_COUNT, &temp_u32);
        if (result == AICAM_OK) {
            if (temp_u32 > 10) temp_u32 = 10;
            work_mode_config->timer_trigger_time_node_count = temp_u32;
        }

        for (uint32_t i = 0; i < work_mode_config->timer_trigger_time_node_count; i++) {
            char key_name[32];
            snprintf(key_name, sizeof(key_name), "%s%u", NVS_KEY_TIMER_NODE_PREFIX, (unsigned int)i);
            if (json_config_nvs_read_uint32(key_name, &temp_u32) == AICAM_OK) {
                work_mode_config->timer_trigger_time_node[i] = temp_u32;
            }

            snprintf(key_name, sizeof(key_name), "%s%u", NVS_KEY_TIMER_WEEKDAYS_PREFIX, (unsigned int)i);
            if (json_config_nvs_read_uint8(key_name, &temp_u8) == AICAM_OK) {
                work_mode_config->timer_trigger_weekdays[i] = temp_u8;
            }
        }
    }

    result = json_config_nvs_read_bool(NVS_KEY_REMOTE_TRIGGER_ENABLE, &temp_bool);
    if (result == AICAM_OK) work_mode_config->remote_trigger_enabled = (uint8_t)temp_bool;

    return AICAM_OK;
}

int quick_storage_read_comm_pref_type(qs_comm_pref_type_t *comm_pref_type)
{
    uint32_t temp_u32 = 0;
    aicam_bool_t temp_bool = AICAM_FALSE;
    if (!comm_pref_type) return AICAM_ERROR_INVALID_PARAM;

    if (json_config_nvs_read_bool(NVS_KEY_CAPTURE_DISABLE_COMM, &temp_bool) == AICAM_OK && temp_bool) {
        *comm_pref_type = COMM_PREF_TYPE_DISABLE;
        return AICAM_OK;
    }
    if (json_config_nvs_read_bool(NVS_KEY_COMM_AUTO_PRIORITY, &temp_bool) == AICAM_OK && temp_bool) {
        *comm_pref_type = COMM_PREF_TYPE_AUTO;
        return AICAM_OK;
    }
    if (json_config_nvs_read_uint32(NVS_KEY_COMM_PREFERRED_TYPE, &temp_u32) != AICAM_OK) {
        *comm_pref_type = COMM_PREF_TYPE_AUTO;
        return AICAM_OK;
    }

    switch (temp_u32) {
        case COMM_PREF_TYPE_WIFI:
        case COMM_PREF_TYPE_CELLULAR:
        case COMM_PREF_TYPE_POE:
            *comm_pref_type = (qs_comm_pref_type_t)temp_u32;
            break;
        default:
            *comm_pref_type = COMM_PREF_TYPE_AUTO;
            break;
    }
    return AICAM_OK;
}

int quick_storage_read_known_wifi_networks(qs_wifi_network_info_t *known_wifi_networks, uint32_t *count)
{
    if (!known_wifi_networks || !count) return AICAM_ERROR_INVALID_PARAM;

    memset(known_wifi_networks, 0, sizeof(qs_wifi_network_info_t) * MAX_KNOWN_WIFI_NETWORKS);
    *count = 0;

    uint32_t temp_u32 = 0;
    if (json_config_nvs_read_uint32(NVS_KEY_NETWORK_KNOWN_COUNT, &temp_u32) != AICAM_OK) {
        return AICAM_OK;
    }

    if (temp_u32 > MAX_KNOWN_WIFI_NETWORKS) temp_u32 = MAX_KNOWN_WIFI_NETWORKS;
    *count = temp_u32;

    for (uint32_t i = 0; i < *count; i++) {
        char key_name[32];

        snprintf(key_name, sizeof(key_name), "net_%u_ssid", (unsigned int)i);
        (void)json_config_nvs_read_string(key_name, known_wifi_networks[i].ssid, sizeof(known_wifi_networks[i].ssid));

        snprintf(key_name, sizeof(key_name), "net_%u_bssid", (unsigned int)i);
        (void)json_config_nvs_read_string(key_name, known_wifi_networks[i].bssid, sizeof(known_wifi_networks[i].bssid));

        snprintf(key_name, sizeof(key_name), "net_%u_pwd", (unsigned int)i);
        (void)json_config_nvs_read_string(key_name, known_wifi_networks[i].password, sizeof(known_wifi_networks[i].password));

        snprintf(key_name, sizeof(key_name), "net_%u_time", (unsigned int)i);
        (void)json_config_nvs_read_uint32(key_name, &known_wifi_networks[i].last_connected_time);
    }

    return AICAM_OK;
}

int quick_storage_read_netif_config(qs_comm_pref_type_t comm_pref_type, netif_config_t *netif_config)
{
    if (!netif_config) return AICAM_ERROR_INVALID_PARAM;

    static char s_host_name_buf[NETIF_HOST_NAME_SIZE];
    memset(s_host_name_buf, 0, sizeof(s_host_name_buf));
    netif_config->host_name = s_host_name_buf;

    uint32_t temp_u32 = 0;
    aicam_bool_t temp_bool = AICAM_FALSE;

    if (comm_pref_type == COMM_PREF_TYPE_WIFI) {
        (void)json_config_nvs_read_string(NVS_KEY_NETWORK_SSID, netif_config->wireless_cfg.ssid, sizeof(netif_config->wireless_cfg.ssid));
        (void)json_config_nvs_read_string(NVS_KEY_NETWORK_PASSWORD, netif_config->wireless_cfg.pw, sizeof(netif_config->wireless_cfg.pw));
        netif_config->wireless_cfg.channel = 0;
        netif_config->wireless_cfg.security = WIRELESS_SECURITY_UNKNOWN;
        netif_config->wireless_cfg.encryption = WIRELESS_DEFAULT_ENCRYPTION;
        netif_config->ip_mode = NETIF_IP_MODE_DHCP;
        return AICAM_OK;
    }

    if (comm_pref_type == COMM_PREF_TYPE_CELLULAR) {
        (void)json_config_nvs_read_string(NVS_KEY_CELLULAR_APN, netif_config->cellular_cfg.apn, sizeof(netif_config->cellular_cfg.apn));
        (void)json_config_nvs_read_string(NVS_KEY_CELLULAR_USERNAME, netif_config->cellular_cfg.user, sizeof(netif_config->cellular_cfg.user));
        (void)json_config_nvs_read_string(NVS_KEY_CELLULAR_PASSWORD, netif_config->cellular_cfg.passwd, sizeof(netif_config->cellular_cfg.passwd));
        (void)json_config_nvs_read_string(NVS_KEY_CELLULAR_PIN, netif_config->cellular_cfg.pin, sizeof(netif_config->cellular_cfg.pin));
        if (json_config_nvs_read_uint8(NVS_KEY_CELLULAR_AUTH, &netif_config->cellular_cfg.authentication) != AICAM_OK) {
            netif_config->cellular_cfg.authentication = 0;
        }
        if (json_config_nvs_read_bool(NVS_KEY_CELLULAR_ROAMING, &temp_bool) == AICAM_OK) {
            netif_config->cellular_cfg.is_enable_roam = (uint8_t)temp_bool;
        }
        if (json_config_nvs_read_uint8(NVS_KEY_CELLULAR_OPERATOR, &netif_config->cellular_cfg.isp_selected) != AICAM_OK) {
            netif_config->cellular_cfg.isp_selected = 0;
        }
        netif_config->ip_mode = NETIF_IP_MODE_DHCP;
        return AICAM_OK;
    }

    if (comm_pref_type == COMM_PREF_TYPE_POE) {
        /* PoE config in NVS stores IPv4 as packed uint32 (A.B.C.D => (A<<24|B<<16|C<<8|D)) */
        if (json_config_nvs_read_uint32(NVS_KEY_POE_IP_MODE, &temp_u32) == AICAM_OK) {
            netif_config->ip_mode = (temp_u32 == 0) ? NETIF_IP_MODE_DHCP : NETIF_IP_MODE_STATIC;
        } else {
            netif_config->ip_mode = NETIF_IP_MODE_DHCP;
        }

        if (json_config_nvs_read_uint32(NVS_KEY_POE_IP_ADDR, &temp_u32) == AICAM_OK) u32_to_ipv4(temp_u32, netif_config->ip_addr);
        if (json_config_nvs_read_uint32(NVS_KEY_POE_NETMASK, &temp_u32) == AICAM_OK) u32_to_ipv4(temp_u32, netif_config->netmask);
        if (json_config_nvs_read_uint32(NVS_KEY_POE_GATEWAY, &temp_u32) == AICAM_OK) u32_to_ipv4(temp_u32, netif_config->gw);

        (void)json_config_nvs_read_string(NVS_KEY_POE_HOSTNAME, s_host_name_buf, sizeof(s_host_name_buf));
        return AICAM_OK;
    }

    /* AUTO/DISABLE: caller decides; we return empty config */
    netif_config->ip_mode = NETIF_IP_MODE_DHCP;
    return AICAM_OK;
}

/* Align with mqtt_service: only non-empty NVS strings become non-NULL pointers. */
static char *qs_mqtt_nonempty_or_null(char *s)
{
    if (s == NULL || s[0] == '\0') {
        return NULL;
    }
    return s;
}

int quick_storage_read_mqtt_all_config(qs_mqtt_all_config_t *mqtt_all_config)
{
    if (!mqtt_all_config) return AICAM_ERROR_INVALID_PARAM;
    memset(mqtt_all_config, 0, sizeof(*mqtt_all_config));

    /* backing store for ms_mqtt_config_t pointers */
    static char s_hostname[128];
    static char s_client_id[64];
    static char s_username[64];
    static char s_password[128];
    static char s_ca_path[128];
    static char s_ca_data[128];
    static char s_client_cert_path[128];
    static char s_client_cert_data[128];
    static char s_client_key_path[128];
    static char s_client_key_data[128];
    static char s_lwt_topic[MAX_TOPIC_LENGTH];
    static char s_lwt_message[256];

    memset(s_hostname, 0, sizeof(s_hostname));
    memset(s_client_id, 0, sizeof(s_client_id));
    memset(s_username, 0, sizeof(s_username));
    memset(s_password, 0, sizeof(s_password));
    memset(s_ca_path, 0, sizeof(s_ca_path));
    memset(s_ca_data, 0, sizeof(s_ca_data));
    memset(s_client_cert_path, 0, sizeof(s_client_cert_path));
    memset(s_client_cert_data, 0, sizeof(s_client_cert_data));
    memset(s_client_key_path, 0, sizeof(s_client_key_path));
    memset(s_client_key_data, 0, sizeof(s_client_key_data));
    memset(s_lwt_topic, 0, sizeof(s_lwt_topic));
    memset(s_lwt_message, 0, sizeof(s_lwt_message));

    uint32_t temp_u32 = 0;
    uint8_t temp_u8 = 0;

    (void)json_config_nvs_read_string(NVS_KEY_MQTT_RECV_TOPIC, mqtt_all_config->data_receive_topic, sizeof(mqtt_all_config->data_receive_topic));
    (void)json_config_nvs_read_string(NVS_KEY_MQTT_REPORT_TOPIC, mqtt_all_config->data_report_topic, sizeof(mqtt_all_config->data_report_topic));

    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_RECV_QOS, &temp_u32) == AICAM_OK) mqtt_all_config->data_receive_qos = (uint8_t)temp_u32;
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_REPORT_QOS, &temp_u32) == AICAM_OK) mqtt_all_config->data_report_qos = (uint8_t)temp_u32;

    /* Base */
    if (json_config_nvs_read_uint8(NVS_KEY_MQTT_PROTOCOL_VER, &temp_u8) == AICAM_OK) mqtt_all_config->ms_mqtt_config.base.protocol_ver = temp_u8;
    (void)json_config_nvs_read_string(NVS_KEY_MQTT_HOST, s_hostname, sizeof(s_hostname));
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_PORT, &temp_u32) == AICAM_OK) mqtt_all_config->ms_mqtt_config.base.port = (uint16_t)temp_u32;
    (void)json_config_nvs_read_string(NVS_KEY_MQTT_CLIENT_ID, s_client_id, sizeof(s_client_id));
    if (json_config_nvs_read_uint8(NVS_KEY_MQTT_CLEAN_SESSION, &temp_u8) == AICAM_OK) mqtt_all_config->ms_mqtt_config.base.clean_session = temp_u8;
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_KEEPALIVE, &temp_u32) == AICAM_OK) mqtt_all_config->ms_mqtt_config.base.keepalive = (int)temp_u32;

    /* Authentication */
    (void)json_config_nvs_read_string(NVS_KEY_MQTT_USERNAME, s_username, sizeof(s_username));
    (void)json_config_nvs_read_string(NVS_KEY_MQTT_PASSWORD, s_password, sizeof(s_password));

    (void)json_config_nvs_read_string(NVS_KEY_MQTT_CA_CERT_PATH, s_ca_path, sizeof(s_ca_path));
    (void)json_config_nvs_read_string(NVS_KEY_MQTT_CA_CERT_DATA, s_ca_data, sizeof(s_ca_data));
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_CA_CERT_LEN, &temp_u32) == AICAM_OK) mqtt_all_config->ms_mqtt_config.authentication.ca_len = (size_t)temp_u32;

    (void)json_config_nvs_read_string(NVS_KEY_MQTT_CLIENT_CERT_PATH, s_client_cert_path, sizeof(s_client_cert_path));
    (void)json_config_nvs_read_string(NVS_KEY_MQTT_CLIENT_CERT_DATA, s_client_cert_data, sizeof(s_client_cert_data));
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_CLIENT_CERT_LEN, &temp_u32) == AICAM_OK) mqtt_all_config->ms_mqtt_config.authentication.client_cert_len = (size_t)temp_u32;

    (void)json_config_nvs_read_string(NVS_KEY_MQTT_CLIENT_KEY_PATH, s_client_key_path, sizeof(s_client_key_path));
    (void)json_config_nvs_read_string(NVS_KEY_MQTT_CLIENT_KEY_DATA, s_client_key_data, sizeof(s_client_key_data));
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_CLIENT_KEY_LEN, &temp_u32) == AICAM_OK) mqtt_all_config->ms_mqtt_config.authentication.client_key_len = (size_t)temp_u32;

    if (json_config_nvs_read_uint8(NVS_KEY_MQTT_VERIFY_HOSTNAME, &temp_u8) == AICAM_OK) mqtt_all_config->ms_mqtt_config.authentication.is_verify_hostname = temp_u8;

    /* LWT */
    (void)json_config_nvs_read_string(NVS_KEY_MQTT_LWT_TOPIC, s_lwt_topic, sizeof(s_lwt_topic));
    (void)json_config_nvs_read_string(NVS_KEY_MQTT_LWT_MESSAGE, s_lwt_message, sizeof(s_lwt_message));
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_LWT_MSG_LEN, &temp_u32) == AICAM_OK) mqtt_all_config->ms_mqtt_config.last_will.msg_len = (int)temp_u32;
    if (json_config_nvs_read_uint8(NVS_KEY_MQTT_LWT_QOS, &temp_u8) == AICAM_OK) mqtt_all_config->ms_mqtt_config.last_will.qos = (int)temp_u8;
    if (json_config_nvs_read_uint8(NVS_KEY_MQTT_LWT_RETAIN, &temp_u8) == AICAM_OK) mqtt_all_config->ms_mqtt_config.last_will.retain = (int)temp_u8;

    /* Task */
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_TASK_PRIORITY, &temp_u32) == AICAM_OK) mqtt_all_config->ms_mqtt_config.task.priority = (int)temp_u32;
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_TASK_STACK, &temp_u32) == AICAM_OK) mqtt_all_config->ms_mqtt_config.task.stack_size = (int)temp_u32;

    /* Network */
    if (json_config_nvs_read_uint8(NVS_KEY_MQTT_DISABLE_RECONNECT, &temp_u8) == AICAM_OK) mqtt_all_config->ms_mqtt_config.network.disable_auto_reconnect = temp_u8;
    if (json_config_nvs_read_uint8(NVS_KEY_MQTT_OUTBOX_LIMIT, &temp_u8) == AICAM_OK) mqtt_all_config->ms_mqtt_config.network.outbox_limit = temp_u8;
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_OUTBOX_RESEND_IV, &temp_u32) == AICAM_OK) mqtt_all_config->ms_mqtt_config.network.outbox_resend_interval_ms = (int)temp_u32;
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_OUTBOX_EXPIRE, &temp_u32) == AICAM_OK) mqtt_all_config->ms_mqtt_config.network.outbox_expired_timeout = (int)temp_u32;
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_RECONNECT_INTERVAL, &temp_u32) == AICAM_OK) mqtt_all_config->ms_mqtt_config.network.reconnect_interval_ms = (int)temp_u32;
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_TIMEOUT, &temp_u32) == AICAM_OK) mqtt_all_config->ms_mqtt_config.network.timeout_ms = (int)temp_u32;
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_BUFFER_SIZE, &temp_u32) == AICAM_OK) mqtt_all_config->ms_mqtt_config.network.buffer_size = (int)temp_u32;
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_TX_BUF_SIZE, &temp_u32) == AICAM_OK) mqtt_all_config->ms_mqtt_config.network.tx_buf_size = (int)temp_u32;
    if (json_config_nvs_read_uint32(NVS_KEY_MQTT_RX_BUF_SIZE, &temp_u32) == AICAM_OK) mqtt_all_config->ms_mqtt_config.network.rx_buf_size = (int)temp_u32;

    {
        ms_mqtt_config_t *mc = &mqtt_all_config->ms_mqtt_config;

        mc->base.hostname = qs_mqtt_nonempty_or_null(s_hostname);
        mc->base.client_id = qs_mqtt_nonempty_or_null(s_client_id);
        mc->authentication.username = qs_mqtt_nonempty_or_null(s_username);
        mc->authentication.password = qs_mqtt_nonempty_or_null(s_password);
        mc->authentication.ca_path = qs_mqtt_nonempty_or_null(s_ca_path);
        mc->authentication.client_cert_path = qs_mqtt_nonempty_or_null(s_client_cert_path);
        mc->authentication.client_key_path = qs_mqtt_nonempty_or_null(s_client_key_path);

        mc->authentication.ca_data = qs_mqtt_nonempty_or_null(s_ca_data);
        if (mc->authentication.ca_data == NULL) {
            mc->authentication.ca_len = 0;
        }
        mc->authentication.client_cert_data = qs_mqtt_nonempty_or_null(s_client_cert_data);
        if (mc->authentication.client_cert_data == NULL) {
            mc->authentication.client_cert_len = 0;
        }
        mc->authentication.client_key_data = qs_mqtt_nonempty_or_null(s_client_key_data);
        if (mc->authentication.client_key_data == NULL) {
            mc->authentication.client_key_len = 0;
        }

        mc->last_will.topic = qs_mqtt_nonempty_or_null(s_lwt_topic);
        mc->last_will.msg = qs_mqtt_nonempty_or_null(s_lwt_message);
        if (mc->last_will.topic == NULL) {
            mc->last_will.msg = NULL;
            mc->last_will.msg_len = 0;
        } else if (mc->last_will.msg == NULL) {
            mc->last_will.msg_len = 0;
        }
    }

    return AICAM_OK;
}

int quick_storage_read_device_info(qs_device_info_t *device_info)
{
    if (!device_info) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    memset(device_info, 0, sizeof(*device_info));

    (void)snprintf(device_info->device_name, sizeof(device_info->device_name), "%s",
                   default_config.device_info.device_name);
    (void)snprintf(device_info->mac_address, sizeof(device_info->mac_address), "%s",
                   default_config.device_info.mac_address);
    (void)snprintf(device_info->serial_number, sizeof(device_info->serial_number), "%s",
                   default_config.device_info.serial_number);
    (void)snprintf(device_info->hardware_version, sizeof(device_info->hardware_version), "%s",
                   default_config.device_info.hardware_version);

    (void)json_config_nvs_read_string(NVS_KEY_DEVICE_INFO_NAME, device_info->device_name,
                                      sizeof(device_info->device_name));
    (void)json_config_nvs_read_string(NVS_KEY_DEVICE_INFO_MAC, device_info->mac_address,
                                      sizeof(device_info->mac_address));
    (void)json_config_nvs_read_string(NVS_KEY_DEVICE_INFO_SERIAL, device_info->serial_number,
                                      sizeof(device_info->serial_number));
    (void)json_config_nvs_read_string(NVS_KEY_DEVICE_INFO_HW_VER, device_info->hardware_version,
                                      sizeof(device_info->hardware_version));

    return AICAM_OK;
}

int quick_storage_init(void)
{
    if (s_quick_storage_inited) return AICAM_OK;

    if (s_write_q == NULL) {
        s_write_q = osMessageQueueNew(MAX_WRITE_TASK_QUEUE_SIZE, sizeof(qs_write_task_item_t), &s_write_q_attr);
        if (s_write_q == NULL) return AICAM_ERROR_NO_MEMORY;
    }

    if (s_write_tid == NULL) {
        /* CMSIS-RTOS2 may require user-provided stacks to be 8-byte aligned.
         * Also avoid relying on heap allocation during early boot.
         */
        static uint8_t s_write_stack[4 * 1024] ALIGN_32;
        static const osThreadAttr_t attr = {
            .name = "qs_write",
            .stack_mem = s_write_stack,
            .priority = (osPriority_t)osPriorityNormal,
            .stack_size = sizeof(s_write_stack),
            .cb_mem     = 0,
            .cb_size    = 0,
            .attr_bits  = 0u,
            .tz_module  = 0u,
        };
        s_write_tid = osThreadNew(qs_write_thread, NULL, &attr);
        if (s_write_tid == NULL) return AICAM_ERROR_NO_MEMORY;
    }

    s_quick_storage_inited = AICAM_TRUE;
    QT_TRACE("[QST] ", "init ok");
    return AICAM_OK;
}

int quick_storage_add_write_task(qs_write_task_param_t *write_task_param)
{
    if (!write_task_param) return AICAM_ERROR_INVALID_PARAM;
    if (!write_task_param->data || write_task_param->data_len == 0) return AICAM_ERROR_INVALID_PARAM;
    if (write_task_param->disk_type > 2) return AICAM_ERROR_INVALID_PARAM;
    if (!write_task_param->file_name[0]) return AICAM_ERROR_INVALID_PARAM;

    if (!s_quick_storage_inited || s_write_q == NULL || s_write_tid == NULL) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }
    int rc = AICAM_OK;

    /* Pre-check SD connection if explicitly targeting SD */
    if (write_task_param->disk_type == 2) {
        sd_disk_info_t info = {0};
        if (sd_get_disk_info(&info) != 0) return AICAM_ERROR_IO;
        if (info.mode != SD_MODE_NORMAL) return AICAM_ERROR_UNAVAILABLE;
    }

    /* Space check before enqueuing (avoid allocating/queueing tasks that must fail) */
    if (write_task_param->disk_type == 0) {
        rc = qs_check_free_space_auto(write_task_param->data_len);
    } else {
        rc = qs_check_free_space(write_task_param->disk_type, write_task_param->data_len);
    }
    if (rc != AICAM_OK) return rc;

    qs_write_task_item_t item = {0};
    memcpy(&item.param, write_task_param, sizeof(item.param));

    if (osMessageQueuePut(s_write_q, &item, 0, 0) != osOK) {
        return AICAM_ERROR_FULL;
    }

    QT_TRACE("[QST] ", "q %s %luB d%u",
             write_task_param->file_name,
             (unsigned long)write_task_param->data_len,
             (unsigned)write_task_param->disk_type);
    return AICAM_OK;
}

