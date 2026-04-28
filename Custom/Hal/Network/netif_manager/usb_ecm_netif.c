#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "cmsis_os2.h"
#include "aicam_error.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/tcpip.h"
#include "lwip/netifapi.h"
#include "Log/debug.h"
#include "mem.h"
#include "usb_host_ecm.h"
#include "usb_ecm_netif.h"
#include "ms_modem.h"

// Basic USB ECM events
#define USB_ECM_EVENT_UP                    (1 << 0)
#define USB_ECM_EVENT_DOWN                  (1 << 1)
#define USB_ECM_EVENT_ACTIVATE              (1 << 2)

// USB ECM netif instance
static struct netif ecm_netif = {
    .name = {NETIF_NAME_USB_ECM[0], NETIF_NAME_USB_ECM[1]},
};

// Stored config (defaults from netif_manager.h)
static netif_config_t usb_ecm_netif_cfg = {
    .ip_mode = NETIF_USB_ECM_DEFAULT_IP_MODE,
    .ip_addr = NETIF_USB_ECM_DEFAULT_IP,
    .netmask = NETIF_USB_ECM_DEFAULT_MASK,
    .gw = NETIF_USB_ECM_DEFAULT_GW,
};

#if NETIF_USB_ECM_IS_CAT1_MODULE
/// @brief 4G network interface status information
static cellular_info_t usb_ecm_cellular_info = {0};
#endif

static osEventFlagsId_t usb_ecm_netif_events = NULL;
static osMutexId_t usb_ecm_netif_mutex = NULL;
static volatile uint32_t usb_ecm_netif_link_flags = 0;
static osMessageQueueId_t usb_ecm_tx_queue = NULL;
static osThreadId_t usb_ecm_tx_thread = NULL;
static volatile uint8_t usb_ecm_tx_run = 0;

typedef struct {
    uint16_t len;
    uint8_t *buf;
} usb_ecm_tx_item_t;

static void usb_ecm_tx_worker(void *argument)
{
    (void)argument;
    usb_ecm_tx_item_t item = {0};

    while (usb_ecm_tx_run) {
        if (osMessageQueueGet(usb_ecm_tx_queue, &item, NULL, osWaitForever) != osOK) continue;
        if (item.buf == NULL || item.len == 0) continue;

        if ((usb_ecm_netif_link_flags & USB_ECM_EVENT_UP) == 0) {
            hal_mem_free(item.buf);
            continue;
        }

        NX_PACKET packet = {0};
        packet.nx_packet_ptr = item.buf;
        packet.nx_packet_length = item.len;

        if (usb_host_ecm_send_raw_data(&packet) == 0) {
            (void)usb_host_ecm_wait_tx_done(50);
        }
        hal_mem_free(item.buf);
    }
}

static void usb_ecm_netif_low_level_input(struct netif *netif, uint8_t *b, uint16_t len)
{
    struct pbuf *p = NULL, *q = NULL;
    uint32_t bufferoffset = 0;
    int ret = ERR_OK;

    if (len <= 0) return;
    if (len < NETIF_LWIP_FRAME_ALIGNMENT) len = NETIF_LWIP_FRAME_ALIGNMENT;

    /* We allocate a pbuf chain of pbufs from the Lwip buffer pool
    * and copy the data to the pbuf chain
    */
    // printf("IN len = %d\n", len);
    if ((p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL)) != ((struct pbuf *)0)) {
        for (q = p, bufferoffset = 0; q != NULL; q = q->next) {
            memcpy((uint8_t *)q->payload, (uint8_t *)b + bufferoffset, q->len);
            bufferoffset += q->len;
        }
        ret = netif->input(p, netif);
        if (ret != ERR_OK) {
            pbuf_free(p);
            // printf(NETIF_NAME_STR_FMT ": Input failed(ret = %d)!", NETIF_NAME_PARAMETER(netif), ret);
        }
    } else {
        // printf(NETIF_NAME_STR_FMT ": Failed to allocate pbuf!", NETIF_NAME_PARAMETER(netif));
    }
}

static err_t usb_ecm_netif_low_level_output(struct netif *netif, struct pbuf *p)
{
    struct pbuf *q = NULL;

    if (netif == NULL || p == NULL) return ERR_ARG;

    /* Do not transmit if link is down/deactivated. */
    if ((usb_ecm_netif_link_flags & USB_ECM_EVENT_UP) == 0) return ERR_IF;

    if (usb_ecm_tx_queue == NULL) return ERR_IF;

    uint16_t len = (uint16_t)p->tot_len;
    uint8_t *buf = hal_mem_alloc(len, MEM_LARGE);
    if (buf == NULL) return ERR_MEM;

    uint16_t off = 0;
    for (q = p; q != NULL; q = q->next) {
        memcpy(buf + off, (uint8_t *)q->payload, q->len);
        off += (uint16_t)q->len;
    }

    usb_ecm_tx_item_t item = {.len = len, .buf = buf};
    if (osMessageQueuePut(usb_ecm_tx_queue, &item, 0, 0) != osOK) {
        hal_mem_free(buf);
        return ERR_MEM;
    }
    return ERR_OK;
}

static err_t usb_ecm_netif_ethernetif_init(struct netif *netif)
{
    if (netif == NULL) return ERR_ARG;

    netif->hwaddr_len = ETH_HWADDR_LEN;
#if LWIP_NETIF_HOSTNAME
    netif->hostname = usb_ecm_netif_cfg.host_name;
#endif
#if LWIP_IPV4 && LWIP_ARP
    netif->output = etharp_output;
#endif
    netif->linkoutput = usb_ecm_netif_low_level_output;

    netif->mtu = NETIF_MAX_TRANSFER_UNIT;
    netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_IGMP;

    return ERR_OK;
}

/* Run lwIP netif changes on tcpip thread — never block USBX enum/class threads on netifapi. */
static void usb_ecm_tcpip_link_up_fn(void *ctx)
{
    (void)ctx;
    netif_set_link_up(&ecm_netif);
    if (usb_ecm_netif_events != NULL) {
        osEventFlagsSet(usb_ecm_netif_events, USB_ECM_EVENT_UP);
    }
    usb_ecm_netif_link_flags |= USB_ECM_EVENT_UP;
    usb_ecm_netif_link_flags &= ~USB_ECM_EVENT_DOWN;
}

static void usb_ecm_tcpip_link_down_fn(void *ctx)
{
    (void)ctx;
    (void)netifapi_dhcp_stop(&ecm_netif);
    netif_set_down(&ecm_netif);
    netif_set_link_down(&ecm_netif);
    if (usb_ecm_netif_events != NULL) {
        osEventFlagsSet(usb_ecm_netif_events, USB_ECM_EVENT_DOWN);
    }
}

static void usb_ecm_netif_event_callback(usb_host_ecm_event_type_t event, void *arg)
{
    // __attribute__((unused)) err_t ret = ERR_OK;

    switch (event) {
        case USB_HOST_ECM_EVENT_ACTIVATE:
            if (memcmp(ecm_netif.hwaddr, (uint8_t *)arg, sizeof(ecm_netif.hwaddr)) != 0) {
                memcpy(ecm_netif.hwaddr, (uint8_t *)arg, sizeof(ecm_netif.hwaddr));
            }
            osEventFlagsSet(usb_ecm_netif_events, USB_ECM_EVENT_ACTIVATE);
            break;
        case USB_HOST_ECM_EVENT_UP:
            if (tcpip_callback(usb_ecm_tcpip_link_up_fn, NULL) != ERR_OK) {
                LOG_DRV_ERROR("usb_ecm: tcpip_callback(link up) failed");
            }
            break;
        case USB_HOST_ECM_EVENT_DEACTIVATE:
        case USB_HOST_ECM_EVENT_DOWN:
            /* Stop TX path immediately; full netif teardown runs on tcpip thread. */
            usb_ecm_netif_link_flags |= USB_ECM_EVENT_DOWN;
            usb_ecm_netif_link_flags &= ~USB_ECM_EVENT_UP;
            if (tcpip_callback(usb_ecm_tcpip_link_down_fn, NULL) != ERR_OK) {
                LOG_DRV_ERROR("usb_ecm: tcpip_callback(link down) failed");
            }
            break;
        case USB_HOST_ECM_EVENT_DATA:
            usb_ecm_netif_low_level_input(&ecm_netif, ((NX_PACKET *)arg)->nx_packet_ptr, ((NX_PACKET *)arg)->nx_packet_length);
            break;
        default:
            break;
    }
}

int usb_ecm_netif_init(void)
{
    int ret = AICAM_OK;
    struct netif *ue = NULL;
    uint32_t event = 0;
#if NETIF_USB_ECM_IS_CAT1_MODULE
    uint8_t try_count = 0;
    if (modem_device_get_state() != MODEM_STATE_UNINIT) return AICAM_ERROR_BUSY;
#endif

    ue = netif_get_by_index(ecm_netif.num + 1);
    if (ue != NULL && ue == &ecm_netif) return -1;
    ue = NULL;

#if NETIF_USB_ECM_IS_CAT1_MODULE
    do {
        if (ret != 0) {
            modem_device_deinit();
            osDelay(NETIF_4G_CAT1_PPP_INTERVAL_MS);
        }
        ret = modem_device_init();
    } while (ret != 0 && ++try_count < NETIF_4G_CAT1_TRY_CNT);
    if (ret != 0) return ret;

    ret = modem_device_get_info((modem_info_t *)&usb_ecm_cellular_info, 1);
    if (ret != 0) {
        LOG_DRV_ERROR("modem get info failed(ret = %d)!", ret);
        modem_device_deinit();
        return ret;
    }

    ret = modem_device_get_config((modem_config_t *)&usb_ecm_netif_cfg.cellular_cfg);
    if (ret != 0) {
        LOG_DRV_ERROR("modem get config failed(ret = %d)!", ret);
        modem_device_deinit();
        return ret;
    }

    ret = modem_device_check_and_enable_ecm();
    if (ret != 0) {
        LOG_DRV_ERROR("modem check and enable ecm failed(ret = %d)!", ret);
        modem_device_deinit();
        return ret;
    }
#endif

    usb_ecm_netif_events = osEventFlagsNew(NULL);
    if (usb_ecm_netif_events == NULL) {
        ret = AICAM_ERROR_NO_MEMORY;
        goto usb_ecm_netif_init_exit;
    }
    usb_ecm_netif_mutex = osMutexNew(NULL);
    if (usb_ecm_netif_mutex == NULL) {
        ret = AICAM_ERROR_NO_MEMORY;
        goto usb_ecm_netif_init_exit;
    }

    usb_ecm_tx_queue = osMessageQueueNew(8, sizeof(usb_ecm_tx_item_t), NULL);
    if (usb_ecm_tx_queue == NULL) {
        ret = AICAM_ERROR_NO_MEMORY;
        goto usb_ecm_netif_init_exit;
    }
    usb_ecm_tx_run = 1;
    usb_ecm_tx_thread = osThreadNew(usb_ecm_tx_worker, NULL, NULL);
    if (usb_ecm_tx_thread == NULL) {
        usb_ecm_tx_run = 0;
        ret = AICAM_ERROR_NO_MEMORY;
        goto usb_ecm_netif_init_exit;
    }

    // Register lwIP netif (run on tcpip thread)
    err_t nif_err = netifapi_netif_add(&ecm_netif, NULL, NULL, NULL, NULL, &usb_ecm_netif_ethernetif_init, &tcpip_input);
    if (nif_err != ERR_OK) {
        ret = AICAM_ERROR;
        goto usb_ecm_netif_init_exit;
    }

    osEventFlagsClear(usb_ecm_netif_events, USB_ECM_EVENT_ACTIVATE);

    ret = usb_host_ecm_init(usb_ecm_netif_event_callback);
    if (ret != 0) goto usb_ecm_netif_init_exit;

    // Wait activate callback within timeout
    event = osEventFlagsWait(usb_ecm_netif_events, USB_ECM_EVENT_ACTIVATE, osFlagsWaitAny, NETIF_USB_ECM_ACTIVATE_TIMEOUT_MS);
    if (event & osFlagsError) {
        // Timeout or error
        ret = AICAM_ERROR_TIMEOUT;
        goto usb_ecm_netif_init_exit;
    }

usb_ecm_netif_init_exit:
    if (ret != AICAM_OK) {
        usb_host_ecm_deinit();
        (void)netifapi_netif_remove(&ecm_netif);
        if (usb_ecm_netif_events != NULL) {
            osEventFlagsDelete(usb_ecm_netif_events);
            usb_ecm_netif_events = NULL;
        }
        if (usb_ecm_netif_mutex != NULL) {
            osMutexDelete(usb_ecm_netif_mutex);
            usb_ecm_netif_mutex = NULL;
        }
        if (usb_ecm_tx_thread != NULL) {
            (void)osThreadTerminate(usb_ecm_tx_thread);
            usb_ecm_tx_thread = NULL;
        }
        usb_ecm_tx_run = 0;
        if (usb_ecm_tx_queue != NULL) {
            usb_ecm_tx_item_t item = {0};
            while (osMessageQueueGet(usb_ecm_tx_queue, &item, NULL, 0) == osOK) {
                if (item.buf != NULL) hal_mem_free(item.buf);
            }
            osMessageQueueDelete(usb_ecm_tx_queue);
            usb_ecm_tx_queue = NULL;
        }
    #if NETIF_USB_ECM_IS_CAT1_MODULE
        modem_device_deinit();
    #endif
    }
    return ret;
}

int usb_ecm_netif_up(void) 
{ 
    struct netif *ue = NULL;
    int ret = 0;
    ip_addr_t ipaddr  = { 0 };
    ip_addr_t gateway = { 0 };
    ip_addr_t netmask = { 0 };
    uint32_t event = 0;
    uint32_t start_tick = 0, end_tick, diff_tick = 0;

    ue = netif_get_by_index(ecm_netif.num + 1);
    if (ue == NULL || ue != &ecm_netif) return AICAM_ERROR_NOT_SUPPORTED;

    if (!netif_is_link_up(&ecm_netif)) {
        osEventFlagsClear(usb_ecm_netif_events, USB_ECM_EVENT_UP);
        event = osEventFlagsWait(usb_ecm_netif_events, USB_ECM_EVENT_UP, osFlagsWaitAny, NETIF_USB_ECM_UP_TIMEOUT_MS);
        if ((event & osFlagsError) && !netif_is_link_up(&ecm_netif)) return AICAM_ERROR_TIMEOUT;
    }

    osEventFlagsClear(usb_ecm_netif_events, USB_ECM_EVENT_UP | USB_ECM_EVENT_DOWN);
    start_tick = HAL_GetTick();
    do {
        event = osEventFlagsWait(usb_ecm_netif_events, USB_ECM_EVENT_UP | USB_ECM_EVENT_DOWN, osFlagsWaitAny, NETIF_USB_ECM_STABLE_TIME_MS);
        if (event & osFlagsError) break;
        end_tick = HAL_GetTick();
        diff_tick = (end_tick >= start_tick) ? (end_tick - start_tick) : (0xFFFFFFFFU - start_tick + end_tick);
    } while (diff_tick < NETIF_USB_ECM_STABLE_TIMEOUT_MS);

    if (diff_tick >= NETIF_USB_ECM_STABLE_TIMEOUT_MS) return AICAM_ERROR_TIMEOUT;

    IP4_ADDR(&ipaddr, usb_ecm_netif_cfg.ip_addr[0], usb_ecm_netif_cfg.ip_addr[1], usb_ecm_netif_cfg.ip_addr[2], usb_ecm_netif_cfg.ip_addr[3]);
    IP4_ADDR(&gateway, usb_ecm_netif_cfg.gw[0], usb_ecm_netif_cfg.gw[1], usb_ecm_netif_cfg.gw[2], usb_ecm_netif_cfg.gw[3]);
    IP4_ADDR(&netmask, usb_ecm_netif_cfg.netmask[0], usb_ecm_netif_cfg.netmask[1], usb_ecm_netif_cfg.netmask[2], usb_ecm_netif_cfg.netmask[3]);
    netifapi_netif_set_addr(&ecm_netif, &ipaddr, &netmask, &gateway);
    ret = netifapi_netif_set_up(&ecm_netif);
    if (ret != ERR_OK) return AICAM_ERROR;
    if (usb_ecm_netif_cfg.ip_mode == NETIF_IP_MODE_DHCP) {
        ip_addr_set_zero_ip4(&(ecm_netif.ip_addr));
        ip_addr_set_zero_ip4(&(ecm_netif.netmask));
        ip_addr_set_zero_ip4(&(ecm_netif.gw));
        ret = netifapi_dhcp_start(&ecm_netif);
        if (ret != ERR_OK){
            netifapi_netif_set_down(&ecm_netif);
            return AICAM_ERROR;
        }
        start_tick = HAL_GetTick();
        do {
            event = osEventFlagsWait(usb_ecm_netif_events, USB_ECM_EVENT_DOWN, osFlagsWaitAny, 100);
            if (!(event & osFlagsError)) {
                (void)netifapi_dhcp_stop(&ecm_netif);
                netifapi_netif_set_down(&ecm_netif);
                return AICAM_ERROR;
            }
            if (dhcp_supplied_address(&ecm_netif)) {
                LOG_DRV_INFO(NETIF_NAME_STR_FMT " dhcp ip: %s", NETIF_NAME_PARAMETER(&ecm_netif), ip4addr_ntoa((const ip4_addr_t *)&ecm_netif.ip_addr));
                break;
            }
            end_tick = HAL_GetTick();
            diff_tick = (end_tick >= start_tick) ? (end_tick - start_tick) : (0xFFFFFFFFU - start_tick + end_tick);
        } while (diff_tick < NETIF_USB_ECM_DHCP_TIMEOUT_MS);
        if (diff_tick >= NETIF_USB_ECM_DHCP_TIMEOUT_MS) {
            (void)netifapi_dhcp_stop(&ecm_netif);
            netifapi_netif_set_down(&ecm_netif);
            return AICAM_ERROR_TIMEOUT;
        }
        usb_ecm_netif_cfg.ip_addr[0] = ip4_addr1(&ecm_netif.ip_addr);
        usb_ecm_netif_cfg.ip_addr[1] = ip4_addr2(&ecm_netif.ip_addr);
        usb_ecm_netif_cfg.ip_addr[2] = ip4_addr3(&ecm_netif.ip_addr);
        usb_ecm_netif_cfg.ip_addr[3] = ip4_addr4(&ecm_netif.ip_addr);
        usb_ecm_netif_cfg.gw[0] = ip4_addr1(&ecm_netif.gw);
        usb_ecm_netif_cfg.gw[1] = ip4_addr2(&ecm_netif.gw);
        usb_ecm_netif_cfg.gw[2] = ip4_addr3(&ecm_netif.gw);
        usb_ecm_netif_cfg.gw[3] = ip4_addr4(&ecm_netif.gw);
        usb_ecm_netif_cfg.netmask[0] = ip4_addr1(&ecm_netif.netmask);
        usb_ecm_netif_cfg.netmask[1] = ip4_addr2(&ecm_netif.netmask);
        usb_ecm_netif_cfg.netmask[2] = ip4_addr3(&ecm_netif.netmask);
        usb_ecm_netif_cfg.netmask[3] = ip4_addr4(&ecm_netif.netmask);
    }
    return AICAM_OK;
}

int usb_ecm_netif_down(void) 
{
    struct netif *ue = NULL;

    ue = netif_get_by_index(ecm_netif.num + 1);
    if (ue == NULL || ue != &ecm_netif) return AICAM_ERROR_NOT_SUPPORTED;
    
    (void)netifapi_dhcp_stop(&ecm_netif);
    netifapi_netif_set_down(&ecm_netif);
    return AICAM_OK;
}

void usb_ecm_netif_deinit(void)
{
    struct netif *ue = NULL;

    ue = netif_get_by_index(ecm_netif.num + 1);
    if (ue == NULL || ue != &ecm_netif) return;

    (void)netifapi_dhcp_stop(&ecm_netif);
    netifapi_netif_set_down(&ecm_netif);
    netifapi_netif_set_link_down(&ecm_netif);
    (void)netifapi_netif_remove(&ecm_netif);

#if NETIF_USB_ECM_IS_CAT1_MODULE
    modem_device_deinit();
#endif
    usb_host_ecm_deinit();

    if (usb_ecm_tx_thread != NULL) {
        usb_ecm_tx_run = 0;
        (void)osThreadTerminate(usb_ecm_tx_thread);
        usb_ecm_tx_thread = NULL;
    }
    if (usb_ecm_tx_queue != NULL) {
        usb_ecm_tx_item_t item = {0};
        while (osMessageQueueGet(usb_ecm_tx_queue, &item, NULL, 0) == osOK) {
            if (item.buf != NULL) hal_mem_free(item.buf);
        }
        osMessageQueueDelete(usb_ecm_tx_queue);
        usb_ecm_tx_queue = NULL;
    }

    if (usb_ecm_netif_events != NULL) {
        osEventFlagsDelete(usb_ecm_netif_events);
        usb_ecm_netif_events = NULL;
    }
    if (usb_ecm_netif_mutex != NULL) {
        osMutexDelete(usb_ecm_netif_mutex);
        usb_ecm_netif_mutex = NULL;
    }
}

int usb_ecm_netif_config(netif_config_t *netif_cfg)
{
#if NETIF_USB_ECM_IS_CAT1_MODULE
    int ret = 0;
#endif
    if (netif_cfg == NULL) return AICAM_ERROR_INVALID_PARAM;
    if (usb_ecm_netif_state() != NETIF_STATE_DOWN) return AICAM_ERROR_BUSY;

#if LWIP_NETIF_HOSTNAME
    if (netif_cfg->host_name != NULL) ecm_netif.hostname = netif_cfg->host_name;
#endif
    
#if NETIF_USB_ECM_IS_CAT1_MODULE
    ret = modem_device_set_config((modem_config_t *)&netif_cfg->cellular_cfg);
    if (ret != 0) {
        LOG_DRV_ERROR("modem set config failed(ret = %d)!", ret);
        return ret;
    }
#endif

    memcpy(&usb_ecm_netif_cfg, netif_cfg, sizeof(usb_ecm_netif_cfg));
    return 0;
}

int usb_ecm_netif_info(netif_info_t *netif_info)
{
    if (netif_info == NULL) return AICAM_ERROR_INVALID_PARAM;
#if LWIP_NETIF_HOSTNAME
    netif_info->host_name = ecm_netif.hostname;
#else
    netif_info->host_name = NULL;
#endif
    netif_info->if_name = NETIF_NAME_USB_ECM;
#if NETIF_USB_ECM_IS_CAT1_MODULE
    netif_info->type = NETIF_TYPE_4G;
#else
    netif_info->type = NETIF_TYPE_ETH;
#endif
    netif_info->state = usb_ecm_netif_state();
    netif_info->rssi = 0;
    netif_info->ip_mode = usb_ecm_netif_cfg.ip_mode;
    memcpy(netif_info->if_mac, ecm_netif.hwaddr, sizeof(netif_info->if_mac));
    memcpy(netif_info->ip_addr, &ecm_netif.ip_addr, sizeof(netif_info->ip_addr));
    memcpy(netif_info->gw, &ecm_netif.gw, sizeof(netif_info->gw));
    memcpy(netif_info->netmask, &ecm_netif.netmask, sizeof(netif_info->netmask));
    memset(netif_info->fw_version, 0, sizeof(netif_info->fw_version));
    
#if NETIF_USB_ECM_IS_CAT1_MODULE
    if (netif_info->state >= NETIF_STATE_DOWN) {
        modem_device_get_info((modem_info_t *)&usb_ecm_cellular_info, 0);
        modem_device_get_config((modem_config_t *)&usb_ecm_netif_cfg.cellular_cfg);
    }
    netif_info->rssi = usb_ecm_cellular_info.rssi;
    strcpy(netif_info->fw_version, usb_ecm_cellular_info.version);
    memcpy(&netif_info->cellular_info, &usb_ecm_cellular_info, sizeof(netif_info->cellular_info));
    memcpy(&netif_info->cellular_cfg, &usb_ecm_netif_cfg.cellular_cfg, sizeof(netif_info->cellular_cfg));
#endif
    return 0;
}

netif_state_t usb_ecm_netif_state(void)
{
    struct netif *ue = NULL;

    ue = netif_get_by_index(ecm_netif.num + 1);
    if (ue == NULL || ue != &ecm_netif) return NETIF_STATE_DEINIT;
    else if (!netif_is_link_up(&ecm_netif) || !netif_is_up(&ecm_netif)) return NETIF_STATE_DOWN;
    else return NETIF_STATE_UP;
}

struct netif *usb_ecm_netif_ptr(void)
{
    return &ecm_netif;
}

int usb_ecm_netif_ctrl(const char *if_name, netif_cmd_t cmd, void *param)
{
    int ret = AICAM_ERROR;
    netif_state_t if_state = NETIF_STATE_DEINIT;
    if (usb_ecm_netif_mutex == NULL) usb_ecm_netif_mutex = osMutexNew(NULL);
    if (usb_ecm_netif_mutex == NULL) return AICAM_ERROR_NO_MEMORY;

    osMutexAcquire(usb_ecm_netif_mutex, osWaitForever);
    switch (cmd) {
        case NETIF_CMD_CFG:
            ret = usb_ecm_netif_config((netif_config_t *)param);
            break;
        case NETIF_CMD_INIT:
            ret = usb_ecm_netif_init();
            break;
        case NETIF_CMD_UP:
            ret = usb_ecm_netif_up();
            break;
        case NETIF_CMD_INFO:
            ret = usb_ecm_netif_info((netif_info_t *)param);
            break;
        case NETIF_CMD_STATE:
            if (param == NULL) ret = AICAM_ERROR_INVALID_PARAM;
            else {
                *(netif_state_t *)param = usb_ecm_netif_state();
                ret = AICAM_OK;
            }
            break;
        case NETIF_CMD_DOWN:
            ret = usb_ecm_netif_down();
            break;
        case NETIF_CMD_UNINIT:
            usb_ecm_netif_deinit();
            ret = AICAM_OK;
            break;
        case NETIF_CMD_CFG_EX:
            if_state = usb_ecm_netif_state();
            if (if_state == NETIF_STATE_UP) {
                ret = usb_ecm_netif_down();
                if (ret) break;
            }
            ret = usb_ecm_netif_config((netif_config_t *)param);
            if (ret) break;
            if (if_state == NETIF_STATE_UP) ret = usb_ecm_netif_up();
            break;
        default:
            ret = AICAM_ERROR_INVALID_PARAM;
            break;
    }
    osMutexRelease(usb_ecm_netif_mutex);
    return ret;
}


