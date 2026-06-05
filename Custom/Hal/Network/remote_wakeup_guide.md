# Remote Wake-up Sleep Interface Usage Guide

## 1. Overview

This document describes how to use the remote wake-up sleep function on the NE302 platform. This function allows the device to enter low-power sleep mode and receive remote wake-up commands through the MQTT protocol, enabling remote control of the device.

The remote wake-up process mainly involves the following modules:
- **Network Interface Management Module** (`netif_manager`): Configure WiFi network card and remote wake-up mode
- **MQTT Client Module** (`si91x_mqtt_client`): Establish MQTT connection and subscribe to wake-up topics
- **U0 Module** (`u0_module`): Control device to enter sleep mode

---

## 2. Prerequisites

### 2.1 Hardware Requirements
- NE302 platform hardware
- WiFi chip (SI91X) working normally
- U0 module initialized and working normally

### 2.2 Software Requirements
- Network interface manager registered: `netif_manager_register()`
- U0 module registered: `u0_module_register()`
- MQTT client library integrated

---

## 3. Complete Remote Wake-up Sleep Process

### 3.1 Flow Diagram

```
┌─────────────────────────────────────────────────────────┐
│ 1. Configure WiFi Network Card (ifconfig wl cfg [ssid] [pw])          │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│ 2. Configure Remote Wake-up Mode (ifconfig wl rmode)                 │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│ 3. Initialize MQTT Client (si91x_mqtt init [args...])        │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│ 4. Connect to MQTT Server (si91x_mqtt connect)                 │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│ 5. Subscribe to Wake-up Topic (si91x_mqtt sub [topic] [qos])        │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│ 6. Configure Low Power Mode (ifconfig wl lpwr)                  │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│ 7. Enter Sleep Mode (u0 sleep 0 3v3 wifi)                 │
└────────────────────┬────────────────────────────────────┘
                     │
                     │ Waiting for remote wake-up...
                     │
┌────────────────────▼────────────────────────────────────┐
│ Device receives MQTT wake-up message, automatically wakes up                          │
└─────────────────────────────────────────────────────────┘
```

---

## 4. Detailed C Language Interface Description

### 4.1 Step 1: Configure WiFi Network Card

**Function Description:**
Configure the SSID and password of the WiFi network card so it can connect to the target WiFi network. **Important: Before configuration, you must first get the current configuration, then modify it based on the existing configuration to avoid overwriting other configuration items.**

**Related APIs:**
```c
// Get network card configuration
int nm_get_netif_cfg(const char *if_name, netif_config_t *netif_cfg);

// Set network card configuration
int nm_set_netif_cfg(const char *if_name, netif_config_t *netif_cfg);

// Get network card status (optional, for querying connection status)
netif_state_t nm_get_netif_state(const char *if_name);
```

**Configuration Structure:**
```c
typedef struct {
    wireless_config_t wireless_cfg;  // Wireless configuration
    netif_ip_mode_t ip_mode;         // IP mode
    uint8_t ip_addr[4];              // IP address (static IP mode)
    uint8_t netmask[4];             // Subnet mask
    uint8_t gw[4];                  // Gateway address
    char *host_name;                 // Host name
} netif_config_t;

typedef struct {
    char ssid[NETIF_SSID_VALUE_SIZE];      // SSID
    char pw[NETIF_PW_VALUE_SIZE];          // Password
    wireless_security_t security;           // Security type
    wireless_encryption_t encryption;      // Encryption method
    uint8_t channel;                       // Channel
    uint8_t max_client_num;                // Maximum number of clients (AP mode)
} wireless_config_t;
```

**Usage Example:**
```c
int ret = 0;
netif_config_t netif_cfg = {0};

// Step 1: First get current configuration
ret = nm_get_netif_cfg(NETIF_NAME_WIFI_STA, &netif_cfg);
if (ret != 0) {
    LOG_SIMPLE("Failed to get WiFi config: %d\r\n", ret);
    return ret;
}

// Step 2: Modify wireless configuration based on existing configuration
strncpy(netif_cfg.wireless_cfg.ssid, "MyWiFiNetwork", sizeof(netif_cfg.wireless_cfg.ssid) - 1);
strncpy(netif_cfg.wireless_cfg.pw, "mypassword", sizeof(netif_cfg.wireless_cfg.pw) - 1);
netif_cfg.wireless_cfg.security = WIRELESS_WPA_WPA2_MIXED;
netif_cfg.ip_mode = NETIF_IP_MODE_DHCP;  // Keep or modify IP mode

// Step 3: Apply configuration
ret = nm_set_netif_cfg(NETIF_NAME_WIFI_STA, &netif_cfg);
if (ret != 0) {
    LOG_SIMPLE("Failed to set WiFi config: %d\r\n", ret);
    return ret;
}

```

**Notes:**
- **Must** first call `nm_get_netif_cfg()` to get current configuration, then modify the fields that need to be changed
- Creating a new configuration structure directly will lose other configuration items (such as IP configuration, host name, etc.)

**Command Line Reference:** `ifconfig wl cfg [ssid] [pw]`

---

### 4.2 Step 2: Configure Remote Wake-up Mode

**Function Description:**
Configure the WiFi chip to remote wake-up single STA mode.

**Related API:**
```c
// Configure remote wake-up mode
int sl_net_netif_romote_wakeup_mode_ctrl(uint8_t enable);
```

**Parameter Description:**
- `enable`: Enable flag (1: enable remote wake-up mode, 0: disable)

**Return Value:**
- `0`: Success
- Negative number: Failure

**Usage Example:**
```c
int ret = 0;

// Enable remote wake-up mode
ret = sl_net_netif_romote_wakeup_mode_ctrl(1);
if (ret != 0) {
    LOG_SIMPLE("Failed to enable remote wakeup mode: %d\r\n", ret);
    return ret;
}
```

**Notes:**
- Must configure this mode before MQTT initialization
- After configuration, the WiFi chip will enter a special single STA mode and will not register the network card

**Command Line Reference:** `ifconfig wl rmode [enable]`

---

### 4.3 Step 3: Initialize MQTT Client

**Function Description:**
Initialize the MQTT protocol stack inside the SI91X chip and configure MQTT server connection parameters.

**Related API:**
```c
// Initialize MQTT client
int si91x_mqtt_client_init(const ms_mqtt_config_t *config);
```

**Configuration Structure:**
```c
typedef struct {
    struct base_t {
        const char *hostname;      // MQTT server address
        uint16_t port;             // MQTT server port
        const char *client_id;     // Client ID
        uint16_t keepalive;        // Keep-alive time (seconds)
    } base;
    
    struct authentication_t {
        const char *username;      // Username
        const char *password;      // Password
        const uint8_t *ca_data;    // CA certificate data (TLS)
        uint32_t ca_len;           // CA certificate length
        const char *ca_path;       // CA certificate path
        const uint8_t *client_cert_data;  // Client certificate data
        uint32_t client_cert_len;   // Client certificate length
        const uint8_t *client_key_data;  // Client key data
        uint32_t client_key_len;    // Client key length
        uint8_t is_verify_hostname; // Whether to verify hostname
    } authentication;
    
    struct network_t {
        uint32_t timeout_ms;       // Timeout (milliseconds)
        uint32_t reconnect_interval_ms;  // Reconnection interval
        uint8_t disable_auto_reconnect;  // Disable auto-reconnect
    } network;
} ms_mqtt_config_t;
```

**Usage Example:**
```c
int ret = 0;
ms_mqtt_config_t mqtt_config = {0};
static const char *mqtt_hostname = "mqtt.example.com";
static const char *mqtt_client_id = "device_001";
static const char *mqtt_username = "user001";
static const char *mqtt_password = "pass001";

// Configure MQTT parameters
mqtt_config.base.hostname = mqtt_hostname;
mqtt_config.base.port = 1883;
mqtt_config.base.client_id = mqtt_client_id;
mqtt_config.base.keepalive = 60;
mqtt_config.authentication.username = mqtt_username;
mqtt_config.authentication.password = mqtt_password;
mqtt_config.network.timeout_ms = 5000;
mqtt_config.network.reconnect_interval_ms = 5000;
mqtt_config.network.disable_auto_reconnect = 0;

// Initialize MQTT client
ret = si91x_mqtt_client_init(&mqtt_config);
if (ret != MQTT_ERR_OK) {
    LOG_SIMPLE("Failed to init MQTT client: %d\r\n", ret);
    return ret;
}
```

**Return Value:**
- `MQTT_ERR_OK` (0): Success
- Negative number: Failure (see `mqtt_error_t` for specific error codes)

**Notes:**
- Ensure network is connected before initialization
- If using TLS encryption, need to configure corresponding certificate data
- Client ID must be unique to avoid conflicts with other devices
- String pointers in the configuration structure need to point to valid memory, recommend using static strings or global variables

**Command Line Reference:** `si91x_mqtt init [hostname] [port] [client_id] [username] [password]`

---

### 4.4 Step 4: Connect to MQTT Server

**Function Description:**
Connect to the MQTT server. **It is recommended to use the synchronous interface**, wait for the connection to complete before proceeding with subsequent operations.

**Related APIs:**
```c
// Connect to MQTT server (synchronous, recommended)
int si91x_mqtt_client_connnect_sync(uint32_t timeout_ms);

// Get MQTT status
ms_mqtt_state_t si91x_mqtt_client_get_state(void);
```

**Parameter Description:**
- `timeout_ms`: Connection timeout (milliseconds), recommended to set to 5000-10000ms

**Return Value:**
- `SL_STATUS_OK` (0): Connection successful
- Other values: Connection failed (see `sl_status_t` for specific error codes)

**Usage Example:**
```c
int ret = 0;
ms_mqtt_state_t mqtt_state;

// Use synchronous interface to connect to MQTT server
ret = si91x_mqtt_client_connnect_sync(5000);
if (ret != 0) {
    LOG_SIMPLE("Failed to connect MQTT server: 0x%08X\r\n", ret);
    return ret;
}

// Optional: Verify connection status
mqtt_state = si91x_mqtt_client_get_state();
if (mqtt_state != MQTT_STATE_CONNECTED) {
    LOG_SIMPLE("MQTT connection state error: %d\r\n", mqtt_state);
    return -1;
}

LOG_SIMPLE("MQTT connected successfully\r\n");
```

**MQTT State Enumeration:**
```c
typedef enum {
    MQTT_STATE_STOPPED = 0,        // Stopped
    MQTT_STATE_STARTING,           // Starting
    MQTT_STATE_DISCONNECTED,       // Disconnected
    MQTT_STATE_CONNECTED,          // Connected
    MQTT_STATE_WAIT_RECONNECT,     // Waiting for reconnection
} ms_mqtt_state_t;
```

**Notes:**
- **It is recommended to use the synchronous interface** for easier error handling and status confirmation
- Connection timeout is recommended to be set to 5-10 seconds
- When connection fails, check network connection and server configuration
- After successful connection, topic subscription should be performed immediately

**Command Line Reference:** `si91x_mqtt connect`

---

### 4.5 Step 5: Subscribe to Wake-up Topic

**Function Description:**
Subscribe to an MQTT topic for receiving remote wake-up messages. When a message is published to this topic, the device will receive a notification and handle it through an event callback.

**Related APIs:**
```c
// Subscribe to topic (synchronous, recommended)
int si91x_mqtt_client_subscribe_sync(const char *topic, int qos, uint32_t timeout_ms);

// Unsubscribe (synchronous)
int si91x_mqtt_client_unsubscribe_sync(const char *topic, uint32_t timeout_ms);
```

**Parameter Description:**
- `topic`: Topic name to subscribe to (required)
- `qos`: Quality of Service level (0/1/2, recommended to use 1)
- `timeout_ms`: Subscription timeout (milliseconds), recommended to set to 3000-5000ms

**Return Value:**
- `SL_STATUS_OK` (0): Subscription successful
- Other values: Subscription failed (see `sl_status_t` for specific error codes)

**QoS Level Description:**
- **QoS 0**: At most once delivery, no guarantee of message delivery (not recommended for wake-up)
- **QoS 1**: At least once delivery, guarantee message is delivered at least once (**recommended for wake-up**)
- **QoS 2**: Exactly once delivery, guarantee message is delivered exactly once (highest reliability, but higher overhead)

**Usage Example:**
```c
int ret = 0;
const char *wakeup_topic = "device/001/wakeup";

// Use synchronous interface to subscribe to wake-up topic
ret = si91x_mqtt_client_subscribe_sync(wakeup_topic, 1, 3000);
if (ret != 0) {
    LOG_SIMPLE("Failed to subscribe topic: 0x%08X\r\n", ret);
    return ret;
}

LOG_SIMPLE("Subscribed to topic: %s\r\n", wakeup_topic);
```

**Notes:**
- **Use synchronous interface** for easier error handling and status confirmation
- Topic name should be consistent with the server side
- **It is recommended to use QoS 1** to ensure reliable delivery of wake-up messages
- Must ensure MQTT is successfully connected before subscription
- After successful subscription, the device can receive wake-up messages for this topic

**Command Line Reference:** `si91x_mqtt sub [topic] [qos]`

---

### 4.6 Step 6: Configure Low Power Mode

**Function Description:**
Configure the WiFi chip to low power mode.

**Related API:**
```c
// Configure low power mode
int sl_net_netif_low_power_mode_ctrl(uint8_t enable);
```

**Parameter Description:**
- `enable`: Enable flag (1: enable low power mode, 0: disable)

**Return Value:**
- `0`: Success
- Negative number: Failure

**Usage Example:**
```c
int ret = 0;

// Enable low power mode
ret = sl_net_netif_low_power_mode_ctrl(1);
if (ret != 0) {
    LOG_SIMPLE("Failed to enable low power mode: %d\r\n", ret);
    return ret;
}
```

**Notes:**
- Low power mode will reduce WiFi performance but can significantly reduce power consumption
- Must enable low power mode before entering sleep
- Ensure MQTT is connected and topic is subscribed before enabling

**Command Line Reference:** `ifconfig wl lpwr [enable]`

---

### 4.7 Step 7: Enter Sleep Mode

**Function Description:**
Control the device to enter sleep mode, waiting for remote wake-up or timed wake-up. During sleep, keep WiFi and 3V3 power on to support remote wake-up function.

**Related APIs:**
```c
// Enter sleep mode
int u0_module_enter_sleep_mode(uint32_t wakeup_flag, uint32_t switch_bits, uint32_t sleep_second);

// Enter sleep mode (extended, supports RTC alarm)
int u0_module_enter_sleep_mode_ex(uint32_t wakeup_flag, uint32_t switch_bits, 
                                  uint32_t sleep_second, 
                                  ms_bridging_alarm_t *rtc_alarm_a, 
                                  ms_bridging_alarm_t *rtc_alarm_b);
```

**Parameter Description:**
- `wakeup_flag`: Wake-up flag bits, specifying allowed wake-up sources (see definitions below)
- `switch_bits`: Power switch bits to keep on during sleep (see definitions below)
- `sleep_second`: Timed wake-up time (seconds), 0 means timed wake-up is not enabled

**Return Value:**
- `0`: Success, device will enter sleep
- Negative number: Failure

**Wake-up Flag Bit Definitions:**
```c
#define PWR_WAKEUP_FLAG_STANDBY         (1 << 0)   // Standby mode wake-up
#define PWR_WAKEUP_FLAG_STOP2           (1 << 1)   // Stop2 mode wake-up
#define PWR_WAKEUP_FLAG_RTC_TIMING      (1 << 2)   // RTC timed wake-up
#define PWR_WAKEUP_FLAG_RTC_ALARM_A     (1 << 3)   // RTC alarm A wake-up
#define PWR_WAKEUP_FLAG_RTC_ALARM_B     (1 << 4)   // RTC alarm B wake-up
#define PWR_WAKEUP_FLAG_CONFIG_KEY      (1 << 5)   // Config key wake-up
#define PWR_WAKEUP_FLAG_PIR_HIGH        (1 << 6)   // PIR high level wake-up
#define PWR_WAKEUP_FLAG_PIR_LOW         (1 << 7)   // PIR low level wake-up
#define PWR_WAKEUP_FLAG_PIR_RISING      (1 << 8)   // PIR rising edge wake-up
#define PWR_WAKEUP_FLAG_PIR_FALLING     (1 << 9)   // PIR falling edge wake-up
#define PWR_WAKEUP_FLAG_SI91X           (1 << 10)  // WiFi wake-up
#define PWR_WAKEUP_FLAG_NET             (1 << 11)  // Network wake-up
#define PWR_WAKEUP_FLAG_WUFI            (1 << 27)  // WUFI wake-up
#define PWR_WAKEUP_FLAG_VALID           (1 << 31)  // Wake-up flag valid
```

**Power Switch Bit Definitions:**
```c
#define PWR_3V3_SWITCH_BIT              (1 << 0)   // 3.3V power switch
#define PWR_WIFI_SWITCH_BIT             (1 << 1)   // WiFi power switch
#define PWR_AON_SWITCH_BIT              (1 << 2)   // Always-On power switch
#define PWR_N6_SWITCH_BIT               (1 << 3)   // N6 chip power switch
#define PWR_EXT_SWITCH_BIT              (1 << 4)   // External power switch
#define PWR_ALL_SWITCH_BIT              (PWR_3V3_SWITCH_BIT | PWR_WIFI_SWITCH_BIT | \
                                         PWR_AON_SWITCH_BIT | PWR_N6_SWITCH_BIT | \
                                         PWR_EXT_SWITCH_BIT)  // All power switches
```

**Usage Example:**
```c
int ret = 0;
uint32_t wakeup_flags = 0;
uint32_t switch_bits = 0;
uint32_t sleep_second = 0;  // 0 means timed wake-up is not enabled

// Configure wake-up flags: WiFi wake-up + config key wake-up
wakeup_flags = PWR_WAKEUP_FLAG_SI91X | PWR_WAKEUP_FLAG_CONFIG_KEY;

// If RTC timed wake-up is needed, add timed wake-up flag
// wakeup_flags |= PWR_WAKEUP_FLAG_RTC_TIMING;
// sleep_second = 3600;  // Timed wake-up after 3600 seconds

// Configure power switches: keep 3v3 and wifi power (required for remote wake-up)
switch_bits = PWR_3V3_SWITCH_BIT | PWR_WIFI_SWITCH_BIT;

// Enter sleep mode
ret = u0_module_enter_sleep_mode(wakeup_flags, switch_bits, sleep_second);
if (ret != 0) {
    LOG_SIMPLE("Failed to enter sleep mode: %d\r\n", ret);
    return ret;
}

// Device enters sleep, waiting for remote wake-up
// This line of code will not be executed because the device has entered sleep
LOG_SIMPLE("Device entered sleep mode, waiting for remote wakeup...\r\n");
```

**Sleep Mode Description:**
- If `switch_bits` is specified or `sleep_second > 0xFFFF` or includes PIR wake-up flags, the device enters **STOP2 mode**
- Otherwise enters **STANDBY mode**

**Automatic Wake-up Flag Setting:**
- When keeping `PWR_WIFI_SWITCH_BIT` or `PWR_3V3_SWITCH_BIT`, should add `PWR_WAKEUP_FLAG_SI91X` (WiFi wake-up)
- When keeping `PWR_EXT_SWITCH_BIT` or `PWR_3V3_SWITCH_BIT`, can add `PWR_WAKEUP_FLAG_NET` (network wake-up)
- When setting `sleep_second > 0`, should add `PWR_WAKEUP_FLAG_RTC_TIMING` (RTC timed wake-up)
- It is recommended to add `PWR_WAKEUP_FLAG_CONFIG_KEY` (config key wake-up)

**Notes:**
- Remote wake-up **must** keep `PWR_3V3_SWITCH_BIT` and `PWR_WIFI_SWITCH_BIT`
- Ensure MQTT is connected and wake-up topic is subscribed before sleep
- Timed wake-up and remote wake-up can be used simultaneously
- Device will automatically resume running state after wake-up
- After successful function call, the device immediately enters sleep, subsequent code will not execute

**Command Line Reference:** `u0 sleep <sleep_second> [name1] [name2] ... [nameN]`

---

## 5. Complete C Code Example

```c
#include "netif_manager.h"
#include "si91x_mqtt_client.h"
#include "u0_module.h"
#include "ms_mqtt_client.h"
#include "cmsis_os.h"

// MQTT configuration parameters (recommended to define as static variables or read from configuration)
static const char *mqtt_hostname = "mqtt.example.com";
static const char *mqtt_client_id = "device_001";
static const char *mqtt_username = "user001";
static const char *mqtt_password = "pass001";
static const char *wakeup_topic = "device/001/wakeup";

// WiFi configuration parameters
static const char *wifi_ssid = "MyWiFiNetwork";
static const char *wifi_password = "mypassword";

/**
 * @brief Remote wake-up sleep configuration function
 * @return 0 on success, negative on failure
 */
int remote_wakeup_sleep_config(void)
{
    int ret = 0;
    netif_config_t netif_cfg = {0};
    ms_mqtt_config_t mqtt_config = {0};
    uint32_t wakeup_flags = 0;
    uint32_t switch_bits = 0;
    
    // ========== Step 1: Configure WiFi Network Card ==========
    // Important: First get current configuration
    ret = nm_get_netif_cfg(NETIF_NAME_WIFI_STA, &netif_cfg);
    if (ret != 0) {
        LOG_SIMPLE("Failed to get WiFi config: %d\r\n", ret);
        return ret;
    }
    
    // Modify wireless configuration based on existing configuration
    strncpy(netif_cfg.wireless_cfg.ssid, wifi_ssid, sizeof(netif_cfg.wireless_cfg.ssid) - 1);
    strncpy(netif_cfg.wireless_cfg.pw, wifi_password, sizeof(netif_cfg.wireless_cfg.pw) - 1);
    netif_cfg.wireless_cfg.security = WIRELESS_WPA_WPA2_MIXED;
    netif_cfg.ip_mode = NETIF_IP_MODE_DHCP;  // Keep or modify IP mode
    
    // Apply configuration (after configuration, network card will automatically connect, no need to manually init and up)
    ret = nm_set_netif_cfg(NETIF_NAME_WIFI_STA, &netif_cfg);
    if (ret != 0) {
        LOG_SIMPLE("Failed to set WiFi config: %d\r\n", ret);
        return ret;
    }
    LOG_SIMPLE("WiFi config applied\r\n");
    
    // Optional: Wait for WiFi connection success
    // while (nm_get_netif_state(NETIF_NAME_WIFI_STA) != NETIF_STATE_UP) {
    //     osDelay(100);
    // }
    
    // ========== Step 2: Configure Remote Wake-up Mode ==========
    ret = sl_net_netif_romote_wakeup_mode_ctrl(1);
    if (ret != 0) {
        LOG_SIMPLE("Failed to enable remote wakeup mode: %d\r\n", ret);
        return ret;
    }
    LOG_SIMPLE("Remote wakeup mode enabled\r\n");
    
    // ========== Step 3: Initialize MQTT Client ==========
    mqtt_config.base.hostname = mqtt_hostname;
    mqtt_config.base.port = 1883;
    mqtt_config.base.client_id = mqtt_client_id;
    mqtt_config.base.keepalive = 60;
    mqtt_config.authentication.username = mqtt_username;
    mqtt_config.authentication.password = mqtt_password;
    mqtt_config.network.timeout_ms = 5000;
    mqtt_config.network.reconnect_interval_ms = 5000;
    mqtt_config.network.disable_auto_reconnect = 0;
    
    ret = si91x_mqtt_client_init(&mqtt_config);
    if (ret != MQTT_ERR_OK) {
        LOG_SIMPLE("Failed to init MQTT client: %d\r\n", ret);
        return ret;
    }
    LOG_SIMPLE("MQTT client initialized\r\n");
    
    // ========== Step 4: Connect to MQTT Server (Synchronous) ==========
    ret = si91x_mqtt_client_connnect_sync(5000);
    if (ret != 0) {
        LOG_SIMPLE("Failed to connect MQTT: 0x%08X\r\n", ret);
        return ret;
    }
    LOG_SIMPLE("MQTT connected\r\n");
    
    // ========== Step 5: Subscribe to Wake-up Topic (Synchronous) ==========
    ret = si91x_mqtt_client_subscribe_sync(wakeup_topic, 1, 3000);
    if (ret != 0) {
        LOG_SIMPLE("Failed to subscribe topic: 0x%08X\r\n", ret);
        return ret;
    }
    LOG_SIMPLE("Subscribed to topic: %s\r\n", wakeup_topic);
    
    // ========== Step 6: Configure Low Power Mode ==========
    ret = sl_net_netif_low_power_mode_ctrl(1);
    if (ret != 0) {
        LOG_SIMPLE("Failed to enable low power mode: %d\r\n", ret);
        return ret;
    }
    LOG_SIMPLE("Low power mode enabled\r\n");
    
    // ========== Step 7: Enter Sleep Mode ==========
    // Configure wake-up flags: WiFi wake-up + config key wake-up
    wakeup_flags = PWR_WAKEUP_FLAG_SI91X | PWR_WAKEUP_FLAG_CONFIG_KEY;
    
    // Configure power switches: keep 3v3 and wifi power (required for remote wake-up)
    switch_bits = PWR_3V3_SWITCH_BIT | PWR_WIFI_SWITCH_BIT;
    
    // Enter sleep mode (0 means timed wake-up is not enabled)
    ret = u0_module_enter_sleep_mode(wakeup_flags, switch_bits, 0);
    if (ret != 0) {
        LOG_SIMPLE("Failed to enter sleep mode: %d\r\n", ret);
        return ret;
    }
    
    // Device enters sleep, waiting for remote wake-up
    // Note: This line of code will not be executed because the device has entered sleep
    LOG_SIMPLE("Device entered sleep mode, waiting for remote wakeup...\r\n");
    
    return 0;
}
```

---

## 6. Remote Wake-up Testing

### 6.1 Send Wake-up Message

After the device enters sleep, you can wake up the device by sending a message to the subscribed topic through the MQTT server:

**Using MQTT Client Tool (such as mosquitto_pub):**
```bash
mosquitto_pub -h mqtt.example.com -p 1883 -u user001 -P pass001 \
  -t device/001/wakeup -m "wakeup" -q 1
```

**Using Online MQTT Client:**
- Connect to MQTT server
- Publish message to topic: `device/001/wakeup`
- Message content: Any (such as "wakeup")
- QoS: 1 or 2 (recommended)

### 6.2 Wake-up Verification

After device wake-up should:
1. Automatically restore WiFi connection
2. Automatically restore MQTT connection
3. Resume normal running state
4. Can confirm wake-up success through logs or status query

---

## 7. Notes and Best Practices

### 7.1 Notes

1. **Order Requirements**
   - Must execute each step in the order specified in the document
   - Ensure the previous step succeeds before executing the next step

2. **Network Connection**
   - Need to wait for connection success after configuring WiFi
   - Use `nm_get_netif_state()` to query connection status

3. **MQTT Connection**
   - **Must use synchronous interface** `si91x_mqtt_client_connnect_sync()`
   - When connection fails, check network and server configuration

4. **Power Management**
   - Remote wake-up must keep `3v3` and `wifi` power
   - Other power modules can choose whether to keep them according to requirements

5. **Timed Wake-up**
   - Can use remote wake-up and RTC timed wake-up simultaneously
   - Timed wake-up time cannot exceed 65535 seconds

### 7.2 Best Practices

1. **Error Handling**
   - Each step should check return value
   - Record error logs and restore state on failure

2. **State Management**
   - Record device state (running/sleep)
   - Restore necessary configuration and connection after wake-up

3. **Topic Design**
   - Use unique device ID as part of the topic
   - Recommend using QoS 1 or 2 to ensure reliable message delivery

4. **Power Optimization**
   - Close unnecessary modules before sleep
   - Reasonably set timed wake-up time

5. **Testing and Verification**
   - Fully test wake-up function in development environment
   - Verify handling of various abnormal situations

---

## 8. Troubleshooting

### 8.1 Common Issues

**Issue 1: WiFi Connection Failure**
- Check if SSID and password are correct
- Confirm WiFi network signal strength
- Check network interface status

**Issue 2: MQTT Connection Failure**
- Check server address and port
- Verify username and password
- Confirm network connection is normal

**Issue 3: Cannot Receive Wake-up Messages**
- Confirm wake-up topic is subscribed
- Check MQTT connection status
- Verify message QoS level

**Issue 4: Device Cannot Wake Up**
- Confirm `3v3` and `wifi` power are kept during sleep
- Check wake-up flag configuration
- Verify MQTT message is successfully sent

### 8.2 Debug Interfaces

```c
// View WiFi status
netif_state_t nm_get_netif_state(const char *if_name);
void nm_print_netif_info(const char *if_name, netif_info_t *netif_info);

// View MQTT status
ms_mqtt_state_t si91x_mqtt_client_get_state(void);

// View power status
int u0_module_get_power_status(uint32_t *switch_bits);

// View wake-up flags
int u0_module_get_wakeup_flag(uint32_t *wakeup_flag);
```

---

## 9. API Reference

### 9.1 Network Interface Management API

```c
// Get network card configuration
int nm_get_netif_cfg(const char *if_name, netif_config_t *netif_cfg);

// Set network card configuration (network card will automatically connect after configuration)
int nm_set_netif_cfg(const char *if_name, netif_config_t *netif_cfg);

// Get network card status
netif_state_t nm_get_netif_state(const char *if_name);

// Configure remote wake-up mode
int sl_net_netif_romote_wakeup_mode_ctrl(uint8_t enable);

// Configure low power mode
int sl_net_netif_low_power_mode_ctrl(uint8_t enable);
```

### 9.2 MQTT Client API

```c
// Initialize MQTT client
int si91x_mqtt_client_init(const ms_mqtt_config_t *config);

// Connect to MQTT server (synchronous, recommended)
int si91x_mqtt_client_connnect_sync(uint32_t timeout_ms);

// Subscribe to topic (synchronous, recommended)
int si91x_mqtt_client_subscribe_sync(const char *topic, int qos, uint32_t timeout_ms);

// Unsubscribe (synchronous)
int si91x_mqtt_client_unsubscribe_sync(const char *topic, uint32_t timeout_ms);

// Disconnect (synchronous)
int si91x_mqtt_client_disconnect_sync(uint32_t timeout_ms);

// Get MQTT status
ms_mqtt_state_t si91x_mqtt_client_get_state(void);
```

### 9.3 U0 Module API

```c
// Enter sleep mode
int u0_module_enter_sleep_mode(uint32_t wakeup_flag, uint32_t switch_bits, uint32_t sleep_second);

// Enter sleep mode (extended, supports RTC alarm)
int u0_module_enter_sleep_mode_ex(uint32_t wakeup_flag, uint32_t switch_bits, 
                                  uint32_t sleep_second, 
                                  ms_bridging_alarm_t *rtc_alarm_a, 
                                  ms_bridging_alarm_t *rtc_alarm_b);

// Get power status
int u0_module_get_power_status(uint32_t *switch_bits);

// Get wake-up flags
int u0_module_get_wakeup_flag(uint32_t *wakeup_flag);
```

---

## 10. Version Information

- **Document Version**: V1.0
- **Creation Date**: 2025-01-XX
- **Applicable Platform**: NE302
- **Maintainer**: Development Team

---

## 11. Related Documents

- [MQTT Client Module Interface Description](./mqtt_client/si91x_mqtt_client.h)
- [Network Card Management Module Interface Description](./netif_manager/netif_manager.md)
- [U0 Module Interface Description](../u0_module.h)

---

## Appendix: Wake-up Flag Bit Definitions

```c
#define PWR_WAKEUP_FLAG_STANDBY         (1 << 0)   // Standby mode wake-up
#define PWR_WAKEUP_FLAG_STOP2           (1 << 1)   // Stop2 mode wake-up
#define PWR_WAKEUP_FLAG_RTC_TIMING      (1 << 2)   // RTC timed wake-up
#define PWR_WAKEUP_FLAG_RTC_ALARM_A     (1 << 3)   // RTC alarm A wake-up
#define PWR_WAKEUP_FLAG_RTC_ALARM_B     (1 << 4)   // RTC alarm B wake-up
#define PWR_WAKEUP_FLAG_CONFIG_KEY      (1 << 5)   // Config key wake-up
#define PWR_WAKEUP_FLAG_PIR_HIGH        (1 << 6)   // PIR high level wake-up
#define PWR_WAKEUP_FLAG_PIR_LOW         (1 << 7)   // PIR low level wake-up
#define PWR_WAKEUP_FLAG_PIR_RISING      (1 << 8)   // PIR rising edge wake-up
#define PWR_WAKEUP_FLAG_PIR_FALLING     (1 << 9)   // PIR falling edge wake-up
#define PWR_WAKEUP_FLAG_SI91X           (1 << 10)  // WiFi wake-up
#define PWR_WAKEUP_FLAG_NET             (1 << 11)  // Network wake-up
#define PWR_WAKEUP_FLAG_WUFI            (1 << 27)  // WUFI wake-up
#define PWR_WAKEUP_FLAG_VALID           (1 << 31)  // Wake-up flag valid
```

## Appendix: Power Switch Bit Definitions

```c
#define PWR_3V3_SWITCH_BIT              (1 << 0)   // 3.3V power switch
#define PWR_WIFI_SWITCH_BIT             (1 << 1)   // WiFi power switch
#define PWR_AON_SWITCH_BIT              (1 << 2)   // Always-On power switch
#define PWR_N6_SWITCH_BIT               (1 << 3)   // N6 chip power switch
#define PWR_EXT_SWITCH_BIT              (1 << 4)   // External power switch
#define PWR_ALL_SWITCH_BIT             (PWR_3V3_SWITCH_BIT | PWR_WIFI_SWITCH_BIT | \
                                         PWR_AON_SWITCH_BIT | PWR_N6_SWITCH_BIT | \
                                         PWR_EXT_SWITCH_BIT)  // All power switches
#define PWR_DEFAULT_SWITCH_BITS        (PWR_3V3_SWITCH_BIT | PWR_AON_SWITCH_BIT | \
                                         PWR_N6_SWITCH_BIT)  // Default power switches
```

---

**End of Document**
