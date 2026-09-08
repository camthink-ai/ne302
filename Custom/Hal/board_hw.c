/**
 * @file board_hw.c
 * @brief NE302 board hardware description.
 *
 * Unlike NE301, the NE302 board has no hardware-revision strap pin: there is
 * no way to detect the board revision from firmware, and none is needed —
 * only one revision exists. The HaLow WAKE pin is fixed (PA13, see main.h),
 * so this file reduces to fixed accessors kept for call-site compatibility
 * with the shared NE301 code base.
 */

#include "board_hw.h"
#include "main.h"

void board_hw_init(void)
{
    /* Nothing to detect on NE302: no revision strap, fixed pin map. */
}

const char *board_hw_version_str(void)
{
    /* NE302 has no board-revision detection; report the fixed HW version.
     * Used for device_info.hardware_version in config/factory/storage. */
    return "V1.0.0";
}

GPIO_TypeDef *board_hw_halow_wake_port(void)
{
    return MM_HALOW_WAKE_GPIO_Port;
}

uint16_t board_hw_halow_wake_pin(void)
{
    return MM_HALOW_WAKE_Pin;
}
