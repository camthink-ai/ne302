/**
 * @file    usb_cherry.h
 * @brief   CherryUSB bring-up seam for NE302: host on USB2_OTG_HS, device
 *          on USB1_OTG_HS.
 *
 * Replaces the former usb_host_ecm/usbx wrappers as the single app-side
 * entry to the USB stacks. Class-level consumers (RNDIS netif, UVC, ...)
 * sit on top of this.
 */
#ifndef USB_CHERRY_H
#define USB_CHERRY_H

#include <stdint.h>
#include "usbh_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One-time CherryUSB osal bootstrap (byte pool + reaper thread).
 * Shared by the host stack and any device class; safe to call repeatedly.
 * @return 0 on success
 */
int usb_cherry_osal_ensure(void);

/**
 * @brief Power the USB rail and start the CherryUSB host stack on USB2.
 * @param event_cb optional bus/device event callback (may be NULL; events
 *                 are logged either way)
 * @return 0 on success
 */
int usb_cherry_host_init(usbh_event_handler_t event_cb);

/**
 * @brief Stop the host stack (real DWC2 deinit: GINT off, FIFOs flushed,
 *        channels halted) and release the USB rail.
 * @return 0 on success
 */
int usb_cherry_host_deinit(void);

/** @brief Non-zero while the host stack is running. */
int usb_cherry_host_is_inited(void);

/* USB device (CDC-ACM console [+ RNDIS NIC]) on USB1_OTG_HS (J21 Type-C) —
 * Custom/Hal/usb/usb_device_composite.c */

/**
 * @brief Start the USB device with a selectable function set.
 * @param with_rndis 1: RNDIS NIC + CDC console composite; 0: CDC console
 *                   only (no lwip netif / DHCP server / tx thread)
 * @return 0 on success
 */
int usb_cherry_device_init_ex(int with_rndis);

/** @brief Full composite (RNDIS + CDC console); CLI / "ud" netif entry. */
int usb_cherry_device_init(void);

/**
 * @brief Switch the RNDIS function on/off at runtime.
 * Re-enumerates the bus when the function set actually changes.
 * @return 0 on success
 */
int usb_cherry_device_set_rndis(int enable);

/** @brief Non-zero while the RNDIS function is in the active config. */
int usb_cherry_device_rndis_is_active(void);

/**
 * @brief Boot-time start: CDC console on every boot, RNDIS added only on
 * the configuration-mode path (sleep fast-boot wakes stay CDC-only).
 * Offloads the init to a low-priority one-shot thread (non-blocking).
 * @return 0 if the boot task was scheduled
 */
int usb_cherry_device_boot_start(void);

int usb_cherry_device_deinit(void);
int usb_cherry_device_is_inited(void);
/** One-line runtime diagnostics (rx/tx counters, netif ip) for the CLI. */
void usb_cherry_device_diag(char *buf, int buflen);

struct netif;
/** netif_manager ctrl for the PC-facing "ud" netif (INFO/STATE/INIT/UNINIT). */
int usb_dev_netif_ctrl(const char *if_name, int cmd, void *param);

#ifdef __cplusplus
}
#endif

#endif /* USB_CHERRY_H */
