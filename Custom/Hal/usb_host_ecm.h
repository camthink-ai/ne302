#ifndef USB_HOST_ECM_H
#define USB_HOST_ECM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "usbx_host.h"
#include "ux_host_class_cdc_ecm.h"

typedef enum {
    USB_HOST_ECM_EVENT_ACTIVATE = 0,
    USB_HOST_ECM_EVENT_DEACTIVATE = 1,
    USB_HOST_ECM_EVENT_UP = 2,
    USB_HOST_ECM_EVENT_DOWN = 3,
    USB_HOST_ECM_EVENT_DATA = 4,
    USB_HOST_ECM_EVENT_ERROR = 5,
} usb_host_ecm_event_type_t;

typedef void (*usb_host_ecm_event_callback_t)(usb_host_ecm_event_type_t event, void *arg);

int usb_host_ecm_init(usb_host_ecm_event_callback_t event_callback);
int usb_host_ecm_send_raw_data(NX_PACKET *packet);
/**
 * @brief Wait for the USBX CDC-ECM bulk OUT transfer completion.
 *
 * Call this right after a successful usb_host_ecm_send_raw_data().
 *
 * @param timeout_ms Timeout in milliseconds, or USB_HOST_ECM_WAIT_FOREVER.
 * @return 0 on success; otherwise a USBX/ThreadX error code (non-zero).
 */
#define USB_HOST_ECM_WAIT_FOREVER (0xFFFFFFFFu)
int usb_host_ecm_wait_tx_done(uint32_t timeout_ms);
void usb_host_ecm_deinit(void);

#ifdef __cplusplus
}
#endif

#endif  
