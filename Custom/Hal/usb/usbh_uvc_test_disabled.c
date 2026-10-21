/**
 * @file    usbh_uvc_test_disabled.c
 * @brief   Link stubs for the host UVC test module (usbh_uvc_test.c).
 *
 * The real module carries ~3 MB of PSRAM frame buffers (capture + preview
 * triple buffer + record staging). The NE302 board has a 32 MB PSRAM and
 * its .psram_section budget (central pool + lwip heap + service buffers)
 * has no room for them — the link overflows. The module is therefore only
 * compiled on BOARD_PSRAM_SIZE=64 builds (same condition as the 60 MB
 * linker script); these stubs keep the CLI ("usb host uvc ...") and the
 * web preview endpoints (/uvc.jpg, /uvc.mjpg) linkable on 32 MB builds,
 * where every UVC call reports "not supported".
 */
#include <stdint.h>
#include "usbh_uvc_test.h"

int usbh_uvc_test_info(void)      { return -1; }
int usbh_uvc_test_open(uint16_t width, uint16_t height, uint8_t alt)
{
    (void)width; (void)height; (void)alt;
    return -1;
}
int usbh_uvc_test_close(void)     { return -1; }
int usbh_uvc_test_start(void)     { return -1; }
int usbh_uvc_test_stop(void)      { return -1; }
int usbh_uvc_test_stat(void)      { return -1; }
int usbh_uvc_test_capture(uint32_t timeout_ms) { (void)timeout_ms; return -1; }
int usbh_uvc_test_dump(uint32_t offset, uint32_t len)
{
    (void)offset; (void)len;
    return -1;
}
int usbh_uvc_test_raw(void)       { return -1; }
int usbh_uvc_test_hdrdbg(void)    { return -1; }
int usbh_uvc_test_isodbg(void)    { return -1; }
int usbh_uvc_test_webfps(int fps) { (void)fps; return -1; }
int usbh_uvc_test_trace(uint32_t count) { (void)count; return -1; }
int usbh_uvc_test_record(uint32_t seconds, const char *filename)
{
    (void)seconds; (void)filename;
    return -1;
}
uint32_t usbh_uvc_preview_get(const uint8_t **buf, uint32_t *len, uint16_t *w, uint16_t *h)
{
    (void)buf; (void)len; (void)w; (void)h;
    return 0;
}
void usbh_uvc_preview_acquire(void)  {}
void usbh_uvc_preview_release(void)  {}
