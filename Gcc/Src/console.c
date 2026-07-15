/**
  ******************************************************************************
  * @file    console.c
  * @author  MDG Application Team
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2023 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the ST_LICENSE.md file
  * in the root directory of this software component.
  * If no ST_LICENSE.md file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

#include "main.h"
#ifdef USE_USB_CDC_CONSOLE
#include "usb_cdc_console.h"
#endif

#include <errno.h>
#include <unistd.h>

#ifdef  STM32N6_DK_BOARD
extern UART_HandleTypeDef huart1;
#else
extern UART_HandleTypeDef huart2;
#endif

int _write(int file, char *ptr, int len)
{
  HAL_StatusTypeDef status;
#ifdef USE_USB_CDC_CONSOLE
  int written;
#endif

  if ((file != STDOUT_FILENO) && (file != STDERR_FILENO)) {
      errno = EBADF;
      return -1;
  }

#ifdef USE_USB_CDC_CONSOLE
  if (usb_cdc_console_is_active() && usb_cdc_console_is_host_ready()) {
    written = usb_cdc_console_write((const uint8_t *)ptr, (uint32_t)len);
    if (written > 0) {
      return written;
    }
  }
#endif

#ifdef  STM32N6_DK_BOARD
  status = HAL_UART_Transmit(&huart1, (uint8_t*)ptr, len, 200);
#else
  status = HAL_UART_Transmit(&huart2, (uint8_t*)ptr, len, 200);
#endif
  return (status == HAL_OK ? len : 0);
}
