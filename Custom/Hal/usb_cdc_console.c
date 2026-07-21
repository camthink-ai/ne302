#include "usb_cdc_console.h"
#include "usbx_platform.h"
#include "debug.h"
#include "common_utils.h"
#include "cmsis_os2.h"

#include "ux_api.h"
#include "ux_dcd_stm32.h"
#include "ux_device_class_cdc_acm.h"
#include "usb_desc.h"

#include <string.h>

#define USB_CDC_RX_PACKET_SIZE          512U
#define USB_CDC_DESC_MAX_LEN            512U
#define USB_CDC_STRING_MAX_LEN          512U
#define USB_CDC_IO_TIMEOUT_MS           200U
#define USB_CDC_IO_TIMEOUT_TICKS        UX_MS_TO_TICK(USB_CDC_IO_TIMEOUT_MS)

static PCD_HandleTypeDef g_hpcd_usb_cdc;
static UX_SLAVE_CLASS_CDC_ACM *g_cdc_acm;
static volatile uint8_t g_cdc_console_active;
static volatile uint8_t g_cdc_device_ready;
static volatile uint8_t g_cdc_stack_ready;
static volatile uint8_t g_cdc_use_usb_path;
static volatile uint8_t g_cdc_dtr_asserted;
static osThreadId_t g_cdc_read_task;
static osMutexId_t g_cdc_io_mutex;

static uint8_t g_usb_desc_fs[USB_CDC_DESC_MAX_LEN] ALIGN_32;
static uint8_t g_usb_desc_hs[USB_CDC_DESC_MAX_LEN] ALIGN_32;
static uint8_t g_usb_dev_strings[USB_CDC_STRING_MAX_LEN];
static uint8_t g_usb_dev_langid[2];
static uint8_t g_cdc_rx_buffer[USB_CDC_RX_PACKET_SIZE];

static uint8_t g_cdc_read_stack[4096] ALIGN_32 IN_PSRAM;

static const osThreadAttr_t g_cdc_read_task_attr = {
    .name = "usbCdcRx",
    .priority = (osPriority_t)osPriorityLow,
    .stack_mem = g_cdc_read_stack,
    .stack_size = sizeof(g_cdc_read_stack),
};

static int usb_cdc_extract_string(uint8_t langid[2], int index, uint8_t *string_desc,
                                  uint8_t *dst, int dst_len)
{
    int str_len = (string_desc[0] - 2) / 2;
    int i;

    if (dst_len < str_len + 4) {
        return -1;
    }

    dst[0] = langid[0];
    dst[1] = langid[1];
    dst[2] = (uint8_t)index;
    dst[3] = (uint8_t)str_len;
    for (i = 0; i < str_len; i++) {
        dst[4 + i] = string_desc[2 + 2 * i];
    }

    return str_len + 4;
}

static int usb_cdc_build_dev_strings(uint8_t langid[2], uint8_t *dst, int dst_len)
{
    uint8_t string_desc[128];
    int res = 0;
    int len;

    len = usb_get_manufacturer_string_desc(string_desc, sizeof(string_desc));
    if (len < 0) {
        return len;
    }
    res += usb_cdc_extract_string(langid, 1, string_desc, &dst[res], dst_len - res);
    if (res < 0) {
        return 0;
    }

    len = usb_get_product_string_desc(string_desc, sizeof(string_desc));
    if (len < 0) {
        return len;
    }
    res += usb_cdc_extract_string(langid, 2, string_desc, &dst[res], dst_len - res);
    if (res < 0) {
        return 0;
    }

    len = usb_get_serial_string_desc(string_desc, sizeof(string_desc));
    if (len < 0) {
        return len;
    }
    res += usb_cdc_extract_string(langid, 3, string_desc, &dst[res], dst_len - res);
    if (res < 0) {
        return 0;
    }

    return res;
}

static int usb_cdc_pcd_init(PCD_HandleTypeDef *hpcd)
{
    int ret;

    memset(hpcd, 0, sizeof(PCD_HandleTypeDef));
    hpcd->Instance = USB1_OTG_HS;
    hpcd->Init.dev_endpoints = 9;
    hpcd->Init.speed = PCD_SPEED_HIGH;
    hpcd->Init.dma_enable = DISABLE;
    hpcd->Init.phy_itface = USB_OTG_HS_EMBEDDED_PHY;
    hpcd->Init.Sof_enable = DISABLE;
    hpcd->Init.low_power_enable = DISABLE;
    hpcd->Init.lpm_enable = DISABLE;
    hpcd->Init.vbus_sensing_enable = DISABLE;
    hpcd->Init.use_dedicated_ep1 = DISABLE;
    hpcd->Init.use_external_vbus = DISABLE;

    ret = HAL_PCD_Init(hpcd);
    if (ret != HAL_OK) {
        return ret;
    }

    HAL_PCDEx_SetRxFiFo(hpcd, 0xA1);
    HAL_PCDEx_SetTxFiFo(hpcd, 0, 0x10);
    HAL_PCDEx_SetTxFiFo(hpcd, 1, 0x100);
    HAL_PCDEx_SetTxFiFo(hpcd, 2, 0x10);

    return HAL_OK;
}

static void usb_cdc_apply_io_timeouts(UX_SLAVE_CLASS_CDC_ACM *cdc_acm)
{
    if (cdc_acm == UX_NULL) {
        return;
    }

    (void)ux_device_class_cdc_acm_ioctl(cdc_acm,
                                        UX_SLAVE_CLASS_CDC_ACM_IOCTL_SET_WRITE_TIMEOUT,
                                        (VOID *)(ULONG)USB_CDC_IO_TIMEOUT_TICKS);
    (void)ux_device_class_cdc_acm_ioctl(cdc_acm,
                                        UX_SLAVE_CLASS_CDC_ACM_IOCTL_SET_READ_TIMEOUT,
                                        (VOID *)(ULONG)USB_CDC_IO_TIMEOUT_TICKS);
}

static int usb_cdc_console_is_link_up_internal(void)
{
    UX_SLAVE_DEVICE *device = &_ux_system_slave->ux_system_slave_device;

    if (!g_cdc_console_active || !g_cdc_device_ready || g_cdc_acm == UX_NULL) {
        return 0;
    }

    return (device->ux_slave_device_state == UX_DEVICE_CONFIGURED) ? 1 : 0;
}

static void usb_cdc_fallback_to_uart(void)
{
    g_cdc_use_usb_path = 0;
}

static void usb_cdc_restore_usb_path(void)
{
    if (usb_cdc_console_is_link_up_internal()) {
        g_cdc_use_usb_path = 1;
    }
}

static void usb_cdc_abort_pipes(UX_SLAVE_CLASS_CDC_ACM *cdc_acm)
{
    if (cdc_acm == UX_NULL) {
        return;
    }

    (void)ux_device_class_cdc_acm_ioctl(cdc_acm,
                                        UX_SLAVE_CLASS_CDC_ACM_IOCTL_ABORT_PIPE,
                                        (VOID *)(ULONG)UX_SLAVE_CLASS_CDC_ACM_ENDPOINT_XMIT);
    (void)ux_device_class_cdc_acm_ioctl(cdc_acm,
                                        UX_SLAVE_CLASS_CDC_ACM_IOCTL_ABORT_PIPE,
                                        (VOID *)(ULONG)UX_SLAVE_CLASS_CDC_ACM_ENDPOINT_RCV);
}

static VOID usb_cdc_activate(VOID *cdc_instance)
{
    UX_SLAVE_CLASS_CDC_ACM *cdc_acm = (UX_SLAVE_CLASS_CDC_ACM *)cdc_instance;

    g_cdc_acm = cdc_acm;
    g_cdc_device_ready = 1;
    /* USB path NOT enabled here — wait for host to assert DTR or send data */
    g_cdc_use_usb_path = 0;
    g_cdc_dtr_asserted = 0;
    usb_cdc_apply_io_timeouts(cdc_acm);
}

static VOID usb_cdc_deactivate(VOID *cdc_instance)
{
    UX_SLAVE_CLASS_CDC_ACM *cdc_acm = (UX_SLAVE_CLASS_CDC_ACM *)cdc_instance;

    usb_cdc_abort_pipes(cdc_acm);
    g_cdc_acm = UX_NULL;
    g_cdc_device_ready = 0;
    g_cdc_use_usb_path = 0;
    g_cdc_dtr_asserted = 0;
}

static VOID usb_cdc_parameter_change(VOID *cdc_instance)
{
    UX_SLAVE_CLASS_CDC_ACM_LINE_CODING_PARAMETER line_coding;
    UX_SLAVE_TRANSFER *transfer_request;
    UX_SLAVE_DEVICE *device;
    ULONG request;
    UINT ret;

    device = &_ux_system_slave->ux_system_slave_device;
    transfer_request = &device->ux_slave_device_control_endpoint.ux_slave_endpoint_transfer_request;
    request = *(transfer_request->ux_slave_transfer_request_setup + UX_SETUP_REQUEST);

    switch (request) {
    case UX_SLAVE_CLASS_CDC_ACM_SET_LINE_CODING:
        ret = ux_device_class_cdc_acm_ioctl(cdc_instance,
                                            UX_SLAVE_CLASS_CDC_ACM_IOCTL_GET_LINE_CODING,
                                            &line_coding);
        (void)ret;
        break;
    case UX_SLAVE_CLASS_CDC_ACM_SET_CONTROL_LINE_STATE: {
        ULONG ctrl_state = *(transfer_request->ux_slave_transfer_request_setup + UX_SETUP_VALUE);
        if (ctrl_state & 1u) {  /* DTR asserted — host opened the port */
            g_cdc_dtr_asserted = 1;
            usb_cdc_restore_usb_path();
        } else {                /* DTR de-asserted — host closed the port */
            g_cdc_dtr_asserted = 0;
            usb_cdc_fallback_to_uart();
        }
        break;
    }
    default:
        break;
    }
}

static int usb_cdc_stack_init(void)
{
    uint8_t lang_string_desc[4];
    usb_desc_conf desc_conf = {0};
    UX_SLAVE_CLASS_CDC_ACM_PARAMETER cdc_acm_parameter;
    int usb_desc_hs_len;
    int usb_desc_fs_len;
    int usb_dev_strings_len;
    int usb_dev_langid_len;
    int len;
    UINT ret;

    if (g_cdc_stack_ready) {
        return 0;
    }

    ret = usbx_platform_system_init();
    if (ret != UX_SUCCESS) {
        return (int)ret;
    }

    desc_conf.is_hs = 1;
    usb_desc_hs_len = usb_get_device_desc(g_usb_desc_hs, sizeof(g_usb_desc_hs), 1, 2, 3);
    if (usb_desc_hs_len <= 0) {
        return -1;
    }
    len = usb_get_configuration_desc(&g_usb_desc_hs[usb_desc_hs_len],
                                     (int)sizeof(g_usb_desc_hs) - usb_desc_hs_len,
                                     &desc_conf);
    if (len <= 0) {
        return -1;
    }
    usb_desc_hs_len += len;

    desc_conf.is_hs = 0;
    usb_desc_fs_len = usb_get_device_desc(g_usb_desc_fs, sizeof(g_usb_desc_fs), 1, 2, 3);
    if (usb_desc_fs_len <= 0) {
        return -1;
    }
    len = usb_get_configuration_desc(&g_usb_desc_fs[usb_desc_fs_len],
                                     (int)sizeof(g_usb_desc_fs) - usb_desc_fs_len,
                                     &desc_conf);
    if (len <= 0) {
        return -1;
    }
    usb_desc_fs_len += len;

    len = usb_get_lang_string_desc(lang_string_desc, sizeof(lang_string_desc));
    if (len != (int)sizeof(lang_string_desc)) {
        return -1;
    }
    g_usb_dev_langid[0] = lang_string_desc[2];
    g_usb_dev_langid[1] = lang_string_desc[3];
    usb_dev_langid_len = 2;

    usb_dev_strings_len = usb_cdc_build_dev_strings(g_usb_dev_langid, g_usb_dev_strings,
                                                    sizeof(g_usb_dev_strings));
    if (usb_dev_strings_len <= 0) {
        return -1;
    }

    ret = ux_device_stack_initialize(g_usb_desc_hs, (ULONG)usb_desc_hs_len,
                                     g_usb_desc_fs, (ULONG)usb_desc_fs_len,
                                     g_usb_dev_strings, (ULONG)usb_dev_strings_len,
                                     g_usb_dev_langid, (ULONG)usb_dev_langid_len,
                                     UX_NULL);
    if (ret != UX_SUCCESS) {
        return (int)ret;
    }

    cdc_acm_parameter.ux_slave_class_cdc_acm_instance_activate = usb_cdc_activate;
    cdc_acm_parameter.ux_slave_class_cdc_acm_instance_deactivate = usb_cdc_deactivate;
    cdc_acm_parameter.ux_slave_class_cdc_acm_parameter_change = usb_cdc_parameter_change;

    ret = ux_device_stack_class_register(_ux_system_slave_class_cdc_acm_name,
                                         ux_device_class_cdc_acm_entry,
                                         1, 0, &cdc_acm_parameter);
    if (ret != UX_SUCCESS) {
        return (int)ret;
    }

    ret = ux_dcd_stm32_initialize((ULONG)USB1_OTG_HS, (ULONG)&g_hpcd_usb_cdc);
    if (ret != UX_SUCCESS) {
        return (int)ret;
    }

    g_cdc_stack_ready = 1;
    return 0;
}

static void usb_cdc_read_task(void *argument)
{
    ULONG rx_len;
    UINT ret;

    (void)argument;

    while (1) {
        if (!g_cdc_console_active || !g_cdc_acm || !usb_cdc_console_is_link_up_internal()) {
            osDelay(10);
            continue;
        }

        ret = ux_device_class_cdc_acm_read(g_cdc_acm, g_cdc_rx_buffer,
                                           sizeof(g_cdc_rx_buffer), &rx_len);
        if (ret != UX_SUCCESS) {
            osDelay(10);
            continue;
        }
        if (rx_len == 0) {
            osDelay(1);
            continue;
        }

        usb_cdc_restore_usb_path();

        for (ULONG i = 0; i < rx_len; i++) {
            debug_process_char((char)g_cdc_rx_buffer[i]);
        }
    }
}

int usb_cdc_console_init(void)
{
    int ret;

    if (g_cdc_read_task != NULL) {
        return 0;
    }

    if (g_cdc_io_mutex == NULL) {
        g_cdc_io_mutex = osMutexNew(NULL);
        if (g_cdc_io_mutex == NULL) {
            return -1;
        }
    }

    ret = usb_cdc_pcd_init(&g_hpcd_usb_cdc);
    if (ret != HAL_OK) {
        return ret;
    }

    ret = usb_cdc_stack_init();
    if (ret != 0) {
        return ret;
    }

    ret = HAL_PCD_Start(&g_hpcd_usb_cdc);
    if (ret != HAL_OK) {
        return ret;
    }

    g_cdc_read_task = osThreadNew(usb_cdc_read_task, NULL, &g_cdc_read_task_attr);
    if (g_cdc_read_task == NULL) {
        return -1;
    }

    return 0;
}

int usb_cdc_console_activate(void)
{
    g_cdc_console_active = 1;
    return 0;
}

int usb_cdc_console_is_active(void)
{
    return g_cdc_console_active ? 1 : 0;
}

int usb_cdc_console_is_link_up(void)
{
    return usb_cdc_console_is_link_up_internal();
}

int usb_cdc_console_is_host_ready(void)
{
    return usb_cdc_console_is_link_up_internal() && g_cdc_use_usb_path;
}

int usb_cdc_console_write(const uint8_t *data, uint32_t len)
{
    UX_SLAVE_CLASS_CDC_ACM *cdc_acm;
    ULONG sent_len = 0;
    UINT ret;

    if (!g_cdc_use_usb_path || !usb_cdc_console_is_link_up_internal() ||
        data == NULL || len == 0) {
        return 0;
    }

    if (g_cdc_io_mutex != NULL) {
        if (osMutexAcquire(g_cdc_io_mutex, USB_CDC_IO_TIMEOUT_MS) != osOK) {
            usb_cdc_fallback_to_uart();
            return 0;
        }
    }

    if (!g_cdc_use_usb_path || !usb_cdc_console_is_link_up_internal()) {
        if (g_cdc_io_mutex != NULL) {
            osMutexRelease(g_cdc_io_mutex);
        }
        return 0;
    }

    cdc_acm = g_cdc_acm;
    if (cdc_acm == UX_NULL) {
        if (g_cdc_io_mutex != NULL) {
            osMutexRelease(g_cdc_io_mutex);
        }
        usb_cdc_fallback_to_uart();
        return 0;
    }

    ret = ux_device_class_cdc_acm_write(cdc_acm, (UCHAR *)data, len, &sent_len);

    if (g_cdc_io_mutex != NULL) {
        osMutexRelease(g_cdc_io_mutex);
    }

    if (ret != UX_SUCCESS || sent_len == 0) {
        usb_cdc_fallback_to_uart();
        return 0;
    }

    return (int)sent_len;
}

PCD_HandleTypeDef *usb_cdc_console_get_pcd(void)
{
    return &g_hpcd_usb_cdc;
}
