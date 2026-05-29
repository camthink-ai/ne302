#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "mm_halow_netif.h"
#include "chip_id_mac.h"
#include "halow_platform_mac.h"

#if NETIF_WIFI_HALOW_IS_ENABLE

#include "stm32n6xx_hal.h"
#include "main.h"
#include "mem.h"
#include "Log/debug.h"
#include "pwr.h"
#include "cmsis_os2.h"
#include "mmhal.h"
#include "mmhal_wlan.h"
#include "mmwlan.h"
#include "mmipal.h"
#include "mmregdb.h"
#include "mmosal.h"
#include "mmutils.h"
#include "lwip/netif.h"

#if !defined(MMHAL_WLAN_USE_SOFT_SPI)
#include "spi.h"
#endif

struct netif *mmipal_get_lwip_netif(void);

/* Morse: set STA MAC after boot, before sta_enable (libmorse.a). */
enum mmwlan_status mmwlan_sta_set_mac_addr(const uint8_t *mac_addr);

#define HALOW_SCAN_RESULT_MAX           (64)
#define HALOW_LINK_WAIT_MS              (30000U)
#define HALOW_DPP_DEFAULT_TIMEOUT_MS    (120000U)

static netif_config_t halow_netif_cfg = {
    .wireless_cfg = {
        .ssid = NETIF_WIFI_HALOW_DEFAULT_SSID,
        .pw = NETIF_WIFI_HALOW_DEFAULT_PW,
        .security = WIRELESS_OPEN,
        .encryption = WIRELESS_DEFAULT_ENCRYPTION,
        .channel = 0,
    },
#if NETIF_WIFI_HALOW_IS_ENABLE
    .halow_cfg = {
        .country_code = NETIF_WIFI_HALOW_DEFAULT_COUNTRY,
        .tx_power_dbm = NETIF_WIFI_HALOW_DEFAULT_TX_PWR,
        .ps_mode = 0,
        .pmf_mode = 0,
        .scan_dwell_ms = NETIF_WIFI_HALOW_DEFAULT_SCAN_DWELL,
        .ndp_probe_enabled = 0,
        .bgscan_short_interval_s = 0,
        .bgscan_signal_threshold_dbm = 0,
        .bgscan_long_interval_s = 0,
    },
#endif
    .ip_mode = NETIF_WIFI_HALOW_DEFAULT_IP_MODE,
    .ip_addr = NETIF_WIFI_HALOW_DEFAULT_IP,
    .netmask = NETIF_WIFI_HALOW_DEFAULT_MASK,
    .gw = NETIF_WIFI_HALOW_DEFAULT_GW,
};

static netif_state_t halow_state = NETIF_STATE_DEINIT;
static uint8_t halow_stack_ready = 0;
/** Set after mmwlan_boot(); cleared by shutdown so regdomain can be changed while DOWN. */
static uint8_t halow_mmwlan_booted = 0;
/** Set after mmwlan_init(); cleared by mmwlan_deinit(). */
static uint8_t halow_mmwlan_inited = 0;
static osMutexId_t halow_mutex = NULL;
static struct mmosal_semb *halow_link_sem = NULL;

static wireless_scan_result_t halow_storage_scan_result = {0};
static osSemaphoreId_t halow_scan_sem = NULL;
static wireless_scan_callback_t halow_scan_user_cb = NULL;
static uint8_t halow_scan_in_progress = 0;
static uint8_t halow_scan_need_restore = 0;
static wireless_scan_info_t *halow_async_scan_buf = NULL;
static wireless_scan_result_t halow_async_scan_result = {0};
static PowerHandle halow_pwr_handle = 0;
static uint8_t halow_pwr_acquired = 0;

static void halow_resolve_sta_mac(uint8_t mac[6])
{
    if (NETIF_MAC_IS_UNICAST(halow_netif_cfg.diy_mac)) {
        memcpy(mac, halow_netif_cfg.diy_mac, 6U);
    } else {
        netif_chip_id_get_mac(mac, NETIF_CHIP_MAC_HALOW);
    }
}

static void halow_apply_sta_mac_policy(void)
{
    uint8_t mac[6];

    halow_resolve_sta_mac(mac);
    mmhal_wlan_use_platform_mac(mac);
}

static int halow_apply_halow_hw_config_locked(void);
static int halow_apply_regdomain_locked(const char *country_code);
static int halow_mmwlan_boot_locked(void);

static void halow_try_set_sta_mac_runtime(const uint8_t mac[6])
{
    uint8_t cur[6];
    enum mmwlan_status status;

    status = mmwlan_sta_set_mac_addr(mac);
    if (status == MMWLAN_SUCCESS) {
        return;
    }
    if (mmwlan_get_vif_mac_addr(MMWLAN_VIF_STA, cur) == MMWLAN_SUCCESS &&
        memcmp(cur, mac, 6) != 0) {
        LOG_DRV_WARN("HaLow STA MAC differs from cfg; down+up or deinit to apply");
    }
}

static volatile uint8_t halow_dpp_active = 0;
static struct mmosal_semb *halow_dpp_done_sem = NULL;
static enum mmwlan_dpp_pb_result halow_dpp_last_result = MMWLAN_DPP_PB_RESULT_ERROR;

static int halow_power_acquire(void)
{
    if (halow_pwr_acquired) {
        return 0;
    }

    if (halow_pwr_handle == 0) {
        halow_pwr_handle = pwr_manager_get_handle(PWR_HALOW_NAME);
        if (halow_pwr_handle == 0) {
            LOG_DRV_ERROR("HaLow power handle not found (%s)", PWR_HALOW_NAME);
            return -1;
        }
    }

    if (pwr_manager_acquire(halow_pwr_handle) != 0) {
        LOG_DRV_ERROR("HaLow power acquire failed");
        return -1;
    }

    halow_pwr_acquired = 1;
    osDelay(10);
    return 0;
}

static void halow_power_release(void)
{
    if (!halow_pwr_acquired) {
        return;
    }

    pwr_manager_release(halow_pwr_handle);
    halow_pwr_acquired = 0;
}

static void mm_halow_gpios_init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitStruct.Pin = MM_HALOW_RESET_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(MM_HALOW_RESET_GPIO_Port, &GPIO_InitStruct);
    HAL_GPIO_WritePin(MM_HALOW_RESET_GPIO_Port, MM_HALOW_RESET_Pin, GPIO_PIN_RESET);

    GPIO_InitStruct.Pin = MM_HALOW_WAKE_Pin;
    HAL_GPIO_Init(MM_HALOW_WAKE_GPIO_Port, &GPIO_InitStruct);
    /*
     * Keep WAKE deasserted initially. The driver will assert it when required and
     * some HW/firmware combinations expect a low->high transition.
     */
    HAL_GPIO_WritePin(MM_HALOW_WAKE_GPIO_Port, MM_HALOW_WAKE_Pin, GPIO_PIN_SET);

    GPIO_InitStruct.Pin = MM_HALOW_SPI_IRQ_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(MM_HALOW_SPI_IRQ_GPIO_Port, &GPIO_InitStruct);
    HAL_GPIO_WritePin(MM_HALOW_SPI_IRQ_GPIO_Port, MM_HALOW_SPI_IRQ_Pin, GPIO_PIN_SET);
    HAL_NVIC_SetPriority(EXTI4_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(EXTI4_IRQn);

    GPIO_InitStruct.Pin = MM_HALOW_BUSY_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(MM_HALOW_BUSY_GPIO_Port, &GPIO_InitStruct);
    HAL_GPIO_WritePin(MM_HALOW_BUSY_GPIO_Port, MM_HALOW_BUSY_Pin, GPIO_PIN_RESET);
    HAL_NVIC_SetPriority(EXTI15_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(EXTI15_IRQn);
}

static void mm_halow_link_status_callback(const struct mmipal_link_status *link_status)
{
    if (link_status == NULL) {
        return;
    }

    if (link_status->link_state == MMIPAL_LINK_UP) {
        halow_state = NETIF_STATE_UP;
        if (halow_link_sem != NULL) {
            mmosal_semb_give(halow_link_sem);
        }
        LOG_DRV_INFO("HaLow link up, IP: %s", link_status->ip_addr);
    } else {
        if (halow_state == NETIF_STATE_UP) {
            halow_state = NETIF_STATE_DOWN;
        }
        LOG_DRV_INFO("HaLow link down");
    }
}

static void mm_halow_sta_status_callback(enum mmwlan_sta_state sta_state)
{
    switch (sta_state) {
    case MMWLAN_STA_DISABLED:
        LOG_DRV_DEBUG("HaLow STA disabled");
        break;
    case MMWLAN_STA_CONNECTING:
        LOG_DRV_DEBUG("HaLow STA connecting");
        break;
    case MMWLAN_STA_CONNECTED:
        LOG_DRV_DEBUG("HaLow STA connected (L2)");
        break;
    default:
        break;
    }
}

static enum mmwlan_security_type halow_map_security(wireless_security_t security)
{
    switch (security) {
    case WIRELESS_OPEN:
        return MMWLAN_OPEN;
    case WIRELESS_OWE:
        return MMWLAN_OWE;
    case WIRELESS_SAE:
    case WIRELESS_WPA3:
        return MMWLAN_SAE;
    default:
        return MMWLAN_OPEN;
    }
}

static int halow_ip_u8_to_str(const uint8_t *ip, char *buf, size_t len)
{
    if (ip == NULL || buf == NULL || len < 16) {
        return -1;
    }
    snprintf(buf, len, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    return 0;
}

/** Stop scan/DPP/STA before stack deinit (not part of init; operational cleanup). */
static void halow_abort_operations_locked(void)
{
    if (halow_scan_in_progress) {
        (void)mmwlan_scan_abort();
        halow_scan_in_progress = 0;
        halow_scan_need_restore = 0;
    }

    (void)mm_halow_dpp_stop();

    if (halow_state == NETIF_STATE_UP) {
        (void)mmwlan_sta_disable();
        halow_state = NETIF_STATE_DOWN;
    }
}

/** Inverse of halow_mmwlan_boot_locked(). */
static void halow_mmwlan_unboot_locked(void)
{
    if (!halow_mmwlan_booted) {
        return;
    }

    if (mmwlan_shutdown() != MMWLAN_SUCCESS) {
        LOG_DRV_ERROR("HaLow mmwlan_shutdown failed");
    }
    halow_mmwlan_booted = 0;
}

/**
 * Morse/mmHAL bring-up — must match halow_stack_deinit_locked() in reverse order.
 * Caller holds halow_mutex; power and link_sem must already be ready.
 */
static int halow_stack_init_locked(void)
{
    mm_halow_gpios_init();

#if !defined(MMHAL_WLAN_USE_SOFT_SPI)
    MX_SPI6_Init();
#endif

    mmhal_init();
    mmhal_wlan_init();
    mmhal_wlan_hard_reset();

    mmwlan_init();
    halow_mmwlan_inited = 1;

    if (halow_apply_regdomain_locked(halow_netif_cfg.halow_cfg.country_code) != 0) {
        return -1;
    }

    (void)halow_apply_halow_hw_config_locked();

    if (halow_mmwlan_boot_locked() != 0) {
        return -1;
    }

    halow_stack_ready = 0;
    return 0;
}

/**
 * Inverse of halow_stack_init_locked() (same order, reversed).
 */
static void halow_stack_deinit_locked(void)
{
    halow_mmwlan_unboot_locked();       /* ↔ halow_mmwlan_boot_locked() */
    /* ↔ halow_apply_halow_hw_config_locked / regdomain: runtime config only */
    if (halow_mmwlan_inited) {
        mmwlan_deinit();                /* ↔ mmwlan_init() */
        halow_mmwlan_inited = 0;
    }
    mmhal_wlan_deinit();                /* ↔ mmhal_wlan_init() */
    /* ↔ mmhal_wlan_hard_reset: not undone */
    /* ↔ mmhal_init: no mmhal_deinit on this platform */
#if !defined(MMHAL_WLAN_USE_SOFT_SPI)
    MX_SPI6_DeInit();                   /* ↔ MX_SPI6_Init() (HAL_SPI_DeInit + MspDeInit) */
#endif
    /* ↔ mm_halow_gpios_init: pins left as-is */

    halow_stack_ready = 0;
    halow_state = NETIF_STATE_DEINIT;
}

/** Inverse of init-time link_sem / power setup (after halow_stack_deinit_locked). */
static void halow_netif_resources_deinit_locked(void)
{
    if (halow_storage_scan_result.scan_info != NULL) {
        hal_mem_free(halow_storage_scan_result.scan_info);
        halow_storage_scan_result.scan_info = NULL;
    }
    halow_storage_scan_result.scan_count = 0;

    if (halow_link_sem != NULL) {
        mmosal_semb_delete(halow_link_sem);
        halow_link_sem = NULL;
    }

    if (halow_dpp_done_sem != NULL) {
        mmosal_semb_delete(halow_dpp_done_sem);
        halow_dpp_done_sem = NULL;
    }
    halow_dpp_active = 0;

    halow_power_release();              /* ↔ halow_power_acquire() in init */
}

/** Stop STA/scan and mmwlan so channel list can be changed (Morse requires inactive UMAC). */
static int halow_mmwlan_teardown_locked(void)
{
    enum mmwlan_status status;

    if (halow_scan_in_progress) {
        (void)mmwlan_scan_abort();
        halow_scan_in_progress = 0;
        halow_scan_need_restore = 0;
    }

    if (halow_state == NETIF_STATE_UP) {
        (void)mmwlan_sta_disable();
        halow_state = NETIF_STATE_DOWN;
    }

    if (!halow_mmwlan_booted) {
        return 0;
    }

    status = mmwlan_shutdown();
    halow_mmwlan_booted = 0;
    if (status != MMWLAN_SUCCESS) {
        LOG_DRV_ERROR("HaLow mmwlan_shutdown failed");
        return -1;
    }
    return 0;
}

static int halow_mmwlan_boot_locked(void)
{
    struct mmwlan_boot_args boot_args = MMWLAN_BOOT_ARGS_INIT;

    if (mmwlan_boot(&boot_args) != MMWLAN_SUCCESS) {
        LOG_DRV_ERROR("HaLow mmwlan_boot failed");
        return -1;
    }

    (void)mmwlan_set_power_save_mode(halow_netif_cfg.halow_cfg.ps_mode ? MMWLAN_PS_ENABLED : MMWLAN_PS_DISABLED);
    (void)halow_apply_halow_hw_config_locked();
    halow_apply_sta_mac_policy();
    halow_mmwlan_booted = 1;
    return 0;
}

static int halow_ensure_mmwlan_booted_locked(void)
{
    if (halow_mmwlan_booted) {
        return 0;
    }
    return halow_mmwlan_boot_locked();
}

static int halow_apply_regdomain_locked(const char *country_code)
{
    const struct mmwlan_regulatory_db *db = get_regulatory_db();
    const struct mmwlan_s1g_channel_list *list;
    uint8_t restart_radio = 0;

    if (country_code == NULL || country_code[0] == '\0') {
        return -1;
    }

    list = mmwlan_lookup_regulatory_domain(db, country_code);
    if (list == NULL) {
        LOG_DRV_ERROR("HaLow regdomain not found: %s", country_code);
        return -1;
    }

    if (halow_mmwlan_booted) {
        restart_radio = 1;
        LOG_DRV_INFO("HaLow restarting radio for regdomain %s", country_code);
        if (halow_mmwlan_teardown_locked() != 0) {
            return -1;
        }
    }

    if (mmwlan_set_channel_list(list) != MMWLAN_SUCCESS) {
        LOG_DRV_ERROR("HaLow set_channel_list failed for %s", country_code);
        if (restart_radio) {
            (void)halow_mmwlan_boot_locked();
        }
        return -1;
    }

    strncpy(halow_netif_cfg.halow_cfg.country_code, country_code, sizeof(halow_netif_cfg.halow_cfg.country_code) - 1);
    halow_netif_cfg.halow_cfg.country_code[sizeof(halow_netif_cfg.halow_cfg.country_code) - 1] = '\0';

    if (restart_radio) {
        if (halow_mmwlan_boot_locked() != 0) {
            return -1;
        }
    }

    return 0;
}

static int halow_apply_halow_hw_config_locked(void)
{
    struct mmwlan_scan_config scan_cfg = MMWLAN_SCAN_CONFIG_INIT;
    enum mmwlan_status status;

    if (halow_netif_cfg.halow_cfg.tx_power_dbm > 0) {
        status = mmwlan_override_max_tx_power(halow_netif_cfg.halow_cfg.tx_power_dbm);
        if (status != MMWLAN_SUCCESS) {
            LOG_DRV_WARN("HaLow tx power override failed: %d", (int)status);
        }
    } else {
        (void)mmwlan_override_max_tx_power(0);
    }

    scan_cfg.dwell_time_ms = halow_netif_cfg.halow_cfg.scan_dwell_ms;
    scan_cfg.ndp_probe_enabled = (halow_netif_cfg.halow_cfg.ndp_probe_enabled != 0);
    scan_cfg.home_channel_dwell_time_ms = MMWLAN_SCAN_DEFAULT_DWELL_ON_HOME_MS;
    (void)mmwlan_set_scan_config(&scan_cfg);

    return 0;
}

static int halow_mmipal_init_from_cfg(void)
{
    struct mmipal_init_args ip_init_args;
    char ip_str[16];
    char mask_str[16];
    char gw_str[16];

    memset(&ip_init_args, 0, sizeof(ip_init_args));

    if (halow_netif_cfg.ip_mode == NETIF_IP_MODE_DHCP) {
        ip_init_args.mode = MMIPAL_DHCP;
    } else {
        ip_init_args.mode = MMIPAL_STATIC;
        halow_ip_u8_to_str(halow_netif_cfg.ip_addr, ip_str, sizeof(ip_str));
        halow_ip_u8_to_str(halow_netif_cfg.netmask, mask_str, sizeof(mask_str));
        halow_ip_u8_to_str(halow_netif_cfg.gw, gw_str, sizeof(gw_str));
        strncpy(ip_init_args.ip_addr, ip_str, sizeof(ip_init_args.ip_addr) - 1);
        strncpy(ip_init_args.netmask, mask_str, sizeof(ip_init_args.netmask) - 1);
        strncpy(ip_init_args.gateway_addr, gw_str, sizeof(ip_init_args.gateway_addr) - 1);
    }

    if (mmipal_init(&ip_init_args) != MMIPAL_SUCCESS) {
        LOG_DRV_ERROR("HaLow mmipal_init failed");
        return -1;
    }

    mmipal_set_link_status_callback(mm_halow_link_status_callback);
    halow_stack_ready = 1;
    return 0;
}

static void halow_fill_sta_args(struct mmwlan_sta_args *sta_args)
{
    const wireless_config_t *wc = &halow_netif_cfg.wireless_cfg;
    const halow_wireless_config_t *hc = &halow_netif_cfg.halow_cfg;

    struct mmwlan_sta_args defaults = MMWLAN_STA_ARGS_INIT;
    memcpy(sta_args, &defaults, sizeof(*sta_args));

    strncpy((char *)sta_args->ssid, wc->ssid, MMWLAN_SSID_MAXLEN - 1);
    sta_args->ssid_len = (uint16_t)strlen(wc->ssid);
    strncpy(sta_args->passphrase, wc->pw, MMWLAN_PASSPHRASE_MAXLEN);
    sta_args->passphrase_len = (uint16_t)strlen(wc->pw);
    sta_args->security_type = halow_map_security(wc->security);
    sta_args->pmf_mode = (hc->pmf_mode != 0) ? MMWLAN_PMF_DISABLED : MMWLAN_PMF_REQUIRED;

    if (NETIF_MAC_IS_UNICAST(wc->bssid)) {
        memcpy(sta_args->bssid, wc->bssid, sizeof(sta_args->bssid));
    }

    sta_args->bgscan_short_interval_s = hc->bgscan_short_interval_s;
    sta_args->bgscan_signal_threshold_dbm = hc->bgscan_signal_threshold_dbm;
    sta_args->bgscan_long_interval_s = hc->bgscan_long_interval_s;
}

static void halow_scan_result_to_info(const struct mmwlan_scan_result *src, wireless_scan_info_t *dst)
{
    size_t ssid_len;

    memset(dst, 0, sizeof(*dst));
    if (src == NULL) {
        return;
    }

    dst->rssi = src->rssi;
    dst->channel_freq_hz = src->channel_freq_hz;
    dst->bw_mhz = src->bw_mhz;
    /* wireless_scan_info_t.security is uint8_t; use MAX as unknown marker */
    dst->security = (uint8_t)WIRELESS_SECURITY_MAX;

    if (src->bssid != NULL) {
        memcpy(dst->bssid, src->bssid, 6);
    }

    if (src->ssid != NULL && src->ssid_len > 0) {
        ssid_len = src->ssid_len;
        if (ssid_len >= NETIF_SSID_VALUE_SIZE) {
            ssid_len = NETIF_SSID_VALUE_SIZE - 1;
        }
        memcpy(dst->ssid, src->ssid, ssid_len);
        dst->ssid[ssid_len] = '\0';
    }

    /* Security detection (best-effort): parse RSN IE and look for OWE/SAE AKMs. */
    if (src->ies != NULL && src->ies_len >= 2) {
        const uint8_t *p = src->ies;
        size_t left = src->ies_len;
        uint8_t saw_rsn = 0;

        while (left >= 2) {
            uint8_t eid = p[0];
            uint8_t elen = p[1];
            p += 2;
            left -= 2;
            if (elen > left) {
                break;
            }

            if (eid == 48 /* RSN */) {
                /* RSN IE: version(2) + group cipher(4) + pairwise_count(2) + pairwise_list + akm_count(2) + akm_list */
                const uint8_t *rsn = p;
                size_t rsn_len = elen;
                size_t off = 0;
                uint16_t pairwise_cnt = 0;
                uint16_t akm_cnt = 0;

                saw_rsn = 1;

                if (rsn_len < 2 + 4 + 2) {
                    goto rsn_done;
                }
                off += 2; /* version */
                off += 4; /* group cipher */
                pairwise_cnt = (uint16_t)(rsn[off] | (rsn[off + 1] << 8));
                off += 2;
                if (off + (size_t)pairwise_cnt * 4 > rsn_len) {
                    goto rsn_done;
                }
                off += (size_t)pairwise_cnt * 4;
                if (off + 2 > rsn_len) {
                    goto rsn_done;
                }
                akm_cnt = (uint16_t)(rsn[off] | (rsn[off + 1] << 8));
                off += 2;
                if (off + (size_t)akm_cnt * 4 > rsn_len) {
                    goto rsn_done;
                }

                for (uint16_t i = 0; i < akm_cnt; i++) {
                    const uint8_t *suite = &rsn[off + (size_t)i * 4];
                    /* 00-0f-ac is the IEEE OUI used in RSN suites */
                    if (suite[0] == 0x00 && suite[1] == 0x0f && suite[2] == 0xac) {
                        uint8_t type = suite[3];
                        if (type == 8 /* SAE */) {
                            dst->security = (uint8_t)WIRELESS_SAE;
                            goto rsn_done;
                        }
                        if (type == 18 /* OWE */) {
                            dst->security = (uint8_t)WIRELESS_OWE;
                            goto rsn_done;
                        }
                    }
                }

rsn_done:
                /* If we saw RSN but no explicit OWE/SAE, mark as protected generic. */
                if (dst->security == (uint8_t)WIRELESS_SECURITY_MAX) {
                    dst->security = (uint8_t)WIRELESS_WPA2;
                }
            }

            p += elen;
            left -= elen;
        }

        if (!saw_rsn && dst->security == (uint8_t)WIRELESS_SECURITY_MAX) {
            /* If privacy bit not set, treat as open. */
            if ((src->capability_info & 0x0010) == 0) {
                dst->security = (uint8_t)WIRELESS_OPEN;
            }
        }
    } else {
        if ((src->capability_info & 0x0010) == 0) {
            dst->security = (uint8_t)WIRELESS_OPEN;
        }
    }
}

static void halow_scan_rx_handler(const struct mmwlan_scan_result *result, void *arg)
{
    wireless_scan_result_t *target = (wireless_scan_result_t *)arg;
    wireless_scan_info_t *info;
    wireless_scan_info_t tmp;

    if (result == NULL || target == NULL) {
        return;
    }

    /* Convert once, then de-duplicate by (BSSID + freq + bw + SSID). */
    halow_scan_result_to_info(result, &tmp);

    if (target->scan_info != NULL) {
        for (uint8_t i = 0; i < target->scan_count; i++) {
            wireless_scan_info_t *cur = &target->scan_info[i];
            if (memcmp(cur->bssid, tmp.bssid, 6) == 0 &&
                cur->channel_freq_hz == tmp.channel_freq_hz &&
                cur->bw_mhz == tmp.bw_mhz &&
                strncmp(cur->ssid, tmp.ssid, NETIF_SSID_VALUE_SIZE) == 0) {
                /* Keep the stronger RSSI and a more specific security if we learned it. */
                if (tmp.rssi > cur->rssi) {
                    cur->rssi = tmp.rssi;
                }
                if (cur->security >= WIRELESS_SECURITY_MAX && tmp.security < WIRELESS_SECURITY_MAX) {
                    cur->security = tmp.security;
                }
                return;
            }
        }
    }

    if (target->scan_count >= HALOW_SCAN_RESULT_MAX) {
        return;
    }

    if (target->scan_info == NULL) {
        return;
    }

    info = &target->scan_info[target->scan_count];
    memcpy(info, &tmp, sizeof(*info));
    target->scan_count++;
}

static void halow_scan_restore_sta_if_needed(void)
{
    struct mmwlan_sta_args sta_args;

    if (!halow_scan_need_restore) {
        return;
    }

    halow_scan_need_restore = 0;
    halow_fill_sta_args(&sta_args);
    (void)mmwlan_set_power_save_mode(halow_netif_cfg.halow_cfg.ps_mode ? MMWLAN_PS_ENABLED : MMWLAN_PS_DISABLED);
    if (mmwlan_sta_enable(&sta_args, mm_halow_sta_status_callback) == MMWLAN_SUCCESS) {
        halow_state = NETIF_STATE_UP;
    }
}

static void halow_scan_complete_handler(enum mmwlan_scan_state scan_state, void *arg)
{
    wireless_scan_result_t *target = (wireless_scan_result_t *)arg;
    wireless_scan_result_t snapshot = {0};
    wireless_scan_callback_t user_cb = halow_scan_user_cb;
    int cb_ret = (scan_state == MMWLAN_SCAN_SUCCESSFUL) ? 0 : -1;

    halow_scan_in_progress = 0;
    halow_scan_user_cb = NULL;

    if (scan_state != MMWLAN_SCAN_SUCCESSFUL) {
        LOG_DRV_WARN("HaLow scan complete state=%d", (int)scan_state);
    }

    if (target != NULL && target->scan_info != NULL && user_cb != NULL) {
        snapshot.scan_count = target->scan_count;
        snapshot.scan_info = target->scan_info;
        user_cb(cb_ret, &snapshot);
    }

    if (target == &halow_async_scan_result && halow_async_scan_buf != NULL) {
        hal_mem_free(halow_async_scan_buf);
        halow_async_scan_buf = NULL;
        halow_async_scan_result.scan_info = NULL;
        halow_async_scan_result.scan_count = 0;
    }

    halow_scan_restore_sta_if_needed();

    if (halow_scan_sem != NULL) {
        osSemaphoreRelease(halow_scan_sem);
    }
}

static int halow_run_scan_locked(wireless_scan_result_t *storage, wireless_scan_callback_t callback, uint32_t wait_ms)
{
    struct mmwlan_scan_req scan_req = MMWLAN_SCAN_REQ_INIT;
    enum mmwlan_status status;
    wireless_scan_result_t *scan_target = storage;
    int ret = 0;

    if (halow_state == NETIF_STATE_DEINIT || halow_scan_in_progress) {
        return -1;
    }

    if (halow_ensure_mmwlan_booted_locked() != 0) {
        return -1;
    }

    if (halow_scan_sem == NULL) {
        halow_scan_sem = osSemaphoreNew(1, 0, NULL);
        if (halow_scan_sem == NULL) {
            return -1;
        }
    }

    if (scan_target == NULL && callback != NULL) {
        halow_async_scan_buf = hal_mem_alloc_large(HALOW_SCAN_RESULT_MAX * sizeof(wireless_scan_info_t));
        if (halow_async_scan_buf == NULL) {
            return -1;
        }
        halow_async_scan_result.scan_info = halow_async_scan_buf;
        halow_async_scan_result.scan_count = 0;
        scan_target = &halow_async_scan_result;
    }

    if (scan_target != NULL && scan_target->scan_info == NULL) {
        scan_target->scan_info = hal_mem_alloc_large(HALOW_SCAN_RESULT_MAX * sizeof(wireless_scan_info_t));
        if (scan_target->scan_info == NULL) {
            if (halow_async_scan_buf != NULL) {
                hal_mem_free(halow_async_scan_buf);
                halow_async_scan_buf = NULL;
            }
            return -1;
        }
    }

    if (scan_target != NULL) {
        scan_target->scan_count = 0;
    }

    halow_scan_need_restore = (halow_state == NETIF_STATE_UP) ? 1 : 0;
    if (halow_scan_need_restore) {
        (void)mmwlan_sta_disable();
        halow_state = NETIF_STATE_DOWN;
    }

    halow_scan_user_cb = callback;
    halow_scan_in_progress = 1;

    scan_req.scan_rx_cb = halow_scan_rx_handler;
    scan_req.scan_complete_cb = halow_scan_complete_handler;
    scan_req.scan_cb_arg = scan_target;
    scan_req.args.dwell_time_ms = halow_netif_cfg.halow_cfg.scan_dwell_ms;
    scan_req.args.dwell_on_home_ms = MMWLAN_SCAN_DEFAULT_DWELL_ON_HOME_MS;

    osSemaphoreAcquire(halow_scan_sem, 0);

    status = mmwlan_scan_request(&scan_req);
    if (status != MMWLAN_SUCCESS) {
        halow_scan_in_progress = 0;
        halow_scan_user_cb = NULL;
        halow_scan_need_restore = 0;
        LOG_DRV_ERROR("HaLow scan_request failed: %d", (int)status);
        if (halow_async_scan_buf != NULL) {
            hal_mem_free(halow_async_scan_buf);
            halow_async_scan_buf = NULL;
        }
        return -1;
    }

    if (wait_ms > 0) {
        if (osSemaphoreAcquire(halow_scan_sem, wait_ms) != osOK) {
            (void)mmwlan_scan_abort();
            ret = -1;
        } else if (scan_target != NULL && scan_target->scan_count == 0) {
            ret = -1;
        }
        /* STA restore handled in complete handler when wait_ms > 0 */
    }

    return ret;
}

int mm_halow_netif_init(void)
{
    if (halow_mutex == NULL) {
        halow_mutex = osMutexNew(NULL);
        if (halow_mutex == NULL) {
            return -1;
        }
    }

    osMutexAcquire(halow_mutex, osWaitForever);

    if (halow_state != NETIF_STATE_DEINIT) {
        LOG_DRV_INFO("HaLow re-init: resetting previous session");
        halow_abort_operations_locked();
        halow_stack_deinit_locked();
    }

    if (halow_link_sem == NULL) {
        halow_link_sem = mmosal_semb_create("halow_link");
        if (halow_link_sem == NULL) {
            osMutexRelease(halow_mutex);
            return -1;
        }
    }

    if (!halow_pwr_acquired) {
        if (halow_power_acquire() != 0) {
            osMutexRelease(halow_mutex);
            return -1;
        }
    }

    if (halow_stack_init_locked() != 0) {
        goto init_fail;
    }

    halow_state = NETIF_STATE_DOWN;
    osMutexRelease(halow_mutex);
    LOG_DRV_INFO("HaLow netif initialized");
    return 0;

init_fail:
    halow_stack_deinit_locked();
    halow_netif_resources_deinit_locked();
    osMutexRelease(halow_mutex);
    return -1;
}

int mm_halow_netif_up(void)
{
    struct mmwlan_sta_args sta_args;
    enum mmwlan_status status;
    enum mmwlan_sta_state sta_state;
    int ip_init_ret = 0;

    if (halow_mutex == NULL) {
        return -1;
    }

    osMutexAcquire(halow_mutex, osWaitForever);

    if (!halow_stack_ready) {
        ip_init_ret = halow_mmipal_init_from_cfg();
        if (ip_init_ret != 0) {
            LOG_DRV_ERROR("HaLow mmipal_init failed (ret=%d)", ip_init_ret);
            osMutexRelease(halow_mutex);
            return -1;
        }
    }

    if (halow_state == NETIF_STATE_UP) {
        osMutexRelease(halow_mutex);
        return 0;
    }

    if (halow_ensure_mmwlan_booted_locked() != 0) {
        osMutexRelease(halow_mutex);
        return -1;
    }

    halow_fill_sta_args(&sta_args);

    {
        uint8_t mac[6];
        halow_apply_sta_mac_policy();
        halow_resolve_sta_mac(mac);
        (void)mmwlan_sta_set_mac_addr(mac);
        LOG_DRV_INFO("HaLow STA MAC " NETIF_MAC_STR_FMT, NETIF_MAC_PARAMETER(mac));
    }

    LOG_DRV_INFO("HaLow UP: ssid='%s' sec=%d ps=%u pmf=%u reg=%s txpwr=%u",
                 halow_netif_cfg.wireless_cfg.ssid,
                 (int)halow_netif_cfg.wireless_cfg.security,
                 halow_netif_cfg.halow_cfg.ps_mode,
                 halow_netif_cfg.halow_cfg.pmf_mode,
                 halow_netif_cfg.halow_cfg.country_code,
                 halow_netif_cfg.halow_cfg.tx_power_dbm);

    (void)mmwlan_set_power_save_mode(halow_netif_cfg.halow_cfg.ps_mode ? MMWLAN_PS_ENABLED : MMWLAN_PS_DISABLED);
    (void)mmwlan_set_dynamic_ps_timeout(halow_netif_cfg.halow_cfg.ps_mode ? 100 : 3600000U);
    (void)halow_apply_halow_hw_config_locked();

    status = mmwlan_sta_enable(&sta_args, mm_halow_sta_status_callback);
    LOG_DRV_INFO("HaLow mmwlan_sta_enable ret=%d sta_state=%d",
                 (int)status, (int)mmwlan_get_sta_state());
    if (status != MMWLAN_SUCCESS) {
        LOG_DRV_ERROR("mmwlan_sta_enable failed: %d", (int)status);
        osMutexRelease(halow_mutex);
        return -1;
    }

    /*
     * Avoid bus/chip sleeping immediately after connect.
     * Default dynamic PS timeout is ~100ms; on some platforms wake/busy signaling is marginal,
     * causing CMD53 timeouts and pageset write failures shortly after link-up.
     *
     * This API requires UMAC core running; calling here ensures it can take effect.
     */
    {
        /*
         * NOTE: morse_ps_update_timeout() computes new_timeout = now_ms + timeout_ms.
         * Using very large values like 0xFFFFFFFF can overflow and effectively become "now-1",
         * causing immediate sleep and CMD53 timeouts. Use a large-but-safe value instead.
         */
        const uint32_t dyn_ps_ms = halow_netif_cfg.halow_cfg.ps_mode ? 100U : 3600000U; /* 1 hour */
        enum mmwlan_status ps_status = mmwlan_set_dynamic_ps_timeout(dyn_ps_ms);
        LOG_DRV_INFO("HaLow set_dynamic_ps_timeout(%lu ms) ret=%d",
                     (unsigned long)dyn_ps_ms, (int)ps_status);
    }

    /* Short diagnostic poll: if STA VIF isn't added, mmwlan_get_sta_state often stays DISABLED. */
    for (int i = 0; i < 10; i++) {
        uint8_t mac[6];
        sta_state = mmwlan_get_sta_state();
        LOG_DRV_DEBUG("HaLow sta_state=%d (t+%dms)", (int)sta_state, (i + 1) * 200);
        halow_resolve_sta_mac(mac);
        halow_try_set_sta_mac_runtime(mac);
        if (sta_state == MMWLAN_STA_CONNECTED || sta_state == MMWLAN_STA_CONNECTING) {
            break;
        }
        osDelay(200);
    }

    if (halow_link_sem != NULL) {
        if (!mmosal_semb_wait(halow_link_sem, HALOW_LINK_WAIT_MS)) {
            LOG_DRV_ERROR("HaLow link wait timeout");
            (void)mmwlan_sta_disable();
            osMutexRelease(halow_mutex);
            return -1;
        }
    }

    halow_state = NETIF_STATE_UP;
    osMutexRelease(halow_mutex);
    return 0;
}

int mm_halow_netif_down(void)
{
    enum mmwlan_status status;

    if (halow_mutex == NULL || halow_state == NETIF_STATE_DEINIT) {
        return -1;
    }

    osMutexAcquire(halow_mutex, osWaitForever);

    if (halow_state == NETIF_STATE_UP) {
        status = mmwlan_sta_disable();
        if (status != MMWLAN_SUCCESS) {
            osMutexRelease(halow_mutex);
            return -1;
        }
        halow_state = NETIF_STATE_DOWN;
    }

    osMutexRelease(halow_mutex);
    return 0;
}

void mm_halow_netif_deinit(void)
{
    if (halow_mutex == NULL) {
        return;
    }

    osMutexAcquire(halow_mutex, osWaitForever);

    if (halow_state == NETIF_STATE_DEINIT) {
        osMutexRelease(halow_mutex);
        return;
    }

    halow_abort_operations_locked();
    halow_stack_deinit_locked();
    halow_netif_resources_deinit_locked();

    osMutexRelease(halow_mutex);
}

int mm_halow_netif_config(netif_config_t *netif_cfg)
{
    if (netif_cfg == NULL) {
        return -1;
    }
    if (halow_state == NETIF_STATE_UP) {
        return -1;
    }

    if (netif_cfg->host_name != NULL) {
        /* hostname stored elsewhere if needed */
    }

    memcpy(&halow_netif_cfg.wireless_cfg, &netif_cfg->wireless_cfg, sizeof(halow_netif_cfg.wireless_cfg));
    memcpy(&halow_netif_cfg.halow_cfg, &netif_cfg->halow_cfg, sizeof(halow_netif_cfg.halow_cfg));
    memcpy(halow_netif_cfg.diy_mac, netif_cfg->diy_mac, sizeof(halow_netif_cfg.diy_mac));
    halow_apply_sta_mac_policy();
    halow_netif_cfg.ip_mode = netif_cfg->ip_mode;
    memcpy(halow_netif_cfg.ip_addr, netif_cfg->ip_addr, sizeof(halow_netif_cfg.ip_addr));
    memcpy(halow_netif_cfg.netmask, netif_cfg->netmask, sizeof(halow_netif_cfg.netmask));
    memcpy(halow_netif_cfg.gw, netif_cfg->gw, sizeof(halow_netif_cfg.gw));

    /*
     * Before init: country_code is stored and applied in mm_halow_netif_init().
     * After init (DOWN): apply regdomain dynamically (restart radio if needed).
     */
    if (halow_state == NETIF_STATE_DEINIT) {
        return 0;
    }

    if (halow_apply_regdomain_locked(halow_netif_cfg.halow_cfg.country_code) != 0) {
        return -1;
    }

    return halow_apply_halow_hw_config_locked();
}

static void halow_u8_from_ip_str(const char *str, uint8_t *ip)
{
    unsigned int a, b, c, d;
    if (str == NULL || ip == NULL) {
        return;
    }
    if (sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
        ip[0] = (uint8_t)a;
        ip[1] = (uint8_t)b;
        ip[2] = (uint8_t)c;
        ip[3] = (uint8_t)d;
    }
}

int mm_halow_netif_info(netif_info_t *netif_info)
{
    struct netif *nif;
    struct mmipal_ip_config ip_cfg;
    struct mmwlan_version version;
    uint8_t mac[MMWLAN_MAC_ADDR_LEN];

    if (netif_info == NULL) {
        return -1;
    }

    memset(netif_info, 0, sizeof(*netif_info));
    netif_info->if_name = NETIF_NAME_WIFI_HALOW;
    netif_info->type = NETIF_TYPE_WIRELESS;
    netif_info->state = mm_halow_netif_state();
    memcpy(&netif_info->wireless_cfg, &halow_netif_cfg.wireless_cfg, sizeof(netif_info->wireless_cfg));
    memcpy(&netif_info->halow_cfg, &halow_netif_cfg.halow_cfg, sizeof(netif_info->halow_cfg));
    netif_info->ip_mode = halow_netif_cfg.ip_mode;
    memcpy(netif_info->ip_addr, halow_netif_cfg.ip_addr, sizeof(netif_info->ip_addr));
    memcpy(netif_info->netmask, halow_netif_cfg.netmask, sizeof(netif_info->netmask));
    memcpy(netif_info->gw, halow_netif_cfg.gw, sizeof(netif_info->gw));

    if (halow_stack_ready && mmipal_get_ip_config(&ip_cfg) == MMIPAL_SUCCESS) {
        halow_u8_from_ip_str(ip_cfg.ip_addr, netif_info->ip_addr);
        halow_u8_from_ip_str(ip_cfg.netmask, netif_info->netmask);
        halow_u8_from_ip_str(ip_cfg.gateway_addr, netif_info->gw);
        if (ip_cfg.mode == MMIPAL_DHCP) {
            netif_info->ip_mode = NETIF_IP_MODE_DHCP;
        } else {
            netif_info->ip_mode = NETIF_IP_MODE_STATIC;
        }
    }

    nif = mm_halow_netif_ptr();
    if (nif != NULL) {
        memcpy(netif_info->if_mac, nif->hwaddr, 6);
    } else if (mmwlan_get_mac_addr(mac) == MMWLAN_SUCCESS) {
        memcpy(netif_info->if_mac, mac, 6);
    }

    if (mmwlan_get_version(&version) == MMWLAN_SUCCESS) {
        snprintf(netif_info->fw_version, sizeof(netif_info->fw_version), "%s/%s",
                 version.morse_fw_version, version.morselib_version);
    }

    if (netif_info->state == NETIF_STATE_UP) {
        netif_info->rssi = (int)mmwlan_get_rssi();
    }

    return 0;
}

netif_state_t mm_halow_netif_state(void)
{
    if (halow_state == NETIF_STATE_DEINIT) {
        return NETIF_STATE_DEINIT;
    }
    if (mmwlan_get_sta_state() == MMWLAN_STA_CONNECTED) {
        struct netif *nif = mm_halow_netif_ptr();
        if (nif != NULL && netif_is_link_up(nif)) {
            return NETIF_STATE_UP;
        }
    }
    if (halow_stack_ready) {
        return NETIF_STATE_DOWN;
    }
    return halow_state;
}

struct netif *mm_halow_netif_ptr(void)
{
    if (!halow_stack_ready) {
        return NULL;
    }
    return mmipal_get_lwip_netif();
}

int mm_halow_start_scan(wireless_scan_callback_t callback)
{
    int ret;

    if (halow_mutex == NULL) {
        return -1;
    }

    osMutexAcquire(halow_mutex, osWaitForever);
    ret = halow_run_scan_locked(NULL, callback, 0);
    osMutexRelease(halow_mutex);
    return ret;
}

wireless_scan_result_t *mm_halow_get_storage_scan_result(void)
{
    return &halow_storage_scan_result;
}

int mm_halow_update_storage_scan_result(uint32_t timeout_ms)
{
    int ret;

    if (halow_mutex == NULL) {
        return -1;
    }

    osMutexAcquire(halow_mutex, osWaitForever);
    ret = halow_run_scan_locked(&halow_storage_scan_result, NULL, timeout_ms);
    osMutexRelease(halow_mutex);
    return ret;
}

int mm_halow_set_regdomain(const char *country_code)
{
    int ret;

    if (halow_mutex == NULL || country_code == NULL) {
        return -1;
    }

    osMutexAcquire(halow_mutex, osWaitForever);
    if (halow_state == NETIF_STATE_UP) {
        LOG_DRV_ERROR("HaLow regdomain: run 'ifconfig hw down' first");
        osMutexRelease(halow_mutex);
        return -1;
    }
    if (halow_scan_in_progress) {
        LOG_DRV_ERROR("HaLow regdomain: wait for scan to finish");
        osMutexRelease(halow_mutex);
        return -1;
    }
    ret = halow_apply_regdomain_locked(country_code);
    osMutexRelease(halow_mutex);
    return ret;
}

int mm_halow_set_tx_power(uint16_t tx_power_dbm)
{
    enum mmwlan_status status;

    if (halow_mutex == NULL) {
        return -1;
    }

    osMutexAcquire(halow_mutex, osWaitForever);
    halow_netif_cfg.halow_cfg.tx_power_dbm = tx_power_dbm;
    status = mmwlan_override_max_tx_power(tx_power_dbm);
    osMutexRelease(halow_mutex);

    return (status == MMWLAN_SUCCESS) ? 0 : -1;
}

int mm_halow_set_power_save(uint8_t enable)
{
    enum mmwlan_status status;

    if (halow_mutex == NULL) {
        return -1;
    }

    osMutexAcquire(halow_mutex, osWaitForever);
    halow_netif_cfg.halow_cfg.ps_mode = enable ? 1 : 0;
    status = mmwlan_set_power_save_mode(enable ? MMWLAN_PS_ENABLED : MMWLAN_PS_DISABLED);
    osMutexRelease(halow_mutex);

    return (status == MMWLAN_SUCCESS) ? 0 : -1;
}

int mm_halow_set_scan_config(uint32_t dwell_ms, uint8_t ndp_probe_enabled)
{
    struct mmwlan_scan_config scan_cfg = MMWLAN_SCAN_CONFIG_INIT;
    enum mmwlan_status status;

    if (halow_mutex == NULL) {
        return -1;
    }

    osMutexAcquire(halow_mutex, osWaitForever);
    halow_netif_cfg.halow_cfg.scan_dwell_ms = dwell_ms;
    halow_netif_cfg.halow_cfg.ndp_probe_enabled = ndp_probe_enabled;
    scan_cfg.dwell_time_ms = dwell_ms;
    scan_cfg.ndp_probe_enabled = (ndp_probe_enabled != 0);
    scan_cfg.home_channel_dwell_time_ms = MMWLAN_SCAN_DEFAULT_DWELL_ON_HOME_MS;
    status = mmwlan_set_scan_config(&scan_cfg);
    osMutexRelease(halow_mutex);

    return (status == MMWLAN_SUCCESS) ? 0 : -1;
}

unsigned mm_halow_regdomain_count(void)
{
    const struct mmwlan_regulatory_db *db = get_regulatory_db();

    if (db == NULL) {
        return 0;
    }
    return db->num_domains;
}

int mm_halow_regdomain_get_code(unsigned index, char *country_code, size_t len)
{
    const struct mmwlan_regulatory_db *db = get_regulatory_db();
    const struct mmwlan_s1g_channel_list *domain;

    if (db == NULL || country_code == NULL || len < MM_HALOW_REGDOMAIN_CC_LEN) {
        return -1;
    }
    if (index >= db->num_domains) {
        return -1;
    }

    domain = db->domains[index];
    if (domain == NULL) {
        return -1;
    }

    memcpy(country_code, domain->country_code, MM_HALOW_REGDOMAIN_CC_LEN);
    country_code[MM_HALOW_REGDOMAIN_CC_LEN - 1U] = '\0';
    return 0;
}

int mm_halow_list_regdomains(void)
{
    const struct mmwlan_regulatory_db *db = get_regulatory_db();
    const char *current = halow_netif_cfg.halow_cfg.country_code;
    unsigned i;

    if (db == NULL) {
        printf("HaLow regdomain DB unavailable\r\n");
        return -1;
    }

    printf("HaLow regdomains (%u):\r\n", db->num_domains);
    for (i = 0; i < db->num_domains; i++) {
        const struct mmwlan_s1g_channel_list *domain = db->domains[i];
        char cc[MM_HALOW_REGDOMAIN_CC_LEN];

        if (domain == NULL) {
            continue;
        }
        memcpy(cc, domain->country_code, sizeof(cc));
        cc[sizeof(cc) - 1U] = '\0';

        printf("  %s (%u ch)", cc, domain->num_channels);
        if (current[0] != '\0' &&
            strncmp(current, cc, 2) == 0) {
            printf(" *");
        }
        printf("\r\n");
    }

    if (current[0] != '\0') {
        printf("Current cfg: %s\r\n", current);
    }
    return 0;
}

int mm_halow_print_version(void)
{
    struct mmwlan_version version;

    if (mmwlan_get_version(&version) != MMWLAN_SUCCESS) {
        printf("HaLow version query failed\r\n");
        return -1;
    }

    printf("HaLow FW: %s\r\n", version.morse_fw_version);
    printf("HaLow Lib: %s\r\n", version.morselib_version);
    printf("HaLow Chip: %s (0x%08lX)\r\n", version.morse_chip_id_string, (unsigned long)version.morse_chip_id);
    return 0;
}

#if defined(MMWLAN_DPP_DISABLED) && MMWLAN_DPP_DISABLED

int mm_halow_dpp_start(uint32_t timeout_ms, uint8_t auto_up)
{
    MM_UNUSED(timeout_ms);
    MM_UNUSED(auto_up);
    LOG_DRV_ERROR("HaLow DPP not available (MMWLAN_DPP_DISABLED in libmorse)");
    return -1;
}

int mm_halow_dpp_stop(void)
{
    return -1;
}

uint8_t mm_halow_dpp_is_active(void)
{
    return 0;
}

#else /* MMWLAN_DPP_DISABLED */

static void halow_dpp_apply_credentials(const struct mmwlan_dpp_cb_args *ev)
{
    const uint8_t *ssid;
    uint16_t ssid_len;
    const char *pass;
    size_t copy_len;

    if (ev == NULL) {
        return;
    }

    ssid = ev->args.pb_result.ssid;
    ssid_len = ev->args.pb_result.ssid_len;
    pass = ev->args.pb_result.passphrase;

    if (ssid != NULL && ssid_len > 0) {
        copy_len = ssid_len;
        if (copy_len >= NETIF_SSID_VALUE_SIZE) {
            copy_len = NETIF_SSID_VALUE_SIZE - 1;
        }
        memcpy(halow_netif_cfg.wireless_cfg.ssid, ssid, copy_len);
        halow_netif_cfg.wireless_cfg.ssid[copy_len] = '\0';
    }

    if (pass != NULL && pass[0] != '\0') {
        strncpy(halow_netif_cfg.wireless_cfg.pw, pass, NETIF_PW_VALUE_SIZE - 1);
        halow_netif_cfg.wireless_cfg.pw[NETIF_PW_VALUE_SIZE - 1] = '\0';
        halow_netif_cfg.wireless_cfg.security = WIRELESS_SAE;
    } else {
        memset(halow_netif_cfg.wireless_cfg.pw, 0, sizeof(halow_netif_cfg.wireless_cfg.pw));
        halow_netif_cfg.wireless_cfg.security = WIRELESS_OPEN;
    }
}

static void mm_halow_dpp_event_cb(const struct mmwlan_dpp_cb_args *dpp_event, void *arg)
{
    MM_UNUSED(arg);

    if (dpp_event == NULL || dpp_event->event != MMWLAN_DPP_EVT_PB_RESULT) {
        return;
    }

    halow_dpp_last_result = dpp_event->args.pb_result.result;

    if (halow_dpp_last_result == MMWLAN_DPP_PB_RESULT_SUCCESS) {
        halow_dpp_apply_credentials(dpp_event);
        LOG_DRV_INFO("HaLow DPP success: ssid='%s'", halow_netif_cfg.wireless_cfg.ssid);
    } else if (halow_dpp_last_result == MMWLAN_DPP_PB_RESULT_SESSION_OVERLAP) {
        LOG_DRV_WARN("HaLow DPP session overlap (multiple configurators?)");
    } else {
        LOG_DRV_ERROR("HaLow DPP failed (result=%d)", (int)halow_dpp_last_result);
    }

    if (halow_dpp_done_sem != NULL) {
        (void)mmosal_semb_give(halow_dpp_done_sem);
    }
}

uint8_t mm_halow_dpp_is_active(void)
{
    return halow_dpp_active;
}

int mm_halow_dpp_stop(void)
{
    (void)mmwlan_dpp_stop();
    if (halow_dpp_active) {
        halow_dpp_active = 0;
        if (halow_dpp_done_sem != NULL) {
            (void)mmosal_semb_give(halow_dpp_done_sem);
        }
    }
    return 0;
}

int mm_halow_dpp_start(uint32_t timeout_ms, uint8_t auto_up)
{
    struct mmwlan_dpp_args dpp_args = {0};
    enum mmwlan_status status;
    int ret = -1;

    if (timeout_ms == 0) {
        timeout_ms = HALOW_DPP_DEFAULT_TIMEOUT_MS;
    }

    if (halow_mutex == NULL || halow_state == NETIF_STATE_DEINIT) {
        LOG_DRV_ERROR("HaLow DPP: call 'ifconfig hw init' first");
        return -1;
    }

    osMutexAcquire(halow_mutex, osWaitForever);
    if (halow_state == NETIF_STATE_UP) {
        LOG_DRV_ERROR("HaLow DPP: interface is up, run 'ifconfig hw down' first");
        osMutexRelease(halow_mutex);
        return -1;
    }
    if (halow_scan_in_progress) {
        LOG_DRV_ERROR("HaLow DPP: scan in progress");
        osMutexRelease(halow_mutex);
        return -1;
    }
    if (halow_ensure_mmwlan_booted_locked() != 0) {
        osMutexRelease(halow_mutex);
        return -1;
    }
    osMutexRelease(halow_mutex);

    if (halow_dpp_done_sem == NULL) {
        halow_dpp_done_sem = mmosal_semb_create("halow_dpp");
        if (halow_dpp_done_sem == NULL) {
            return -1;
        }
    }

    while (mmosal_semb_wait(halow_dpp_done_sem, 0)) {
        /* drain stale completion */
    }

    (void)mm_halow_dpp_stop();

    dpp_args.dpp_event_cb = mm_halow_dpp_event_cb;
    dpp_args.dpp_event_cb_arg = NULL;

    printf("HaLow DPP: press the AP/configurator button now (timeout %lu s)\r\n",
           (unsigned long)(timeout_ms / 1000U));

    status = mmwlan_dpp_start(&dpp_args);
    if (status != MMWLAN_SUCCESS) {
        LOG_DRV_ERROR("mmwlan_dpp_start failed: %d (set regdomain?)", (int)status);
        return -1;
    }
    halow_dpp_active = 1;

    if (!mmosal_semb_wait(halow_dpp_done_sem, timeout_ms)) {
        LOG_DRV_ERROR("HaLow DPP wait timeout");
        (void)mm_halow_dpp_stop();
        return -1;
    }

    (void)mmwlan_dpp_stop();
    halow_dpp_active = 0;

    if (halow_dpp_last_result == MMWLAN_DPP_PB_RESULT_SESSION_OVERLAP) {
        printf("HaLow DPP: session overlap\r\n");
        return -1;
    }

    if (halow_dpp_last_result != MMWLAN_DPP_PB_RESULT_SUCCESS) {
        printf("HaLow DPP: failed\r\n");
        return -1;
    }

    printf("HaLow DPP OK: ssid=%s sec=%s\r\n",
           halow_netif_cfg.wireless_cfg.ssid,
           halow_netif_cfg.wireless_cfg.security == WIRELESS_SAE ? "sae" : "open");

    if (auto_up) {
        ret = mm_halow_netif_up();
        if (ret != 0) {
            printf("HaLow DPP: credentials saved, but 'up' failed\r\n");
        }
        return ret;
    }

    printf("HaLow DPP: run 'ifconfig hw up' to connect\r\n");
    return 0;
}

#endif /* MMWLAN_DPP_DISABLED */

int mm_halow_netif_ctrl(const char *if_name, netif_cmd_t cmd, void *param)
{
    int ret = -1;
    netif_state_t if_state = NETIF_STATE_MAX;

    if (if_name == NULL || strcmp(if_name, NETIF_NAME_WIFI_HALOW) != 0) {
        return -1;
    }

    switch (cmd) {
    case NETIF_CMD_CFG:
        ret = mm_halow_netif_config((netif_config_t *)param);
        break;
    case NETIF_CMD_INIT:
        ret = mm_halow_netif_init();
        break;
    case NETIF_CMD_UP:
        ret = mm_halow_netif_up();
        break;
    case NETIF_CMD_INFO:
        ret = mm_halow_netif_info((netif_info_t *)param);
        break;
    case NETIF_CMD_DOWN:
        ret = mm_halow_netif_down();
        break;
    case NETIF_CMD_UNINIT:
        mm_halow_netif_deinit();
        ret = 0;
        break;
    case NETIF_CMD_STATE:
        if (param == NULL) {
            ret = -1;
        } else {
            *(netif_state_t *)param = mm_halow_netif_state();
            ret = 0;
        }
        break;
    case NETIF_CMD_CFG_EX:
        if_state = mm_halow_netif_state();
        if (if_state == NETIF_STATE_UP) {
            ret = mm_halow_netif_down();
            if (ret != 0) {
                break;
            }
        }
        ret = mm_halow_netif_config((netif_config_t *)param);
        if (ret != 0) {
            break;
        }
        if (if_state == NETIF_STATE_UP) {
            ret = mm_halow_netif_up();
        }
        break;
    default:
        break;
    }

    return ret;
}

#endif /* NETIF_WIFI_HALOW_IS_ENABLE */
