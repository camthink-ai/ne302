/**
 * @brief Minimal CMSIS-RTOS2 stubs for FSBL (no RTOS).
 *        STM32_Camera_Middleware registers osDelay for sensor BSP; map ticks to HAL delay.
 */
#include "cmsis_os2.h"
#include "stm32n6xx_hal.h"

osStatus_t osDelay(uint32_t ticks)
{
  /* Same convention as CMSIS-RTOS2 on STM32: tick period is 1 ms */
  HAL_Delay(ticks);
  return osOK;
}
