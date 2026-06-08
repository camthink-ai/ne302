#include "usbx_platform.h"
#include "common_utils.h"

#include <stdint.h>

#define USBX_PLATFORM_MEM_SIZE          (128U * 1024U)
#define USBX_PLATFORM_MEM_UNCACHED_SIZE (24U * 1024U)

static uint8_t g_usbx_mem_pool[USBX_PLATFORM_MEM_SIZE] ALIGN_32 IN_PSRAM;
static uint8_t g_usbx_mem_pool_uncached[USBX_PLATFORM_MEM_UNCACHED_SIZE] UNCACHED ALIGN_32;
static volatile uint8_t g_usbx_system_ready = 0;

UINT usbx_platform_system_init(void)
{
    UINT ret;

    if (g_usbx_system_ready) {
        return UX_SUCCESS;
    }

    ret = ux_system_initialize(g_usbx_mem_pool, USBX_PLATFORM_MEM_SIZE,
                               g_usbx_mem_pool_uncached, USBX_PLATFORM_MEM_UNCACHED_SIZE);
    if (ret == UX_SUCCESS) {
        g_usbx_system_ready = 1;
    }

    return ret;
}

int usbx_platform_system_is_ready(void)
{
    return g_usbx_system_ready ? 1 : 0;
}
