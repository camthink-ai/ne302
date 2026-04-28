/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usb_otg.c
  * @brief   This file provides code for the configuration
  *          of the USB_OTG instances.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include <string.h>
#include "usb_otg.h"
// #include "storage.h"
#include "ux_stm32_config.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */
#ifndef ISP_MW_TUNING_TOOL_SUPPORT
#ifdef UX_HCD_ECM_USE_USB_OTG_HS1
HCD_HandleTypeDef hhcd_USB_OTG_HS1;
#else
PCD_HandleTypeDef hpcd_USB_OTG_HS1;
#endif
#endif
HCD_HandleTypeDef hhcd_USB_OTG_HS2;

/* USB1_OTG_HS init function */

#ifndef ISP_MW_TUNING_TOOL_SUPPORT

#ifdef UX_HCD_ECM_USE_USB_OTG_HS1
void MX_USB1_OTG_HS_HCD_Init(void)
{
  memset(&hhcd_USB_OTG_HS1, 0x0, sizeof(HCD_HandleTypeDef));

  /* USER CODE END USB1_OTG_HS_Init 1 */
  hhcd_USB_OTG_HS1.Instance = USB1_OTG_HS;
  hhcd_USB_OTG_HS1.Init.Host_channels = UX_HCD_STM32_MAX_NB_CHANNELS;
  hhcd_USB_OTG_HS1.Init.speed = HCD_SPEED_HIGH;
  hhcd_USB_OTG_HS1.Init.dma_enable = ENABLE;
  hhcd_USB_OTG_HS1.Init.phy_itface = USB_OTG_HS_EMBEDDED_PHY;
  hhcd_USB_OTG_HS1.Init.Sof_enable = DISABLE;
  hhcd_USB_OTG_HS1.Init.vbus_sensing_enable = DISABLE;
  hhcd_USB_OTG_HS1.Init.use_external_vbus = ENABLE;

  if (HAL_HCD_Init(&hhcd_USB_OTG_HS1) != HAL_OK)
  {
    Error_Handler();
  }
}
#else
void MX_USB1_OTG_HS_PCD_Init(void)
{

  /* USER CODE BEGIN USB1_OTG_HS_Init 0 */

  /* USER CODE END USB1_OTG_HS_Init 0 */

  /* USER CODE BEGIN USB1_OTG_HS_Init 1 */

  /* USER CODE END USB1_OTG_HS_Init 1 */
  hpcd_USB_OTG_HS1.Instance = USB1_OTG_HS;
  hpcd_USB_OTG_HS1.Init.dev_endpoints = 9;
  hpcd_USB_OTG_HS1.Init.speed = HCD_SPEED_HIGH;
  hpcd_USB_OTG_HS1.Init.phy_itface = USB_OTG_HS_EMBEDDED_PHY;
  hpcd_USB_OTG_HS1.Init.Sof_enable = DISABLE;
  hpcd_USB_OTG_HS1.Init.low_power_enable = DISABLE;
  hpcd_USB_OTG_HS1.Init.lpm_enable = DISABLE;
  hpcd_USB_OTG_HS1.Init.use_dedicated_ep1 = DISABLE;
  hpcd_USB_OTG_HS1.Init.vbus_sensing_enable = DISABLE;
  hpcd_USB_OTG_HS1.Init.dma_enable = ENABLE;
  if (HAL_PCD_Init(&hpcd_USB_OTG_HS1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USB1_OTG_HS_Init 2 */

  /* USER CODE END USB1_OTG_HS_Init 2 */

}
#endif
#endif

/* USB2_OTG_HS init function */

void MX_USB2_OTG_HS_HCD_Init(void)
{
  // char speed_mode[16] = {0};
  /* USER CODE BEGIN USB2_OTG_HS_Init 0 */

  /* USER CODE END USB2_OTG_HS_Init 0 */

  /* USER CODE BEGIN USB2_OTG_HS_Init 1 */
  memset(&hhcd_USB_OTG_HS2, 0x0, sizeof(HCD_HandleTypeDef));
  /* USER CODE END USB2_OTG_HS_Init 1 */
  hhcd_USB_OTG_HS2.Instance = USB2_OTG_HS;
  hhcd_USB_OTG_HS2.Init.Host_channels = UX_HCD_STM32_MAX_NB_CHANNELS;
  hhcd_USB_OTG_HS2.Init.speed = HCD_SPEED_HIGH;
  // if (storage_nvs_read(NVS_FACTORY, "usb2_speed", speed_mode, sizeof(speed_mode)) > 0) {
  //   if (strcmp(speed_mode, "full") == 0) {
  //     hhcd_USB_OTG_HS2.Init.speed = HCD_SPEED_FULL;
  //   } else if (strcmp(speed_mode, "low") == 0) {
  //     hhcd_USB_OTG_HS2.Init.speed = HCD_SPEED_LOW;
  //   }
  // }
  hhcd_USB_OTG_HS2.Init.dma_enable = ENABLE;
  hhcd_USB_OTG_HS2.Init.phy_itface = USB_OTG_HS_EMBEDDED_PHY;
  hhcd_USB_OTG_HS2.Init.Sof_enable = DISABLE;
  hhcd_USB_OTG_HS2.Init.vbus_sensing_enable = DISABLE;
  hhcd_USB_OTG_HS2.Init.use_external_vbus = ENABLE;
  if (HAL_HCD_Init(&hhcd_USB_OTG_HS2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USB2_OTG_HS_Init 2 */

  /* USER CODE END USB2_OTG_HS_Init 2 */

}

#ifndef ISP_MW_TUNING_TOOL_SUPPORT
void HAL_PCD_MspInit(PCD_HandleTypeDef* pcdHandle)
{

  // RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  if(pcdHandle->Instance==USB1_OTG_HS)
  {
  /* USER CODE BEGIN USB1_OTG_HS_MspInit 0 */

  /* USER CODE END USB1_OTG_HS_MspInit 0 */
    __HAL_RCC_PWR_CLK_ENABLE();
    /* Enable the VDD33USB independent USB 33 voltage monitor */
    HAL_PWREx_EnableVddUSBVMEN();

    /* Wait until VDD33USB is ready */
    while (__HAL_PWR_GET_FLAG(PWR_FLAG_USB33RDY) == 0U);

    /* Enable VDDUSB supply */
    HAL_PWREx_EnableVddUSB();

    /* Enable USB1 OTG clock */
    __HAL_RCC_USB1_OTG_HS_CLK_ENABLE();

    /* Set FSEL to 24 Mhz */
    USB1_HS_PHYC->USBPHYC_CR &= ~(0x7U << 0x4U);
    USB1_HS_PHYC->USBPHYC_CR |= (0x2U << 0x4U);

    /* Enable USB1 OTG PHY clock */
    __HAL_RCC_USB1_OTG_HS_PHY_CLK_ENABLE();

    HAL_NVIC_SetPriority(USB1_OTG_HS_IRQn, 6U, 0U);

    /* Enable USB OTG interrupt */
    HAL_NVIC_EnableIRQ(USB1_OTG_HS_IRQn);
  }
}
#endif

void HAL_HCD_MspInit(HCD_HandleTypeDef* hcdHandle)
{
  // uint32_t reg_value = 0;
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  if (hcdHandle->Instance == USB1_OTG_HS) {
    /* USER CODE BEGIN USB_OTG_HS_MspInit 0 */

    /* USER CODE END USB_OTG_HS_MspInit 0 */
    /* Enable VDDUSB */
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWREx_EnableVddUSBVMEN();
    while (__HAL_PWR_GET_FLAG(PWR_FLAG_USB33RDY) == 0U);
    HAL_PWREx_EnableVddUSB();

    /** Initializes the peripherals clock
    */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_USBOTGHS1;
   PeriphClkInitStruct.UsbOtgHs1ClockSelection = RCC_USBPHY1CLKSOURCE_HSE_DIRECT;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      /* Initialization Error */
      Error_Handler();
    }

    /** Set USB OTG HS PHY1 Reference Clock Source */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_USBPHY1;
    PeriphClkInitStruct.UsbPhy1ClockSelection = RCC_USBPHY1CLKSOURCE_HSE_DIRECT;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      /* Initialization Error */
      Error_Handler();
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();

    LL_AHB5_GRP1_ForceReset(0x00800000);
    __HAL_RCC_USB1_OTG_HS_FORCE_RESET();
    __HAL_RCC_USB1_OTG_HS_PHY_FORCE_RESET();

    LL_RCC_HSE_SelectHSEDiv2AsDiv2Clock();
    LL_AHB5_GRP1_ReleaseReset(0x00800000);

    /* Peripheral clock enable */
    __HAL_RCC_USB1_OTG_HS_CLK_ENABLE();

    /* Required few clock cycles before accessing USB PHY Controller Registers */
    HAL_Delay(1);

    USB1_HS_PHYC->USBPHYC_CR &= ~(0x7 << 0x4);

    USB1_HS_PHYC->USBPHYC_CR |= (0x1 << 16) |
                                (0x2 << 4)  |
                                (0x1 << 2)  |
                                 0x1U;

    __HAL_RCC_USB1_OTG_HS_PHY_RELEASE_RESET();

    /* Required few clock cycles before Releasing Reset */
    HAL_Delay(1);

    __HAL_RCC_USB1_OTG_HS_RELEASE_RESET();

    /* Peripheral PHY clock enable */
    __HAL_RCC_USB1_OTG_HS_PHY_CLK_ENABLE();

    /* USB_OTG_HS interrupt Init */
    HAL_NVIC_SetPriority(USB1_OTG_HS_IRQn, 7, 0);
    HAL_NVIC_EnableIRQ(USB1_OTG_HS_IRQn);

    /* USER CODE BEGIN USB_OTG_HS_MspInit 1 */

    /* USER CODE END USB_OTG_HS_MspInit 1 */
  } else if(hcdHandle->Instance == USB2_OTG_HS) {
  /* USER CODE BEGIN USB2_OTG_HS_MspInit 0 */

  /* USER CODE END USB2_OTG_HS_MspInit 0 */

    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWREx_EnableVddUSBVMEN();
    while (__HAL_PWR_GET_FLAG(PWR_FLAG_USB33RDY) == 0U);
    HAL_PWREx_EnableVddUSB();

    /** Initializes the peripherals clock
    */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_USBOTGHS2;
   PeriphClkInitStruct.UsbOtgHs2ClockSelection = RCC_USBPHY2CLKSOURCE_HSE_DIRECT;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      /* Initialization Error */
      Error_Handler();
    }

    /** Set USB OTG HS PHY2 Reference Clock Source */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_USBPHY2;
    PeriphClkInitStruct.UsbPhy2ClockSelection = RCC_USBPHY2CLKSOURCE_HSE_DIRECT;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      /* Initialization Error */
      Error_Handler();
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();

    LL_AHB5_GRP1_ForceReset(0x01000000);
    __HAL_RCC_USB2_OTG_HS_FORCE_RESET();
    __HAL_RCC_USB2_OTG_HS_PHY_FORCE_RESET();

    LL_RCC_HSE_SelectHSEDiv2AsDiv2Clock();
    LL_AHB5_GRP1_ReleaseReset(0x01000000);

    /* Peripheral clock enable */
    __HAL_RCC_USB2_OTG_HS_CLK_ENABLE();

    /* Required few clock cycles before accessing USB PHY Controller Registers */
    HAL_Delay(1);

    USB2_HS_PHYC->USBPHYC_CR &= ~(0x7 << 0x4);

    USB2_HS_PHYC->USBPHYC_CR |= (0x1 << 16) |
                                (0x2 << 4)  |
                                (0x1 << 2)  |
                                 0x1U;

    // if (storage_nvs_read(NVS_FACTORY, "usb2phy_cr1", &reg_value, sizeof(reg_value)) == sizeof(reg_value)) {
    //   USB2_HS_PHYC->USBPHYC_TRIM1CR = reg_value;
    //   printf("USB2 PHY TRIM1CR CONFIG from NVS: 0x%08lX\r\n", reg_value);
    // }
    // if (storage_nvs_read(NVS_FACTORY, "usb2phy_cr2", &reg_value, sizeof(reg_value)) == sizeof(reg_value)) {
    //   USB2_HS_PHYC->USBPHYC_TRIM2CR = reg_value;
    //   printf("USB2 PHY TRIM2CR CONFIG from NVS: 0x%08lX\r\n", reg_value);
    // }
    // USB2_HS_PHYC->USBPHYC_TRIM1CR &= ~(0x3 << 29);
    // USB2_HS_PHYC->USBPHYC_TRIM1CR &= ~(0x3 << 27);
    // USB2_HS_PHYC->USBPHYC_TRIM1CR &= ~(0x7 << 9);
    // USB2_HS_PHYC->USBPHYC_TRIM1CR |= (0x7 << 6);
    // USB2_HS_PHYC->USBPHYC_TRIM2CR = 0x3;

    __HAL_RCC_USB2_OTG_HS_PHY_RELEASE_RESET();

    /* Required few clock cycles before Releasing Reset */
    HAL_Delay(1);

    __HAL_RCC_USB2_OTG_HS_RELEASE_RESET();

    /* Peripheral PHY clock enable */
    __HAL_RCC_USB2_OTG_HS_PHY_CLK_ENABLE();

    HAL_NVIC_SetPriority(USB2_OTG_HS_IRQn, 7, 0);
    HAL_NVIC_EnableIRQ(USB2_OTG_HS_IRQn);

  /* USER CODE BEGIN USB2_OTG_HS_MspInit 1 */

  /* USER CODE END USB2_OTG_HS_MspInit 1 */
  }
}

#ifndef ISP_MW_TUNING_TOOL_SUPPORT
void HAL_PCD_MspDeInit(PCD_HandleTypeDef* pcdHandle)
{

  if(pcdHandle->Instance==USB1_OTG_HS)
  {
  /* USER CODE BEGIN USB1_OTG_HS_MspDeInit 0 */

  /* USER CODE END USB1_OTG_HS_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USB1_OTG_HS_CLK_DISABLE();
    __HAL_RCC_USB1_OTG_HS_PHY_CLK_DISABLE();

    /* Disable VDDUSB */
    HAL_PWREx_DisableVddUSB();

    /* USB1_OTG_HS interrupt Deinit */
    HAL_NVIC_DisableIRQ(USB1_OTG_HS_IRQn);
  /* USER CODE BEGIN USB1_OTG_HS_MspDeInit 1 */

  /* USER CODE END USB1_OTG_HS_MspDeInit 1 */
  }
}
#endif

void HAL_HCD_MspDeInit(HCD_HandleTypeDef* hcdHandle)
{

  if(hcdHandle->Instance==USB2_OTG_HS)
  {
  /* USER CODE BEGIN USB2_OTG_HS_MspDeInit 0 */

  /* USER CODE END USB2_OTG_HS_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USB2_OTG_HS_CLK_DISABLE();
    __HAL_RCC_USB2_OTG_HS_PHY_CLK_DISABLE();

    /* Disable VDDUSB */
    HAL_PWREx_DisableVddUSB();

    /* USB2_OTG_HS interrupt Deinit */
    HAL_NVIC_DisableIRQ(USB2_OTG_HS_IRQn);
  /* USER CODE BEGIN USB2_OTG_HS_MspDeInit 1 */

  /* USER CODE END USB2_OTG_HS_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
