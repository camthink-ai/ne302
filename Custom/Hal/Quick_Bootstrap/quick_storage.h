#ifndef __QUICK_STORAGE_H__
#define __QUICK_STORAGE_H__

#include "../Network/netif_manager/netif_manager.h"
#include "../Network/mqtt_client/ms_mqtt_client.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_KNOWN_WIFI_NETWORKS         16
#define MAX_WRITE_FILE_NAME_LEN         128
#define MAX_WRITE_TASK_QUEUE_SIZE       10

/**
 * @brief Snapshot configuration
 */
typedef struct {
    uint8_t ai_enabled;
    uint8_t ai_1_active;
    uint32_t ai_pipe_width;   /**< 0 或无效表示未从 NVS 解析到可靠值，须待模型信息后再定 AI 管道 */
    uint32_t ai_pipe_height;  /**< 同上 */
    uint32_t confidence_threshold;
    uint32_t nms_threshold;

    uint8_t light_mode;             //0: off, 1: on, 2: auto, 3: custom
    uint32_t light_threshold;
    uint32_t light_brightness;
    uint32_t light_start_time;
    uint32_t light_end_time;

    uint8_t mirror_flip;            //0: none, 1: flip, 2: mirror, 3: flip + mirror
    uint32_t fast_capture_skip_frames;
    uint32_t fast_capture_resolution;
    uint32_t fast_capture_jpeg_quality;
    uint8_t capture_storage_ai;
    
    // TODO: add ISP configuration

} qs_snapshot_config_t;

/**
 * @brief Read snapshot configuration from nvs storage
 * @param snapshot_config Snapshot configuration
 * @return 0 on success, other values on error
 */
int quick_storage_read_snapshot_config(qs_snapshot_config_t *snapshot_config);


typedef struct {
    uint8_t work_mode;                  // 0: image, 1: video stream
    
    uint8_t image_mode_enabled;
    // TODO: add video stream mode configuration and io trigger configuration

    uint8_t pir_trigger_enabled;
    uint8_t pir_trigger_type;               // 0: rising, 1: falling, 2: both edges, 3: high level, 4: low level
    uint8_t pir_trigger_sensitivity;
    uint8_t pir_trigger_ignore_time;
    uint8_t pir_trigger_pulse_count;
    uint8_t pir_trigger_window_time;

    uint8_t timer_trigger_enabled;
    uint8_t timer_trigger_capture_mode;     // 0: none, 1: interval, 2: absolute
    uint32_t timer_trigger_interval_sec;
    uint32_t timer_trigger_time_node_count;
    uint32_t timer_trigger_time_node[10];
    uint8_t timer_trigger_weekdays[10];     // 0: all days, 1: Monday, 2: Tuesday, 3: Wednesday, 4: Thursday, 5: Friday, 6: Saturday, 7: Sunday

    uint8_t remote_trigger_enabled;
} qs_work_mode_config_t;

/**
 * @brief Read work mode configuration from nvs storage
 * @param work_mode_config Work mode configuration
 * @return 0 on success, other values on error
 */
int quick_storage_read_work_mode_config(qs_work_mode_config_t *work_mode_config);

/**
 * @brief Communication preference type
 */
typedef enum {
    COMM_PREF_TYPE_AUTO = 0,
    COMM_PREF_TYPE_WIFI = 1,
    COMM_PREF_TYPE_CELLULAR = 2,
    COMM_PREF_TYPE_POE = 3,
    COMM_PREF_TYPE_DISABLE = 0XFF,
} qs_comm_pref_type_t;

/**
 * @brief Read communication preference type from nvs storage
 * @param comm_pref_type Communication preference type
 * @return 0 on success, other values on error
 */
int quick_storage_read_comm_pref_type(qs_comm_pref_type_t *comm_pref_type);

/**
 * @brief WiFi network information
 */
typedef struct {
    char ssid[32];
    char bssid[18];
    char password[64];
    uint32_t last_connected_time;
} qs_wifi_network_info_t;

/**
 * @brief Read known WiFi networks from nvs storage
 * @param known_wifi_networks Known WiFi networks
 * @param count Known WiFi networks count
 * @return 0 on success, other values on error
 */
int quick_storage_read_known_wifi_networks(qs_wifi_network_info_t *known_wifi_networks, uint32_t *count);

/**
 * @brief Read network interface configuration from nvs storage
 * @param comm_pref_type Communication preference type
 * @param netif_config Network interface configuration
 * @return 0 on success, other values on error
 */
int quick_storage_read_netif_config(qs_comm_pref_type_t comm_pref_type, netif_config_t *netif_config);

/**
 * @brief MQTT all configuration
 */
typedef struct {
    char data_receive_topic[128];
    char data_report_topic[128];

    uint8_t data_receive_qos;
    uint8_t data_report_qos;

    ms_mqtt_config_t ms_mqtt_config;
} qs_mqtt_all_config_t;

/**
 * @brief Read MQTT all configuration from nvs storage
 * @param mqtt_all_config MQTT all configuration
 * @return 0 on success, other values on error
 */
int quick_storage_read_mqtt_all_config(qs_mqtt_all_config_t *mqtt_all_config);


/**
 * @brief Device information
 */
typedef struct {
    char device_name[64];
    char mac_address[18];
    char serial_number[32];
    char hardware_version[32];
} qs_device_info_t;

/**
 * @brief Read device information from nvs storage
 * @param device_info Device information
 * @return 0 on success, other values on error
 */
int quick_storage_read_device_info(qs_device_info_t *device_info);

/**
 * @brief Write task parameter
 */
typedef struct {
    uint8_t mode;  // 0: append, 1: overwrite
    uint8_t disk_type;  // 0: Auto, 1: flash, 2: sd card
    char file_name[MAX_WRITE_FILE_NAME_LEN];
    void *data;
    size_t data_len;
    void (*callback)(int result, void *param);
    void *callback_param;
} qs_write_task_param_t;

/**
 * @brief Initialize quick storage
 * @return 0 on success, other values on error
 */
int quick_storage_init(void);

/**
 * @brief Add write task
 * @param write_task_param Write task parameter
 * @note `write_task_param->data` buffer must remain valid until callback is invoked.
 *       The caller must NOT free or reuse the buffer before callback.
 * @return 0 on success, other values on error
 */
int quick_storage_add_write_task(qs_write_task_param_t *write_task_param);

#ifdef __cplusplus
}
#endif

#endif /* __QUICK_STORAGE_H__ */
