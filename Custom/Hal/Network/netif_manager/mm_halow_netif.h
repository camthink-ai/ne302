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

int mm_halow_set_regdomain(const char *country_code);

/** Country code buffer size (2 letters + null), matches @ref MMWLAN_COUNTRY_CODE_LEN. */
#define MM_HALOW_REGDOMAIN_CC_LEN           (3U)

unsigned mm_halow_regdomain_count(void);
int mm_halow_regdomain_get_code(unsigned index, char *country_code, size_t len);
/** Print supported regdomain codes from mmregdb (no HaLow init required). */
int mm_halow_list_regdomains(void);
int mm_halow_set_tx_power(uint16_t tx_power_dbm);
int mm_halow_set_power_save(uint8_t enable);
int mm_halow_set_scan_config(uint32_t dwell_ms, uint8_t ndp_probe_enabled);
int mm_halow_print_version(void);

/** Wi-Fi Easy Connect (DPP) push-button provisioning; blocks until done or @p timeout_ms. */
int mm_halow_dpp_start(uint32_t timeout_ms, uint8_t auto_up);
/** Stop an in-progress DPP session. */
int mm_halow_dpp_stop(void);
uint8_t mm_halow_dpp_is_active(void);

#endif /* NETIF_WIFI_HALOW_IS_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* __MM_HALOW_NETIF_H__ */
