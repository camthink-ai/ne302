/**
  ******************************************************************************
  * @file    stm32_boot_lrun.c
  * @author  MCD Application Team
  * @brief   this file manages the boot in the mode load and run.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2022 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "boot.h"
#include "mem_map.h"
#include "stm32n6xx_hal.h"
#include "upgrade_manager.h"
#include "xspim.h"
#include "fsbl_version.h"

/* Private typedefs ----------------------------------------------------------*/
/* Private defines -----------------------------------------------------------*/
#define HEADER_V2_1_IMG_SIZE_OFFSET 76
#define HEADER_V2_3_IMG_SIZE_OFFSET 108
#define HEADER_V2_1_SIZE 1024
#define HEADER_V2_3_SIZE 1024

/* offset of the vector table from the start of the image. Should be set in extmem_conf.h if needed  */
#define EXTMEM_HEADER_OFFSET 0x400

/* Private macros ------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
BOOTStatus_TypeDef CopyApplication(void);
BOOTStatus_TypeDef JumpToApplication(void);

static int boot_flash_write(uint32_t offset, void *data, size_t size)
{
    XSPI_NOR_DisableMemoryMappedMode();
    if (XSPI_NOR_Write((uint8_t *)data, offset, size) != 0) {
        XSPI_NOR_EnableMemoryMappedMode();
        return -1; 
    }
    XSPI_NOR_EnableMemoryMappedMode();
    return 0;
}

static int boot_flash_read(uint32_t offset, void *data, size_t size)
{
    memcpy(data, (const void *)(FLASH_BASE + offset), size);
    XSPI_NOR_DisableMemoryMappedMode();
    XSPI_NOR_EnableMemoryMappedMode();
    return 0;
}

static int boot_flash_erase(uint32_t offset, size_t num_blk)
{
    if (offset % FLASH_BLK_SIZE != 0) {
        return -1;
    }
    XSPI_NOR_DisableMemoryMappedMode();
    for (size_t i = 0; i < num_blk; i++) {
        if (XSPI_NOR_Erase4K(offset + i * FLASH_BLK_SIZE) != 0) {
            XSPI_NOR_EnableMemoryMappedMode();
            return -1;
        }
    }
    XSPI_NOR_EnableMemoryMappedMode();
    return 0;
}

static uint32_t BOOT_GetApplicationSize(uint32_t img_addr)
{
  uint32_t img_size;

  img_size = (*(uint32_t *)(img_addr + HEADER_V2_3_IMG_SIZE_OFFSET));
  img_size += HEADER_V2_3_SIZE;

  return img_size;
}

static uint32_t BOOT_GetApplicationVectorTable(void)
{
  uint32_t vector_table;
  vector_table = SRAM_APP_BASE;
  vector_table += EXTMEM_HEADER_OFFSET;
  return vector_table;
}

/**
  * @brief  This function copy the data from source to destination
  * @return @ref BOOTStatus_TypeDef
  */
BOOTStatus_TypeDef BOOT_Copy_Application(void)
{
  BOOTStatus_TypeDef retr = BOOT_OK;
  uint8_t *source;
  uint8_t *destination;
  uint32_t img_size;
  // uint32_t start_tick, end_tick;

  /* Initialize the system state */
  init_system_state(boot_flash_read, boot_flash_write, boot_flash_erase);
  /* this case correspond to copy the SW from external memory into internal memory */
  destination = (uint8_t *)SRAM_APP_BASE;

  check_and_select_boot_slot(FIRMWARE_APP);
    /* manage the copy in mapped mode */
  source = FLASH_BASE + (uint8_t*)get_active_partition(FIRMWARE_APP) + OTA_HEADER_SIZE;
  img_size = BOOT_GetApplicationSize((uint32_t) source);
  // printf("Application size: %ld flash address: %p sram address: %p\r\n", img_size, source, destination);
  /* copy form source to destination in mapped mode */
  // start_tick = HAL_GetTick();
  memcpy(destination, source, img_size);
  // end_tick = HAL_GetTick();
  // printf("Copy time: %ld ms\r\n", end_tick - start_tick);

  return retr;
}

/**
  * @brief  This function jumps to the application through its vector table
  * @return @ref BOOTStatus_TypeDef
  */
BOOTStatus_TypeDef BOOT_Jump_Application(void)
{
  uint32_t primask_bit;
  typedef  void (*pFunction)(void);
  static pFunction JumpToApp;
  uint32_t Application_vector;
  /* Suspend SysTick */
  HAL_SuspendTick();

#if defined(__ICACHE_PRESENT) && (__ICACHE_PRESENT == 1U)
  /* if I-Cache is enabled, disable I-Cache-----------------------------------*/
  if (SCB->CCR & SCB_CCR_IC_Msk)
  {
    SCB_DisableICache();
  }
#endif /* defined(ICACHE_PRESENT) && (ICACHE_PRESENT == 1U) */

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
  /* if D-Cache is enabled, disable D-Cache-----------------------------------*/
  if (SCB->CCR & SCB_CCR_DC_Msk)
  {
    SCB_DisableDCache();
  }
#endif /* defined(DCACHE_PRESENT) && (DCACHE_PRESENT == 1U) */

  /* Initialize user application's Stack Pointer & Jump to user application  */
  primask_bit = __get_PRIMASK();
  __disable_irq();

  Application_vector = BOOT_GetApplicationVectorTable();

  SCB->VTOR = (uint32_t)Application_vector;
  JumpToApp = (pFunction) (*(__IO uint32_t *)(Application_vector + 4));

#if ((defined (__ARM_ARCH_8M_MAIN__ ) && (__ARM_ARCH_8M_MAIN__ == 1)) || \
     (defined (__ARM_ARCH_8_1M_MAIN__ ) && (__ARM_ARCH_8_1M_MAIN__ == 1)) || \
     (defined (__ARM_ARCH_8M_BASE__ ) && (__ARM_ARCH_8M_BASE__ == 1))    )
  /* on ARM v8m, set MSPLIM before setting MSP to avoid unwanted stack overflow faults */
  __set_MSPLIM(0x00000000);
#endif  /* __ARM_ARCH_8M_MAIN__ or __ARM_ARCH_8M_BASE__ */

  __set_MSP(*(__IO uint32_t*)Application_vector);

  /* Re-enable the interrupts */
  __set_PRIMASK(primask_bit);

  JumpToApp();
  return BOOT_OK;
}


/**
  * @}
  */
