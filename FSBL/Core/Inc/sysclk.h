/**
 * @file sysclk.h
 * @brief FSBL 系统时钟配置（由 SysClk_Config(profile) 选择方案）。
 *
 * 参考晶振/内部振荡器：HSE = 48 MHz，HSI = 64 MHz（与 stm32n6xx_hal_conf.h 中 HSE_VALUE / HSI_VALUE 一致）。
 *
 * 下列频率为按 PLL 分频公式推算的标称值；NPU / NPURAM 等请以 HAL_RCC_GetNpuClockFreq() 等运行时读数为准。
 *
 * --- SYSCLK_PROFILE_HSE_400MHZ（HSE 作 PLL 参考）---
 *   PLL1: 48M/1*25/(3*1) = 400 MHz -> IC1(CPU) = 400 MHz；IC2/IC6/IC11 均选 PLL1、分频 1 -> SYS 相关域 400 MHz
 *   HCLK: AHB 预分频 /2 -> 200 MHz；APB1/2/4/5 均为 /1 -> 与 HCLK 同频（200 MHz）
 *   PLL2: 512 MHz（外设 PLL）；PLL3: 150 MHz；PLL4: 512 MHz
 *   末尾使能 LSE CSS
 *
 * --- SYSCLK_PROFILE_HSE_200MHZ ---
 *   PLL1: 48M/1*25/(3*2) = 200 MHz -> CPU / IC2/IC6/IC11 均为 200 MHz（分频均为 1）
 *   HCLK: /1 -> 200 MHz；APB 均为 /1 -> 200 MHz
 *   PLL2/3/4 与 400MHz 方案相同（512 / 150 / 512 MHz）
 *   末尾使能 LSE CSS
 *
 * --- SYSCLK_PROFILE_HSI_800MHZ（HSI 作 PLL 参考，无 HSE 锁相）---
 *   PLL1: 64M/2*25/(1*1) = 800 MHz -> IC1(CPU) = 800 MHz
 *   PLL2: 64M/8*125/(1*1) = 1000 MHz；PLL3: 64M/8*225/(1*2) = 900 MHz
 *   PLL4: 64M/4*64/(2*1) = 512 MHz
 *   IC2: PLL1/2 = 400 MHz；IC6: PLL2；IC11: PLL3
 *   HCLK: /2 -> 400 MHz；APB /1 -> 400 MHz
 *   不调用 HAL_RCCEx_EnableLSECSS()（与原 main.c 行为一致）
 *
 * --- SYSCLK_PROFILE_HSE_800MHZ（默认，与 Makefile -DCPU_CLK_USE_800MHZ 对应）---
 *   PLL1: 48M/2*100/(3*1) = 800 MHz -> IC1(CPU) = 800 MHz
 *   PLL2: 48M/6*125/(1*1) = 1000 MHz；PLL3: 48M/4*75/(1*1) = 900 MHz
 *   PLL4: 48M/3*64/(2*1) = 512 MHz
 *   IC2: PLL1/2 = 400 MHz；IC6: PLL2；IC11: PLL3
 *   HCLK: /2 -> 400 MHz；APB /1 -> 400 MHz
 *   末尾使能 LSE CSS
 */

#ifndef SYSCLK_H
#define SYSCLK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  SYSCLK_PROFILE_MIN = 0,
  SYSCLK_PROFILE_HSE_200MHZ = 1,
  SYSCLK_PROFILE_HSE_400MHZ,
  SYSCLK_PROFILE_HSI_800MHZ,
  SYSCLK_PROFILE_HSE_800MHZ,
  SYSCLK_PROFILE_MAX,
} SysClk_Profile_t;

void SysClk_Config(SysClk_Profile_t profile);
void SysClk_PeriphCommonConfig(void);

/* 与历史编译宏对应，便于 Makefile 仍使用 -DCPU_CLK_USE_* 时一键选用 */
#if defined(CPU_CLK_USE_400MHZ)
#define SYSCLK_PROFILE_DEFAULT SYSCLK_PROFILE_HSE_400MHZ
#elif defined(CPU_CLK_USE_200MHZ)
#define SYSCLK_PROFILE_DEFAULT SYSCLK_PROFILE_HSE_200MHZ
#elif defined(CPU_CLK_USE_HSI_800MHZ)
#define SYSCLK_PROFILE_DEFAULT SYSCLK_PROFILE_HSI_800MHZ
#else
#define SYSCLK_PROFILE_DEFAULT SYSCLK_PROFILE_HSE_800MHZ
#endif

#ifdef __cplusplus
}
#endif

#endif /* SYSCLK_H */
