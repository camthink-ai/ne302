#ifndef __MS_MQTT_CLIENT_H__
#define __MS_MQTT_CLIENT_H__

#ifdef __cplusplus
    extern "C" {
#endif

#include <stdint.h>
#include "FreeRTOS.h"
#include "event_groups.h"
#include "semphr.h"
#include "task.h"

#define MS_MQTT_CLIENT_IS_DEBUG                     (0)
#define MS_MQTT_CLIENT_PING_TRY_COUNT               (3)
#define MS_MQTT_CLIENT_MAX_EVENT_FUNC_SIZE          (3)
#define MS_MQTT_CLIENT_TASK_BLOCK_TICK              (100)
#define MS_MQTT_CLIENT_MAX_CERT_DATA_SIZE           (32 * 1024)

/// @brief MQTT client
typedef struct ms_mqtt_client *ms_mqtt_client_handle_t;

/// @brief MQTT configuration
typedef struct {
    struct base_t {
        uint8_t protocol_ver;       // Protocol version (3 = 3.1, 4 = 3.1.1)
        char *hostname;             // Server address
        uint16_t port;              // Port
        char *client_id;            // Client ID
        uint8_t clean_session;      // Whether to clear session
        int keepalive;              // Keepalive time (unit: seconds)
    } base;
    
    struct authentication_t {
        char *username;             // Username
        char *password;             // Password

        char *ca_path;              // Server CA certificate path (preferred if not empty) (not supported yet)
        char *ca_data;              // Server CA certificate data
        size_t ca_len;              // Server CA certificate length (if 0, use strlen)

        char *client_cert_path;     // Client certificate path (preferred if not empty) (not supported yet)
        char *client_cert_data;     // Client certificate data
        size_t client_cert_len;     // Client certificate length (if 0, use strlen)

        char *client_key_path;      // Client key path (preferred if not empty) (not supported yet)
        char *client_key_data;      // Client key data
        size_t client_key_len;      // Client key length (if 0, use strlen)

        uint8_t is_verify_hostname; // Whether to verify hostname
    } authentication;
    
    struct last_will_t {
        char *topic;                // Last will topic
        char *msg;                  // Last will message
        int msg_len;                // Message length (if 0, use strlen)
        int qos;                    // Message QoS
        int retain;                 // Message retain flag
    } last_will;
    
    struct task_t {
        int priority;        // Task priority
        int stack_size;      // Task stack size
    } task;
    
    struct network_t {
        uint8_t disable_auto_reconnect; // Whether to disable auto reconnect
        uint8_t outbox_limit;           // Retransmission packet count limit
        int outbox_resend_interval_ms;  // Retransmission packet interval
        int outbox_expired_timeout;     // Retransmission packet expiration timeout
        int reconnect_interval_ms;      // Reconnect interval
        int timeout_ms;                 // Network operation timeout
        int buffer_size;                // Send/receive buffer size
        int tx_buf_size;                // Transmit buffer size (priority over buffer_size, use buffer_size if 0)
        int rx_buf_size;                // Receive buffer size (priority over buffer_size, use buffer_size if 0)
    } network;
} ms_mqtt_config_t;

/// @brief MQTT error code
typedef enum
{
    MQTT_ERR_OK = 0,
    MQTT_ERR_FAILED = -1,
    MQTT_ERR_INVALID_ARG = -2,
    MQTT_ERR_INVALID_STATE = -3,
    MQTT_ERR_TIMEOUT = -4,
    MQTT_ERR_CONN = -8,
    MQTT_ERR_SEND = -9,
    MQTT_ERR_RECV = -10,
    MQTT_ERR_MEM = -15,
    MQTT_ERR_SERIAL = -16,
    MQTT_ERR_DESERIAL = -17,
    MQTT_ERR_SIZE = -18,
    MQTT_ERR_RESPONSE = -19,
    MQTT_ERR_LIMIT = -20,
    MQTT_ERR_FILE = -21,
    MQTT_ERR_NETIF = -22,
    MQTT_ERR_UNKNOWN = -0xff,
} mqtt_error_t;

/// @brief MQTT state
typedef enum {
    MQTT_STATE_STOPPED = 0,
    MQTT_STATE_STARTING,
    MQTT_STATE_DISCONNECTED,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_WAIT_RECONNECT,
    MQTT_STATE_MAX,
} ms_mqtt_state_t;

/// @brief MQTT event ID
typedef enum {
    MQTT_EVENT_ANY = -1,
    MQTT_EVENT_ERROR = 0,
    MQTT_EVENT_STARTED,
    MQTT_EVENT_STOPPED,
    MQTT_EVENT_CONNECTED,
    MQTT_EVENT_DISCONNECTED,
    MQTT_EVENT_SUBSCRIBED,
    MQTT_EVENT_UNSUBSCRIBED,
    MQTT_EVENT_PUBLISHED,
    MQTT_EVENT_DATA,
    MQTT_EVENT_BEFORE_CONNECT, 
    MQTT_EVENT_DELETED,
    MQTT_EVENT_USER,            
} ms_mqtt_event_id_t;

/// @brief MQTT event data
typedef struct ms_mqtt_event_data_t {
    ms_mqtt_event_id_t event_id;    
    ms_mqtt_client_handle_t client;
    int error_code;
    uint8_t *data;
    int data_len;
    uint8_t *topic;
    int topic_len;
    uint16_t msg_id;
    uint8_t session_present;
    uint8_t connect_rsp_code;
    uint8_t retain;
    int qos;
    uint8_t dup;
} ms_mqtt_event_data_t;

/// @brief MQTT topic
typedef struct topic_t {
    char *filter;
    int qos;
} ms_mqtt_topic_t;

/// @brief MQTT event callback function prototype
typedef void (*ms_mqtt_client_event_handler_t)(ms_mqtt_event_data_t *event_data, void *user_args);

/// @brief Duplicate string
/// @param s Source string
/// @return Duplicated string
char *ms_strdup(const char *s);

/// @brief Load certificate data from file
/// @param cert_path Certificate path
/// @param cert_data Certificate data
/// @param cert_len Certificate length
/// @return Error code
int ms_mqtt_client_get_cert_from_file(const char *cert_path, uint8_t **cert_data, uint32_t *cert_len);

/// @brief Initialize MQTT client
/// @param config MQTT configuration
/// @return Client handle on success, NULL on failure
ms_mqtt_client_handle_t ms_mqtt_client_init(const ms_mqtt_config_t *config);

/// @brief Start MQTT client
/// @param client Client handle
/// @return Error code
int ms_mqtt_client_start(ms_mqtt_client_handle_t client);

/// @brief Notify client to reconnect
/// @param client Client handle
/// @return Error code
int ms_mqtt_client_reconnect(ms_mqtt_client_handle_t client);

/// @brief Notify client to disconnect (will not reconnect)
/// @param client Client handle
/// @return Error code
int ms_mqtt_client_disconnect(ms_mqtt_client_handle_t client);

/// @brief Stop MQTT client
/// @param client Client handle
/// @return Error code
int ms_mqtt_client_stop(ms_mqtt_client_handle_t client);

/// @brief Subscribe to single topic
/// @param client Client handle
/// @param topic Topic address
/// @param qos Maximum QoS
/// @return Message ID if >= 0, error code if < 0
int ms_mqtt_client_subscribe_single(ms_mqtt_client_handle_t client, char *topic, int qos);

/// @brief Subscribe to multiple topics
/// @param client Client handle
/// @param topic_list Topic address list
/// @param size Count
/// @return Message ID if >= 0, error code if < 0
int ms_mqtt_client_subscribe_multiple(ms_mqtt_client_handle_t client, const ms_mqtt_topic_t *topic_list, int size);

/// @brief Unsubscribe from topic
/// @param client Client handle
/// @param topic Topic address
/// @return Message ID if >= 0, error code if < 0
int ms_mqtt_client_unsubscribe(ms_mqtt_client_handle_t client, char *topic);

/// @brief Publish message
/// @param client Client handle
/// @param topic Topic address
/// @param data Message data
/// @param len Message length
/// @param qos Publish QoS
/// @param retain Whether to retain
/// @return Message ID if >= 0, error code if < 0
int ms_mqtt_client_publish(ms_mqtt_client_handle_t client, char *topic, uint8_t *data, int len, int qos, int retain);

/// @brief Publish message (asynchronous)
/// @param client Client handle
/// @param topic Topic address
/// @param data Message data
/// @param len Message length
/// @param qos Publish QoS
/// @param retain Whether to retain
/// @return Message ID if >= 0, error code if < 0
int ms_mqtt_client_enqueue(ms_mqtt_client_handle_t client, char *topic, uint8_t *data, int len, int qos, int retain);

/// @brief Destroy MQTT client
/// @param client Client handle
/// @return Error code
int ms_mqtt_client_destroy(ms_mqtt_client_handle_t client);

/// @brief Register event callback function
/// @param client Client handle
/// @param event_handler Event callback function
/// @param user_arg User data
/// @return Error code
int ms_mqtt_client_register_event(ms_mqtt_client_handle_t client, ms_mqtt_client_event_handler_t event_handler, void *user_arg);

/// @brief Unregister event callback function
/// @param client Client handle
/// @return Error code
int ms_mqtt_client_unregister_event(ms_mqtt_client_handle_t client, ms_mqtt_client_event_handler_t event_handler);

/// @brief Get unsent message count
/// @param client Client handle
/// @return Unsent message count
int ms_mqtt_client_get_outbox_size(ms_mqtt_client_handle_t client);

/// @brief Get MQTT state
/// @param client Client handle
/// @return MQTT state
ms_mqtt_state_t ms_mqtt_client_get_state(ms_mqtt_client_handle_t client);

#ifdef __cplusplus
}
#endif

#endif /* __MS_MQTT_CLIENT_H__ */
