#ifndef __QUICK_BOOTSTRAP_H__
#define __QUICK_BOOTSTRAP_H__

#ifdef __cplusplus
extern "C" {
#endif

#define QB_LOG_LEVEL_NONE   0
#define QB_LOG_LEVEL_ERROR  1
#define QB_LOG_LEVEL_WARN   2
#define QB_LOG_LEVEL_DEBUG  3
#define QB_LOG_LEVEL        QB_LOG_LEVEL_DEBUG
#define QB_LOG_TAG          "[QB] "

/* Thread-safe line output (mutex + vprintf). See quick_bootstrap.c; shared with QT_TRACE. */
void quick_log_mutex_init(void);
void quick_log_printf(const char *fmt, ...);

/* Optional lightweight traces/profiling (compile-time only).
 * Keep default OFF to avoid extra runtime overhead in production builds.
 */
#ifndef QB_TRACE_ENABLE
#define QB_TRACE_ENABLE     0
#endif

#ifndef QB_TIMELOG_ENABLE
#define QB_TIMELOG_ENABLE   0
#endif

#if QB_LOG_LEVEL >= QB_LOG_LEVEL_ERROR
#define QB_LOGE(fmt, ...)   quick_log_printf(QB_LOG_TAG fmt "\r\n", ##__VA_ARGS__)
#else
#define QB_LOGE(fmt, ...)  
#endif

#if QB_LOG_LEVEL >= QB_LOG_LEVEL_WARN
#define QB_LOGW(fmt, ...)   quick_log_printf(QB_LOG_TAG fmt "\r\n", ##__VA_ARGS__)
#else
#define QB_LOGW(fmt, ...)  
#endif

#if QB_LOG_LEVEL >= QB_LOG_LEVEL_DEBUG
#define QB_LOGD(fmt, ...)   quick_log_printf(QB_LOG_TAG fmt "\r\n", ##__VA_ARGS__)
#else
#define QB_LOGD(fmt, ...)  
#endif

/**
 * @brief Wakeup source type
 */
typedef enum {
    QB_WAKEUP_SOURCE_BUTTON = 0,
    QB_WAKEUP_SOURCE_TIMER,
    QB_WAKEUP_SOURCE_PIR,
    QB_WAKEUP_SOURCE_MAX,
} qb_wakeup_source_type_t;

/**
 * @brief Run quick bootstrap
 * @param wakeup_source Wakeup source type
 */
void quick_bootstrap_run(qb_wakeup_source_type_t wakeup_source);

/**
 * @brief Event-flag bit used with the bootstrap internal `osEventFlags` (not the handle itself).
 *        Set together with completion when MQTT publish fails (init drain, disconnect, broker error,
 *        or enqueue failure). Success path clears implicitly on the next `quick_bootstrap_run` (new event group).
 */
#ifndef QB_FLAG_MQTT_FAILED
#define QB_FLAG_MQTT_FAILED   (1UL << 12)
#endif

#ifdef __cplusplus
}
#endif

#endif /* __QUICK_BOOTSTRAP_H__ */
