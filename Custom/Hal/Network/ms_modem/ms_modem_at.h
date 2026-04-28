#ifndef __MS_MODEM_AT_H__
#define __MS_MODEM_AT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"

#define __DEF_MODEM_DBG__           (1)
#if __DEF_MODEM_DBG__ > 0
    #define MODEM_LOGE(fmt, ...)    printf("[%s: %d]"fmt"\r\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)
#else
    #define MODEM_LOGE(fmt, ...)
#endif
#if __DEF_MODEM_DBG__ > 1
    #define MODEM_LOGD(fmt, ...)    printf("[%s: %d]"fmt"\r\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)
#else
    #define MODEM_LOGD(fmt, ...)
#endif

/**
 * @brief MODEM component configuration parameters
 */
#define MODEM_AT_CMD_LEN_MAXIMUM			    (256)   /* Maximum AT command length */
#define MODEM_AT_RSP_LEN_MAXIMUM			    (256)   /* Maximum AT response length */
#define MODEM_AT_RSP_MAX_LINE_NUM               (16)     /* Maximum number of AT response lines */

#define MODEM_AT_TX_TIMEOUT_DEFAULT		        (500)   /* UART default send timeout */
#define MODEM_AT_TX_MUTEX_TAKE_TIMEOUT          (15000) /* AT command send mutex acquisition timeout */

#define MODEM_AT_RSP_DATA_QUEUE_DEPTH           (MODEM_AT_RSP_MAX_LINE_NUM * 2)     /* Response/report data queue depth */

#define MODEM_AT_CMD_RETRY_TIME			        (2)	    /* Maximum retry times for AT command sending */
#define MODEM_AT_CMD_INTERVAL_DELAY             (0)     /* Wait time between AT command sending (unit: milliseconds) */

/// @brief MODEM error code
typedef enum
{
    MODEM_OK = 0x00,
    MODEM_ERR_INVALID_ARG = -0xDF,
    MODEM_ERR_INVALID_STATE,
    MODEM_ERR_INVALID_SIZE,
    MODEM_ERR_NOT_SUPPORT,
    MODEM_ERR_UART_FAILED,
    MODEM_ERR_FAILED,
    MODEM_ERR_CHECK,
    MODEM_ERR_MUTEX,
    MODEM_ERR_FMT,
    MODEM_ERR_MEM,
    MODEM_ERR_TIMEOUT,
    MODEM_ERR_HW_NOT_CNT,
    MODEM_ERR_UNKNOW,
} modem_err_t;

/**
 * @brief UART send function definition
 * @param[in] p_data Send data
 * @param[in] len Data length
 * @param[in] timeout Send timeout
 * @return Less than 0 is error code, others are actual bytes sent
 */
typedef int (*at_uart_tx_func_t)(const uint8_t *p_data, uint16_t len, uint32_t timeout_ms);

/**
 * @brief User processing callback function prototype definition after receiving response data
 * @param[in] handle     MODEM module handle
 * @param[in] res		AT command execution result
 * @param[in] rsp_list  Response data list
 * @param[in] rsp_num   Number of response data lines
 * @param[in] user_data User custom data
 * @return Error code
 */
typedef int (*at_rsp_handler_t)(void *handle, int res, char **rsp_list, uint16_t rsp_num, void *user_data);

/**
 * @brief AT command request response structure definition
 */
typedef struct {
	char *cmd;              // AT command to send
	uint16_t cmd_len;       // AT command data length (0 means use strlen function to get)
    uint32_t timeout_ms;    // Response timeout
    void *user_data;        // User data

    uint16_t expect_rsp_line;                       // Expected number of response lines
	char *expect_rsp[MODEM_AT_RSP_MAX_LINE_NUM];    // Expected response data content
	at_rsp_handler_t handler;                       // User processing callback function after receiving response data
} at_cmd_item_t;

/**
 * @brief Receive data parser structure
 */
typedef struct {
	uint8_t rsp_buf[MODEM_AT_RSP_LEN_MAXIMUM];
    uint16_t rsp_len;
    uint8_t cr_flag;
} at_rx_parser_t;

typedef enum {
    MODEM_AT_STATE_UNINIT = 0,
    MODEM_AT_STATE_INIT,
} modem_at_state_t;

/**
 * @brief MODEM component context data structure
 */
typedef struct {
    uint8_t is_filter_echo;                     // Filter echo flag
	modem_at_state_t state;                     // Component state
    at_rx_parser_t rx_parser;                   // Receive data parser
    QueueHandle_t rsp_queue;                    // AT response queue

	at_uart_tx_func_t uart_tx_func;             // UART send function definition
    SemaphoreHandle_t uart_tx_mutex;            // UART send lock
} modem_at_handle_t;

int modem_at_init(modem_at_handle_t *handle, at_uart_tx_func_t uart_tx_func);
void modem_at_deinit(modem_at_handle_t *handle);
int modem_at_cmd_exec_with_opt(modem_at_handle_t *handle, const at_cmd_item_t *cmd_item, uint8_t is_lock, uint8_t is_release_lock);
int modem_at_cmd_exec(modem_at_handle_t *handle, const at_cmd_item_t *cmd);
int modem_at_cmd_list_exec(modem_at_handle_t *handle, const at_cmd_item_t *cmd_list, uint16_t cmd_num);

int modem_at_rx_deal_handler(modem_at_handle_t *handle, const uint8_t *p_data, uint16_t len, uint32_t timeout_ms);
int modem_at_test(modem_at_handle_t *handle, uint8_t *is_ate1, uint32_t timeout_ms);
int modem_at_set_echo_filter(modem_at_handle_t *handle, uint8_t is_filter);

int modem_at_cmd_wait_ok(modem_at_handle_t *handle, const char *cmd, uint32_t timeout_ms);
int modem_at_cmd_wait_str(modem_at_handle_t *handle, const char *cmd, const char *rsp_str, uint32_t timeout_ms);
int modem_at_cmd_wait_rsp(modem_at_handle_t *handle, const char *cmd, char **rsp_list, uint16_t rsp_num, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* __MS_MODEM_AT_H__ */
