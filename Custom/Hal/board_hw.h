/**
 * @file board_hw.h
 * @brief NE302 board hardware description.
 *
 * NE302 has no hardware-revision strap: firmware cannot detect the board
 * revision and only one revision exists. The HaLow WAKE pin is fixed
 * (PA13, from main.h). These accessors exist so shared NE301 call sites
 * (wake-pin setup, hardware-version reporting) compile unchanged.
 */
#ifndef _BOARD_HW_H_
#define _BOARD_HW_H_

#include "stm32n6xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** No-op on NE302 (kept for driver_core_init() compatibility). */
void board_hw_init(void);

/** Fixed "V1.0.0" — NE302 has no board-revision detection. */
const char *board_hw_version_str(void);

/** Fixed HaLow WAKE pin (PA13 on NE302). */
GPIO_TypeDef *board_hw_halow_wake_port(void);
uint16_t board_hw_halow_wake_pin(void);

#ifdef __cplusplus
}
#endif

#endif /* _BOARD_HW_H_ */
