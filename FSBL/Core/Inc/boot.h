/**
  ******************************************************************************
  * @file    stm32_boot_lrun.h
  * @author  MCD Application Team
  * @brief   Header for stm32_boot_lrun.c file.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2023 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __BOOT_H__
#define __BOOT_H__

#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
typedef enum {
     BOOT_OK,
     BOOT_ERROR_UNSUPPORTED_MEMORY, /* !< unsupported memory type         */
     BOOT_ERROR_NOBASEADDRESS,      /* !< not base address for the memory */
     BOOT_ERROR_MAPPEDMODEFAIL,     /* !< */
     BOOT_ERROR_COPY,
}BOOTStatus_TypeDef;

/**
  * @}
  */


/* Private function prototypes -----------------------------------------------*/
/**
  *  @defgroup BOOT_LRUN_Exported_Functions Boot LRUN exported functions
  * @{
  */

/**
 * @brief This function copies the application from the flash to the SRAM/PSRAM
 * @return @ref BOOTStatus_TypeDef
 **/
BOOTStatus_TypeDef BOOT_Copy_Application(void);


/**
 * @brief This function jumps to the application, the operation consists in mapping 
 *        the memories, loading the code and jumping in the application. 
 *
 * @return @ref BOOTStatus_TypeDef
 **/
 BOOTStatus_TypeDef BOOT_Jump_Application(void);

/**
  * @}
  */

/**
  * @}
  */

#ifdef __cplusplus
}
#endif

#endif /* __STM32_BOOT_LRUN_H__ */