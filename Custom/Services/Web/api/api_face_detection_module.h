/**
 * @file api_face_detection_module.h
 * @brief Face Detection demo Web API module
 */

#ifndef API_FACE_DETECTION_MODULE_H
#define API_FACE_DETECTION_MODULE_H

#include "aicam_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the face detection demo API routes
 * @routes GET/POST /api/v1/face/config   demo configuration (merge update)
 *         GET     /api/v1/face/status    live statistics + model info
 *         GET     /api/v1/face/results   visible tracks + recognition state
 *         POST    /api/v1/face/trigger   force a report (all or one track)
 *         POST    /api/v1/face/reset     clear statistics and caches
 */
aicam_result_t web_api_register_face_detection_module(void);

#ifdef __cplusplus
}
#endif

#endif /* API_FACE_DETECTION_MODULE_H */
