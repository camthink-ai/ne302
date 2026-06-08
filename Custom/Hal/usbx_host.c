#include "usbx_host.h"
#include "usbx_platform.h"
#include "common_utils.h"
#include "debug.h"
#include "usb_otg.h"
#include "ux_system.h"
#include "ux_utility.h"

#ifdef UX_HCD_ECM_USE_USB_OTG_HS1
extern HCD_HandleTypeDef hhcd_USB_OTG_HS1;
#else
extern HCD_HandleTypeDef hhcd_USB_OTG_HS2;
#endif
static uint8_t usbx_host_is_init = 0;

int USBX_Host_Init(ux_host_config_t *config)
{
    int ret = 0;
    if (config == NULL) return UX_INVALID_PARAMETER;
    if (usbx_host_is_init) return UX_SUCCESS;

    /* Initialize the USB OTG HS2 */
#ifdef UX_HCD_ECM_USE_USB_OTG_HS1
    if (hhcd_USB_OTG_HS1.State == HAL_HCD_STATE_RESET) MX_USB1_OTG_HS_HCD_Init();
#else
    if (hhcd_USB_OTG_HS2.State == HAL_HCD_STATE_RESET) MX_USB2_OTG_HS_HCD_Init();
#endif

    if (!config->is_uninit_memory) {
        ret = usbx_platform_system_init();
        if (ret != UX_SUCCESS) {
            LOG_DRV_ERROR("USBX Memory Initialization Failed: 0x%X", ret);
            goto USBX_Host_Init_Exit;
        }
    }
    
    /* Install the host portion of USBX */
    ret = ux_host_stack_initialize(config->event_callback);
    if (ret != UX_SUCCESS) {
        LOG_DRV_ERROR("USBX Host Initialization Failed: 0x%X", ret);
        goto USBX_Host_Init_Exit;
    }

    /* Register a callback error function */
    ux_utility_error_callback_register(config->error_callback);

    /* Register the host class */
    ret = ux_host_stack_class_register(config->class_name, config->class_entry_function);
    if (ret != UX_SUCCESS) {
        LOG_DRV_ERROR("USBX Host Class Registration Failed: 0x%X", ret);
        goto USBX_Host_Init_Exit;
    }

    /* Register the host HCD */
#ifdef UX_HCD_ECM_USE_USB_OTG_HS1
    ret = ux_host_stack_hcd_register(config->hcd_name, config->hcd_init_function, USB1_OTG_HS_BASE, (ULONG)&hhcd_USB_OTG_HS1);
#else
    ret = ux_host_stack_hcd_register(config->hcd_name, config->hcd_init_function, USB2_OTG_HS_BASE, (ULONG)&hhcd_USB_OTG_HS2);
#endif
    if (ret != UX_SUCCESS) {
        LOG_DRV_ERROR("USBX Host HCD Registration Failed: 0x%X", ret);
        goto USBX_Host_Init_Exit;
    }

#ifdef UX_HCD_ECM_USE_USB_OTG_HS1
    ret = HAL_HCD_Start(&hhcd_USB_OTG_HS1);
#else
    ret = HAL_HCD_Start(&hhcd_USB_OTG_HS2);
#endif
    if (ret != HAL_OK) {
        LOG_DRV_ERROR("USBX Host HCD Start Failed: 0x%X", ret);
        goto USBX_Host_Init_Exit;
    }
    
    usbx_host_is_init = 1;
    return ret;
USBX_Host_Init_Exit:
    USBX_Host_Deinit(config);
    return ret;
}

void USBX_Host_Deinit(ux_host_config_t *config)
{
//     int ret = 0;
//     if (config == NULL || !usbx_is_init) return;

// #ifdef UX_HCD_ECM_USE_USB_OTG_HS1
//     ret = HAL_HCD_Stop(&hhcd_USB_OTG_HS1);
// #else
//     ret = HAL_HCD_Stop(&hhcd_USB_OTG_HS2);
// #endif
//     if (ret != HAL_OK) {
//         LOG_DRV_ERROR("USBX Host HCD Stop Failed: 0x%X", ret);
//     }
// #ifdef UX_HCD_ECM_USE_USB_OTG_HS1
//     ret = ux_host_stack_hcd_unregister(config->hcd_name, USB1_OTG_HS_BASE, (ULONG)&hhcd_USB_OTG_HS1);
// #else
//     ret = ux_host_stack_hcd_unregister(config->hcd_name, USB2_OTG_HS_BASE, (ULONG)&hhcd_USB_OTG_HS2);
// #endif
//     if (ret != UX_SUCCESS) {
//         LOG_DRV_ERROR("USBX Host HCD Unregistration Failed: 0x%X", ret);
//     }
//     ret = ux_host_stack_class_unregister(config->class_entry_function);
//     if (ret != UX_SUCCESS) {
//         LOG_DRV_ERROR("USBX Host Class Unregistration Failed: 0x%X", ret);
//     }
//     ret = ux_host_stack_uninitialize();
//     if (ret != UX_SUCCESS) {
//         LOG_DRV_ERROR("USBX Host Uninitialization Failed: 0x%X", ret);
//     }
//     if (!config->is_uninit_memory) {
//         ret = ux_system_uninitialize();
//         if (ret != UX_SUCCESS) {
//             LOG_DRV_ERROR("USBX Memory Uninitialization Failed: 0x%X", ret);
//         }
//     }
//     // TODO: Using HAL library to reinitialize functions can cause system crashes, to be investigated
// #ifdef UX_HCD_ECM_USE_USB_OTG_HS1
//     HAL_HCD_DeInit(&hhcd_USB_OTG_HS1);
// #else
//     HAL_HCD_DeInit(&hhcd_USB_OTG_HS2);
// #endif
//     usbx_is_init = 0;
}
