/**
 * @file api_people_counting_module.h
 * @brief People Counting demo Web API module interface
 */

#ifndef API_PEOPLE_COUNTING_MODULE_H
#define API_PEOPLE_COUNTING_MODULE_H

#include "web_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the people counting API routes
 * @return aicam_result_t Operation result
 */
aicam_result_t web_api_register_people_counting_module(void);

#ifdef __cplusplus
}
#endif

#endif /* API_PEOPLE_COUNTING_MODULE_H */
