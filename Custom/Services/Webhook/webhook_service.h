/**
 * @file webhook_service.h
 * @brief Webhook service — push capture images via HTTP(S) POST
 */

#ifndef WEBHOOK_SERVICE_H
#define WEBHOOK_SERVICE_H

#include "aicam_types.h"
#include "service_interfaces.h"
#include "mqtt_service.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Service Lifecycle ==================== */

aicam_result_t webhook_service_init(void *config);
aicam_result_t webhook_service_start(void);
aicam_result_t webhook_service_stop(void);
aicam_result_t webhook_service_deinit(void);
service_state_t webhook_service_get_state(void);

aicam_bool_t webhook_service_is_enabled(void);

/**
 * @brief Push a JPEG capture image to the configured webhook URL
 *        Runs asynchronously in a dedicated task — does not block the caller.
 *        The caller must provide a jpeg_data buffer that remains valid until
 *        the push completes (the service takes ownership and frees it via buffer_free).
 * @param jpeg_data   JPEG image buffer (ownership transferred on success)
 * @param jpeg_size   JPEG image size in bytes
 * @param metadata    Image metadata (copied internally)
 * @param ai_result   AI inference result (copied internally), may be NULL
 */
aicam_result_t webhook_service_push_capture(
    const uint8_t *jpeg_data, uint32_t jpeg_size,
    const mqtt_image_metadata_t *metadata,
    const mqtt_ai_result_t *ai_result);

/**
 * @brief Send a test push with a minimal payload
 */
aicam_result_t webhook_service_test_push(void);

#ifdef __cplusplus
}
#endif

#endif /* WEBHOOK_SERVICE_H */
