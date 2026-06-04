#ifndef __MM_HALOW_NETIF_H__
#define __MM_HALOW_NETIF_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "netif_manager.h"

#if NETIF_WIFI_HALOW_IS_ENABLE

int mm_halow_netif_ctrl(const char *if_name, netif_cmd_t cmd, void *param);
struct netif *mm_halow_netif_ptr(void);
netif_state_t mm_halow_netif_state(void);

int mm_halow_start_scan(wireless_scan_callback_t callback);
wireless_scan_result_t *mm_halow_get_storage_scan_result(void);
int mm_halow_update_storage_scan_result(uint32_t timeout_ms);

/**
 * Select a scanned AP for quick join (sets @c wireless_cfg.bssid).
 * Requires a prior scan that captured S1G Operation IE for @p bssid (within 60s).
 * @return 0 on success, -1 if @p bssid is not in the quick-join cache.
 */
int mm_halow_set_preconnect_target(const uint8_t bssid[6]);

int mm_halow_set_regdomain(const char *country_code);

/** Country code buffer size (2 letters + null), matches @ref MMWLAN_COUNTRY_CODE_LEN. */
#define MM_HALOW_REGDOMAIN_CC_LEN           (3U)

unsigned mm_halow_regdomain_count(void);
/** 1 if @p country_code is in mmregdb and the embedded firmware BCF. */
int mm_halow_regdomain_is_supported(const char *country_code);
int mm_halow_regdomain_get_code(unsigned index, char *country_code, size_t len);
/** Print regdomains present in firmware BCF (no HaLow init required). */
int mm_halow_list_regdomains(void);
int mm_halow_set_tx_power(uint16_t tx_power_dbm);
int mm_halow_set_power_save(uint8_t enable);
int mm_halow_set_scan_config(uint32_t dwell_ms, uint8_t ndp_probe_enabled);
int mm_halow_print_version(void);
/**
 * Print current BCF metadata.
 *
 * @param country_code Optional 2-letter country code. If provided, selects the matching BCF first
 *                     and then prints metadata. If NULL, uses the current HaLow config country code.
 */
int mm_halow_print_bcf_info(const char *country_code);

/**
 * Apply @c halow_netif_cfg IP settings at runtime (DHCP or static).
 * @return 0 on success, -1 if stack not ready or mmipal rejected the change.
 */
int mm_halow_apply_ip_config(void);

/** HaLow DPP (Wi-Fi Easy Connect) completion events. */
typedef enum {
    MM_HALOW_DPP_EVT_SUCCESS = 0,
    MM_HALOW_DPP_EVT_FAILED,
    MM_HALOW_DPP_EVT_SESSION_OVERLAP,
    MM_HALOW_DPP_EVT_TIMEOUT,
    MM_HALOW_DPP_EVT_STOPPED,
} mm_halow_dpp_evt_t;

/** Payload passed to @ref mm_halow_dpp_callback_t (valid only for the duration of the call). */
typedef struct {
    mm_halow_dpp_evt_t event;
    /** Set on @ref MM_HALOW_DPP_EVT_SUCCESS; points at saved netif credentials. */
    const char *ssid;
    /** @ref WIRELESS_OPEN or @ref WIRELESS_SAE on success. */
    uint8_t security;
} mm_halow_dpp_evt_info_t;

typedef void (*mm_halow_dpp_callback_t)(const mm_halow_dpp_evt_info_t *info, void *user_arg);

/**
 * Start DPP push-button provisioning (non-blocking).
 * @p timeout_ms 0 uses @ref HALOW_DPP_DEFAULT_TIMEOUT_MS.
 * @p cb          Optional; invoked on success, failure, overlap, timeout, or stop.
 * @return 0 if started, -1 on precondition or start failure (no callback).
 */
int mm_halow_dpp_start(uint32_t timeout_ms, mm_halow_dpp_callback_t cb, void *user_arg);
/** Stop an in-progress DPP session; invokes @p cb with @ref MM_HALOW_DPP_EVT_STOPPED if active. */
int mm_halow_dpp_stop(void);
uint8_t mm_halow_dpp_is_active(void);

#endif /* NETIF_WIFI_HALOW_IS_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* __MM_HALOW_NETIF_H__ */
