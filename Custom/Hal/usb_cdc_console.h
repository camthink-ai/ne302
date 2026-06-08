#ifndef USB_CDC_CONSOLE_H
#define USB_CDC_CONSOLE_H

#include <stdint.h>
#include "stm32n6xx_hal.h"

int usb_cdc_console_init(void);
int usb_cdc_console_activate(void);
int usb_cdc_console_is_active(void);
/* Console I/O routed to USB (link up and not fallen back to UART) */
int usb_cdc_console_is_host_ready(void);
/* USB link up: configured and CDC instance active */
int usb_cdc_console_is_link_up(void);
int usb_cdc_console_write(const uint8_t *data, uint32_t len);
PCD_HandleTypeDef *usb_cdc_console_get_pcd(void);

#endif
