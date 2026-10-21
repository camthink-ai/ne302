/**
 * @file debug.h
 * @brief AI Camera Debug System Header File
 * @details Debug system header file providing command line interface, log output, YModem transfer and other functions
 */

#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>
#include <stdbool.h>
#include "aicam_types.h"
#include "u0_module.h"
#include "generic_log.h"
#include "json_config_mgr.h"
#include "main.h"
#include "cmsis_os2.h"
#include "generic_cmdline.h"
#include "dev_manager.h"
#include "generic_utils.h"
#include "generic_ymodem.h"
#include "mem.h"

#ifdef __cplusplus
extern "C" {
#endif



/* ==================== Debug System Configuration ==================== */

#define DEBUG_PROMPT_STR            "AICAM>"
#define DEBUG_QUEUE_SIZE            1024
#define DEBUG_DMA_RX_BUF_SIZE       2048

// Default configuration parameters (can be modified through JSON configuration manager)
#define DEBUG_DEFAULT_LOG_FILE_NAME     "aicam.log"
#define DEBUG_DEFAULT_LOG_FILE_SIZE     (500 * 1024)
#define DEBUG_DEFAULT_LOG_FILE_COUNT    3

/* ==================== Debug Module Names ==================== */

#define DEBUG_MODULE_DRIVER         "DRIVER"
#define DEBUG_MODULE_HAL            "HAL"
#define DEBUG_MODULE_CORE           "CORE"
#define DEBUG_MODULE_SERVICE        "SERVICE"
#define DEBUG_MODULE_TASK           "TASK"
#define DEBUG_MODULE_APP            "APP"

/* ==================== Log Level Mapping ==================== */

// Map generic_log.h LogLevel to our log levels
typedef enum {
    LOG_LEVEL_DEBUG = LOG_DEBUG,
    LOG_LEVEL_INFO = LOG_INFO,
    LOG_LEVEL_WARN = LOG_WARNING,
    LOG_LEVEL_ERROR = LOG_ERROR,
    LOG_LEVEL_FATAL = LOG_FATAL,
    LOG_LEVEL_SIMPLE = LOG_SIMPLE
} log_level_e;

/* ==================== Debug System Types ==================== */

/**
 * @brief Debug system working modes
 */
typedef enum {
    DEBUG_MODE_COMMAND = 0,             // Command line mode
    DEBUG_MODE_YMODEM = 1,              // YModem transfer mode
    DEBUG_MODE_DISABLED = 2             // Debug disabled
} debug_mode_e;

/**
 * @brief Debug command registration structure
 */
typedef struct {
    const char *name;
    const char *help;
    cmd_handler handler;
} debug_cmd_reg_t;

/**
 * @brief Debug system statistics
 */
typedef struct {
    unsigned int total_commands;        // Total commands executed
    unsigned int failed_commands;       // Failed commands count
    unsigned int ymodem_transfers;      // YModem transfers count
    unsigned int uart_errors;           // UART errors count
    unsigned long long uptime_seconds;  // Debug system uptime
} debug_stats_t;

/**
 * @brief Debug system context structure
 */
typedef struct {
    aicam_bool_t initialized;           // Initialization status
    device_t *device;                   // Device handle
    debug_mode_e current_mode;          // Current working mode
    
    // Command line interface
    cmdline_t cmdline;                  // Command line processor
    cmd_queue_t cmd_queue;              // Command queue
    char *queue_buffer;                 // Queue buffer
    
    // Logging system (using generic_log)
    log_manager_t log_manager;          // Log manager from generic_log
    log_file_ops_t log_file_ops;        // File operations for logging
    
    // RTOS objects
    osMutexId_t mutex;                  // Main mutex
    osMutexId_t log_mutex;              // Log mutex
    osSemaphoreId_t semaphore;          // Synchronization semaphore
    osThreadId_t debug_task;            // Debug task handle
    osThreadId_t ymodem_task;           // YModem task handle
    
    // UART buffers
    uint8_t uart_rx_byte;               // Single byte buffer for IT mode
    uint8_t *uart_dma_buffer;           // DMA buffer for YModem mode
    GenericQueue dma_queue;             // DMA data queue
    
    // Statistics
    debug_stats_t stats;                // Debug system statistics
    
    // Configuration (from JSON config manager)
    struct {
        log_level_e console_level;      // Console log level
        log_level_e file_level;         // File log level
        unsigned int log_file_size;     // Log file size limit
        uint8_t log_rotation_count;     // Log rotation count
        aicam_bool_t uart_echo_enable;  // UART echo enable
        aicam_bool_t timestamp_enable;  // Timestamp in logs
        aicam_bool_t color_enable;      // Color output enable
    } config;
    
} debug_context_t;

/* ==================== ANSI Color Codes ==================== */

#define ANSI_COLOR_RESET     "\033[0m"
#define ANSI_COLOR_BLACK     "\033[30m"
#define ANSI_COLOR_RED       "\033[31m"
#define ANSI_COLOR_GREEN     "\033[32m"
#define ANSI_COLOR_YELLOW    "\033[33m"
#define ANSI_COLOR_BLUE      "\033[34m"
#define ANSI_COLOR_MAGENTA   "\033[35m"
#define ANSI_COLOR_CYAN      "\033[36m"
#define ANSI_COLOR_WHITE     "\033[37m"

// Bright colors
#define ANSI_COLOR_BRIGHT_BLACK   "\033[90m"
#define ANSI_COLOR_BRIGHT_RED     "\033[91m"
#define ANSI_COLOR_BRIGHT_GREEN   "\033[92m"
#define ANSI_COLOR_BRIGHT_YELLOW  "\033[93m"
#define ANSI_COLOR_BRIGHT_BLUE    "\033[94m"
#define ANSI_COLOR_BRIGHT_MAGENTA "\033[95m"
#define ANSI_COLOR_BRIGHT_CYAN    "\033[96m"
#define ANSI_COLOR_BRIGHT_WHITE   "\033[97m"

// Background colors
#define ANSI_BG_BLACK     "\033[40m"
#define ANSI_BG_RED       "\033[41m"
#define ANSI_BG_GREEN     "\033[42m"
#define ANSI_BG_YELLOW    "\033[43m"
#define ANSI_BG_BLUE      "\033[44m"
#define ANSI_BG_MAGENTA   "\033[45m"
#define ANSI_BG_CYAN      "\033[46m"
#define ANSI_BG_WHITE     "\033[47m"

// Text styles
#define ANSI_STYLE_BOLD      "\033[1m"
#define ANSI_STYLE_DIM       "\033[2m"
#define ANSI_STYLE_ITALIC    "\033[3m"
#define ANSI_STYLE_UNDERLINE "\033[4m"
#define ANSI_STYLE_BLINK     "\033[5m"
#define ANSI_STYLE_REVERSE   "\033[7m"

/* ==================== Log Level Colors ==================== */

#define LOG_COLOR_ERROR      ANSI_COLOR_BRIGHT_RED
#define LOG_COLOR_WARN       ANSI_COLOR_BRIGHT_YELLOW
#define LOG_COLOR_INFO       ANSI_COLOR_BRIGHT_CYAN
#define LOG_COLOR_DEBUG      ANSI_COLOR_BRIGHT_GREEN
#define LOG_COLOR_FATAL      ANSI_BG_RED ANSI_COLOR_BRIGHT_WHITE ANSI_STYLE_BOLD

/* ==================== Module Colors ==================== */

#define MODULE_COLOR_DRIVER   ANSI_COLOR_BLUE
#define MODULE_COLOR_HAL      ANSI_COLOR_MAGENTA
#define MODULE_COLOR_CORE     ANSI_COLOR_GREEN
#define MODULE_COLOR_SERVICE  ANSI_COLOR_CYAN
#define MODULE_COLOR_TASK     ANSI_COLOR_YELLOW
#define MODULE_COLOR_APP      ANSI_COLOR_WHITE

/* ==================== Enhanced Log Macros with Color Support ==================== */

// Color-enabled logging function
#define LOG_OUTPUT_COLOR(level, module, color, fmt, ...) \
    log_message(level, module, color fmt ANSI_COLOR_RESET, ##__VA_ARGS__)

// Generic logging function that uses generic_log
#define LOG_OUTPUT(level, module, fmt, ...) \
    log_message(level, module, fmt, ##__VA_ARGS__)

// Driver layer logging with colors
#define LOG_DRV_ERROR(fmt, ...)     LOG_OUTPUT_COLOR(LOG_LEVEL_ERROR, DEBUG_MODULE_DRIVER, LOG_COLOR_ERROR MODULE_COLOR_DRIVER "[DRIVER] ", fmt, ##__VA_ARGS__)
#define LOG_DRV_WARN(fmt, ...)      LOG_OUTPUT_COLOR(LOG_LEVEL_WARN, DEBUG_MODULE_DRIVER, LOG_COLOR_WARN MODULE_COLOR_DRIVER "[DRIVER] ", fmt, ##__VA_ARGS__)
#define LOG_DRV_INFO(fmt, ...)      LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, DEBUG_MODULE_DRIVER, LOG_COLOR_INFO MODULE_COLOR_DRIVER "[DRIVER] ", fmt, ##__VA_ARGS__)
#define LOG_DRV_DEBUG(fmt, ...)     LOG_OUTPUT_COLOR(LOG_LEVEL_DEBUG, DEBUG_MODULE_DRIVER, LOG_COLOR_DEBUG MODULE_COLOR_DRIVER "[DRIVER] ", fmt, ##__VA_ARGS__)

// HAL layer logging with colors
#define LOG_HAL_ERROR(fmt, ...)     LOG_OUTPUT_COLOR(LOG_LEVEL_ERROR, DEBUG_MODULE_HAL, LOG_COLOR_ERROR MODULE_COLOR_HAL "[HAL] ", fmt, ##__VA_ARGS__)
#define LOG_HAL_WARN(fmt, ...)      LOG_OUTPUT_COLOR(LOG_LEVEL_WARN, DEBUG_MODULE_HAL, LOG_COLOR_WARN MODULE_COLOR_HAL "[HAL] ", fmt, ##__VA_ARGS__)
#define LOG_HAL_INFO(fmt, ...)      LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, DEBUG_MODULE_HAL, LOG_COLOR_INFO MODULE_COLOR_HAL "[HAL] ", fmt, ##__VA_ARGS__)
#define LOG_HAL_DEBUG(fmt, ...)     LOG_OUTPUT_COLOR(LOG_LEVEL_DEBUG, DEBUG_MODULE_HAL, LOG_COLOR_DEBUG MODULE_COLOR_HAL "[HAL] ", fmt, ##__VA_ARGS__)

// Core layer logging with colors
#define LOG_CORE_ERROR(fmt, ...)    LOG_OUTPUT_COLOR(LOG_LEVEL_ERROR, DEBUG_MODULE_CORE, LOG_COLOR_ERROR MODULE_COLOR_CORE "[CORE] ", fmt, ##__VA_ARGS__)
#define LOG_CORE_WARN(fmt, ...)     LOG_OUTPUT_COLOR(LOG_LEVEL_WARN, DEBUG_MODULE_CORE, LOG_COLOR_WARN MODULE_COLOR_CORE "[CORE] ", fmt, ##__VA_ARGS__)
#define LOG_CORE_INFO(fmt, ...)     LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, DEBUG_MODULE_CORE, LOG_COLOR_INFO MODULE_COLOR_CORE "[CORE] ", fmt, ##__VA_ARGS__)
#define LOG_CORE_DEBUG(fmt, ...)    LOG_OUTPUT_COLOR(LOG_LEVEL_DEBUG, DEBUG_MODULE_CORE, LOG_COLOR_DEBUG MODULE_COLOR_CORE "[CORE] ", fmt, ##__VA_ARGS__)

// Service layer logging with colors
#define LOG_SVC_ERROR(fmt, ...)     LOG_OUTPUT_COLOR(LOG_LEVEL_ERROR, DEBUG_MODULE_SERVICE, LOG_COLOR_ERROR MODULE_COLOR_SERVICE "[SERVICE] ", fmt, ##__VA_ARGS__)
#define LOG_SVC_WARN(fmt, ...)      LOG_OUTPUT_COLOR(LOG_LEVEL_WARN, DEBUG_MODULE_SERVICE, LOG_COLOR_WARN MODULE_COLOR_SERVICE "[SERVICE] ", fmt, ##__VA_ARGS__)
#define LOG_SVC_INFO(fmt, ...)      LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, DEBUG_MODULE_SERVICE, LOG_COLOR_INFO MODULE_COLOR_SERVICE "[SERVICE] ", fmt, ##__VA_ARGS__)
#define LOG_SVC_DEBUG(fmt, ...)     LOG_OUTPUT_COLOR(LOG_LEVEL_DEBUG, DEBUG_MODULE_SERVICE, LOG_COLOR_DEBUG MODULE_COLOR_SERVICE "[SERVICE] ", fmt, ##__VA_ARGS__)

// Task layer logging with colors
#define LOG_TASK_ERROR(fmt, ...)    LOG_OUTPUT_COLOR(LOG_LEVEL_ERROR, DEBUG_MODULE_TASK, LOG_COLOR_ERROR MODULE_COLOR_TASK "[TASK] ", fmt, ##__VA_ARGS__)
#define LOG_TASK_WARN(fmt, ...)     LOG_OUTPUT_COLOR(LOG_LEVEL_WARN, DEBUG_MODULE_TASK, LOG_COLOR_WARN MODULE_COLOR_TASK "[TASK] ", fmt, ##__VA_ARGS__)
#define LOG_TASK_INFO(fmt, ...)     LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, DEBUG_MODULE_TASK, LOG_COLOR_INFO MODULE_COLOR_TASK "[TASK] ", fmt, ##__VA_ARGS__)
#define LOG_TASK_DEBUG(fmt, ...)    LOG_OUTPUT_COLOR(LOG_LEVEL_DEBUG, DEBUG_MODULE_TASK, LOG_COLOR_DEBUG MODULE_COLOR_TASK "[TASK] ", fmt, ##__VA_ARGS__)

// Application layer logging with colors
#define LOG_APP_ERROR(fmt, ...)     LOG_OUTPUT_COLOR(LOG_LEVEL_ERROR, DEBUG_MODULE_APP, LOG_COLOR_ERROR MODULE_COLOR_APP "[APP] ", fmt, ##__VA_ARGS__)
#define LOG_APP_WARN(fmt, ...)      LOG_OUTPUT_COLOR(LOG_LEVEL_WARN, DEBUG_MODULE_APP, LOG_COLOR_WARN MODULE_COLOR_APP "[APP] ", fmt, ##__VA_ARGS__)
#define LOG_APP_INFO(fmt, ...)      LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, DEBUG_MODULE_APP, LOG_COLOR_INFO MODULE_COLOR_APP "[APP] ", fmt, ##__VA_ARGS__)
#define LOG_APP_DEBUG(fmt, ...)     LOG_OUTPUT_COLOR(LOG_LEVEL_DEBUG, DEBUG_MODULE_APP, LOG_COLOR_DEBUG MODULE_COLOR_APP "[APP] ", fmt, ##__VA_ARGS__)

// Backward compatibility macros
#define LOG_DRV_FATAL(fmt, ...)     LOG_DRV_ERROR(fmt, ##__VA_ARGS__)
#define LOG_LIB_ERROR(fmt, ...)     LOG_CORE_ERROR(fmt, ##__VA_ARGS__)
#define LOG_LIB_WARN(fmt, ...)      LOG_CORE_WARN(fmt, ##__VA_ARGS__)
#define LOG_LIB_INFO(fmt, ...)      LOG_CORE_INFO(fmt, ##__VA_ARGS__)
#define LOG_LIB_DEBUG(fmt, ...)     LOG_CORE_DEBUG(fmt, ##__VA_ARGS__)
#define LOG_LIB_FATAL(fmt, ...)     LOG_CORE_ERROR(fmt, ##__VA_ARGS__)
#define LOG_FW_ERROR(fmt, ...)      LOG_CORE_ERROR(fmt, ##__VA_ARGS__)
#define LOG_FW_WARN(fmt, ...)       LOG_CORE_WARN(fmt, ##__VA_ARGS__)
#define LOG_FW_INFO(fmt, ...)       LOG_CORE_INFO(fmt, ##__VA_ARGS__)
#define LOG_FW_DEBUG(fmt, ...)      LOG_CORE_DEBUG(fmt, ##__VA_ARGS__)
#define LOG_FW_FATAL(fmt, ...)      LOG_CORE_ERROR(fmt, ##__VA_ARGS__)
#define LOG_APP_FATAL(fmt, ...)     LOG_APP_ERROR(fmt, ##__VA_ARGS__)

// Simple logging without module prefix
#define LOG_SIMPLE(fmt, ...)        LOG_OUTPUT(LOG_LEVEL_SIMPLE, "SIMPLE", fmt, ##__VA_ARGS__)

// Generic logging macros with colors
#define LOG_ERROR(fmt, ...)         LOG_OUTPUT_COLOR(LOG_LEVEL_ERROR, "SYSTEM", LOG_COLOR_ERROR "[SYSTEM] ", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)          LOG_OUTPUT_COLOR(LOG_LEVEL_WARN, "SYSTEM", LOG_COLOR_WARN "[SYSTEM] ", fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)          LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "SYSTEM", LOG_COLOR_INFO "[SYSTEM] ", fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)         LOG_OUTPUT_COLOR(LOG_LEVEL_DEBUG, "SYSTEM", LOG_COLOR_DEBUG "[SYSTEM] ", fmt, ##__VA_ARGS__)

/* ==================== Convenient Color Logging Macros ==================== */

// Direct color logging without module prefix
#define LOG_RED(fmt, ...)           LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "COLOR", ANSI_COLOR_RED, fmt, ##__VA_ARGS__)
#define LOG_GREEN(fmt, ...)         LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "COLOR", ANSI_COLOR_GREEN, fmt, ##__VA_ARGS__)
#define LOG_YELLOW(fmt, ...)        LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "COLOR", ANSI_COLOR_YELLOW, fmt, ##__VA_ARGS__)
#define LOG_BLUE(fmt, ...)          LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "COLOR", ANSI_COLOR_BLUE, fmt, ##__VA_ARGS__)
#define LOG_MAGENTA(fmt, ...)       LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "COLOR", ANSI_COLOR_MAGENTA, fmt, ##__VA_ARGS__)
#define LOG_CYAN(fmt, ...)          LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "COLOR", ANSI_COLOR_CYAN, fmt, ##__VA_ARGS__)
#define LOG_WHITE(fmt, ...)         LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "COLOR", ANSI_COLOR_WHITE, fmt, ##__VA_ARGS__)

// Bright color variants
#define LOG_BRIGHT_RED(fmt, ...)    LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "COLOR", ANSI_COLOR_BRIGHT_RED, fmt, ##__VA_ARGS__)
#define LOG_BRIGHT_GREEN(fmt, ...)  LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "COLOR", ANSI_COLOR_BRIGHT_GREEN, fmt, ##__VA_ARGS__)
#define LOG_BRIGHT_YELLOW(fmt, ...) LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "COLOR", ANSI_COLOR_BRIGHT_YELLOW, fmt, ##__VA_ARGS__)
#define LOG_BRIGHT_BLUE(fmt, ...)   LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "COLOR", ANSI_COLOR_BRIGHT_BLUE, fmt, ##__VA_ARGS__)
#define LOG_BRIGHT_CYAN(fmt, ...)   LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "COLOR", ANSI_COLOR_BRIGHT_CYAN, fmt, ##__VA_ARGS__)

// Special formatting
#define LOG_BOLD(fmt, ...)          LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "STYLE", ANSI_STYLE_BOLD, fmt, ##__VA_ARGS__)
#define LOG_UNDERLINE(fmt, ...)     LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "STYLE", ANSI_STYLE_UNDERLINE, fmt, ##__VA_ARGS__)
#define LOG_HIGHLIGHT(fmt, ...)     LOG_OUTPUT_COLOR(LOG_LEVEL_INFO, "HIGHLIGHT", ANSI_BG_YELLOW ANSI_COLOR_BLACK, fmt, ##__VA_ARGS__)


/* ==================== Public API Functions ==================== */

/**
 * @brief Initialize debug system
 * @details Initialize UART, command line, logging, and YModem subsystems
 * @return aicam_result_t Operation result
 */
aicam_result_t debug_system_init(void);

/**
 * @brief Deinitialize debug system
 * @return aicam_result_t Operation result
 */
aicam_result_t debug_system_deinit(void);

/**
 * @brief Register debug system as device
 * @details Register with device manager for unified device access
 */
void debug_register(void);

/**
 * @brief Register command table with debug system
 * @param cmd_table Array of command registrations
 * @param count Number of commands in table
 * @return aicam_result_t Operation result
 */
aicam_result_t debug_register_commands(const debug_cmd_reg_t *cmd_table, size_t count);

/**
 * @brief Set debug system working mode
 * @param mode New working mode
 * @return aicam_result_t Operation result
 */
aicam_result_t debug_set_mode(debug_mode_e mode);

/**
 * @brief Get current debug system mode
 * @return debug_mode_e Current mode
 */
debug_mode_e debug_get_mode(void);

/**
 * @brief Update debug configuration from JSON config manager
 * @return aicam_result_t Operation result
 */
aicam_result_t debug_update_config(void);

/**
 * @brief Get debug system statistics
 * @param stats Output statistics structure
 * @return aicam_result_t Operation result
 */
aicam_result_t debug_get_stats(debug_stats_t *stats);

/**
 * @brief Reset debug system statistics
 * @return aicam_result_t Operation result
 */
aicam_result_t debug_reset_stats(void);

/**
 * @brief UART interrupt handler
 * @param huart UART handle pointer
 */
void debug_IRQHandler(UART_HandleTypeDef *huart);

/**
 * @brief Process single character input (for command line)
 * @param c Input character
 */
void debug_process_char(char c);

/**
 * @brief Register custom log output function
 * @param func Custom output function
 * @return aicam_result_t Operation result
 */
aicam_result_t debug_register_log_output(log_custom_output_func_t func);

/**
 * @brief Register custom log output function (alias for debug_register_log_output)
 * @param func Custom output function
 */
void debug_output_register(log_custom_output_func_t func);

/**
 * @brief Process command line input character
 * @param c Input character
 */
void debug_cmdline_input(char c);

/**
 * @brief Enable/disable console log output
 * @param enable Enable flag
 * @return aicam_result_t Operation result
 */
aicam_result_t debug_set_console_output(aicam_bool_t enable);

/**
 * @brief Force flush all log outputs
 * @return aicam_result_t Operation result
 */
aicam_result_t debug_flush_logs(void);

/**
 * @brief Get log level string representation
 * @param level Log level
 * @return const char* Log level string
 */
const char* log_level_to_string(log_level_e level);

/**
 * @brief Enable/disable color output in logs
 * @param enable Enable flag
 * @return aicam_result_t Operation result
 */
aicam_result_t debug_set_color_output(aicam_bool_t enable);

/**
 * @brief Get current color output status
 * @return aicam_bool_t True if color output is enabled
 */
aicam_bool_t debug_get_color_output(void);

/**
 * @brief Register command line commands
 * @param cmd_table Command table
 * @param n Number of commands
 */
void debug_cmdline_register(debug_cmd_reg_t *cmd_table, int n);

/* ==================== Driver Command Registration System ==================== */

/**
 * @brief Driver command registration callback function type
 * @details Each driver module should implement this function to register its commands
 */
typedef void (*driver_cmd_register_func_t)(void);

/**
 * @brief Register a driver command registration function
 * @param name Driver module name
 * @param register_func Command registration callback function
 * @return int 0 on success, -1 on failure
 */
int driver_cmd_register_callback(const char* name, driver_cmd_register_func_t register_func);

#ifdef __cplusplus
}
#endif

#endif // DEBUG_H