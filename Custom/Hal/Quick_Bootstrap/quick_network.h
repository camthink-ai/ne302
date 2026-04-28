#ifndef __QUICK_NETWORK_H__
#define __QUICK_NETWORK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "quick_storage.h"

/**
 * @brief Initialize quick network
 * @return 0 on success, other values on error
 */
int quick_network_init(void);

/**
 * @brief Deinitialize quick network
 */
void quick_network_deinit(void);

/**
 * @brief Wait for network config
 * @param comm_pref_type Communication preference type
 * @return 0 on success, other values on error
 */
int quick_network_wait_config(qs_comm_pref_type_t *comm_pref_type);

/**
 * @brief MQTT task parameter
 */
typedef struct {
    void *data;
    size_t data_len;
    uint8_t cmd;  // 0: publish, 0xff: stop mqtt thread
    uint8_t is_wait_pub_ack;  // 0: no wait, 1: wait
    void (*callback)(int result, void *param);
    void *callback_param;
} qs_mqtt_task_param_t;

/**
 * @brief Add MQTT task
 * @param mqtt_task_param MQTT task parameter
 * @return 0 on success, other values on error
 */
int quick_network_add_mqtt_task(qs_mqtt_task_param_t *mqtt_task_param);

/**
 * @brief Last MQTT thread outcome (init/connect path and error codes from stack).
 *        0 means connected path succeeded; non-zero matches aicam_error.h style codes.
 */
int quick_network_get_mqtt_result(void);

/**
 * @brief Switch remote wakeup mode (current only support wifi communication)
 * @return 0 on success, other values on error
 */
int quick_network_switch_remote_wakeup_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* __QUICK_NETWORK_H__ */
