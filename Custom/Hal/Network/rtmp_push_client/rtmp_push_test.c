/**
 * @file rtmp_push_test.c
 * @brief RTMP Push Streaming Test Command Implementation
 * @details Command-line interface for testing RTMP push streaming functionality
 */

#include "rtmp_push_test.h"
#include "rtmp_publisher.h"
#include "debug.h"
#include "dev_manager.h"
#include "camera.h"
#include "enc.h"
#include "cmsis_os2.h"
#include "Hal/mem.h"
#include "aicam_error.h"
#include "h264encapi.h"
#include "pixel_format_map.h"
#include "librtmp/log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>

/* ==================== Internal Structures ==================== */

typedef struct {
    rtmp_publisher_t *publisher;
    device_t *camera_dev;
    device_t *encoder_dev;
    bool is_running;
    osThreadId_t push_thread;
    osMutexId_t mutex;
    uint32_t frame_count;
    uint32_t error_count;
} rtmp_push_test_ctx_t;

static rtmp_push_test_ctx_t g_rtmp_test_ctx = {0};

const osThreadAttr_t rtmp_push_test_task_attributes = {
    .name = "rtmpPushTest",
    .priority = (osPriority_t) osPriorityNormal,
    .stack_size = 8 * 1024
};

/* ==================== Helper Functions ==================== */

/**
 * @brief Extract SPS and PPS from H.264 data
 */
static int extract_sps_pps_from_h264(const uint8_t *data, uint32_t size,
                                     uint8_t **sps, uint32_t *sps_size,
                                     uint8_t **pps, uint32_t *pps_size)
{
    const uint8_t *p = data;
    const uint8_t *end = data + size;
    
    *sps = NULL;
    *sps_size = 0;
    *pps = NULL;
    *pps_size = 0;
    
    // Find SPS (NAL type 7) and PPS (NAL type 8)
    while (p < end - 4) {
        // Check for start code (0x00000001)
        if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
            uint8_t nal_type = (p[4] & 0x1F);
            
            if (nal_type == 7) { // SPS
                const uint8_t *sps_start = p + 4;
                const uint8_t *sps_end = sps_start + 1;
                
                // Find next start code
                while (sps_end < end - 3) {
                    if (sps_end[0] == 0 && sps_end[1] == 0 && 
                        (sps_end[2] == 1 || (sps_end[2] == 0 && sps_end[3] == 1))) {
                        break;
                    }
                    sps_end++;
                }
                
                *sps_size = (uint32_t)(sps_end - sps_start);
                *sps = (uint8_t*)hal_mem_alloc_large(*sps_size);
                if (*sps) {
                    memcpy(*sps, sps_start, *sps_size);
                }
            } else if (nal_type == 8) { // PPS
                const uint8_t *pps_start = p + 4;
                const uint8_t *pps_end = pps_start + 1;
                
                // Find next start code
                while (pps_end < end - 3) {
                    if (pps_end[0] == 0 && pps_end[1] == 0 && 
                        (pps_end[2] == 1 || (pps_end[2] == 0 && pps_end[3] == 1))) {
                        break;
                    }
                    pps_end++;
                }
                
                *pps_size = (uint32_t)(pps_end - pps_start);
                *pps = (uint8_t*)hal_mem_alloc_large(*pps_size);
                if (*pps) {
                    memcpy(*pps, pps_start, *pps_size);
                }
            }
            
            // Skip to next potential start code
            p += 4;
        } else if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
            // 3-byte start code
            uint8_t nal_type = (p[3] & 0x1F);
            
            if (nal_type == 7) { // SPS
                const uint8_t *sps_start = p + 3;
                const uint8_t *sps_end = sps_start + 1;
                
                while (sps_end < end - 2) {
                    if (sps_end[0] == 0 && sps_end[1] == 0 && 
                        (sps_end[2] == 1 || (sps_end[2] == 0 && sps_end[3] == 1))) {
                        break;
                    }
                    sps_end++;
                }
                
                *sps_size = (uint32_t)(sps_end - sps_start);
                *sps = (uint8_t*)hal_mem_alloc_large(*sps_size);
                if (*sps) {
                    memcpy(*sps, sps_start, *sps_size);
                }
            } else if (nal_type == 8) { // PPS
                const uint8_t *pps_start = p + 3;
                const uint8_t *pps_end = pps_start + 1;
                
                while (pps_end < end - 2) {
                    if (pps_end[0] == 0 && pps_end[1] == 0 && 
                        (pps_end[2] == 1 || (pps_end[2] == 0 && pps_end[3] == 1))) {
                        break;
                    }
                    pps_end++;
                }
                
                *pps_size = (uint32_t)(pps_end - pps_start);
                *pps = (uint8_t*)hal_mem_alloc_large(*pps_size);
                if (*pps) {
                    memcpy(*pps, pps_start, *pps_size);
                }
            }
            
            p += 3;
        } else {
            p++;
        }
    }
    
    return (*sps && *pps) ? 0 : -1;
}

/**
 * @brief RTMP push thread function
 */
static void rtmp_push_thread(void *argument)
{
    rtmp_push_test_ctx_t *ctx = (rtmp_push_test_ctx_t *)argument;
    uint8_t *camera_buffer = NULL;
    uint32_t frame_interval_ms = 33; // ~30fps
    uint32_t timestamp_ms = 0, start_timestamp_ms = 0;
    bool sps_pps_sent = false;
    uint8_t *sps_data = NULL;
    uint32_t sps_size = 0;
    uint8_t *pps_data = NULL;
    uint32_t pps_size = 0;
    int ret = 0;
    
    LOG_SIMPLE("[RTMP_TEST] Push thread started\r\n");
    
    // Get encoder parameters
    enc_param_t enc_param = {0};
    if (device_ioctl(ctx->encoder_dev, ENC_CMD_GET_PARAM, 
                    (uint8_t *)&enc_param, sizeof(enc_param_t)) != AICAM_OK) {
        LOG_SIMPLE("[RTMP_TEST] Failed to get encoder params\r\n");
        goto cleanup;
    }
    
    frame_interval_ms = 1000 / enc_param.fps;
    LOG_SIMPLE("[RTMP_TEST] Encoder: %dx%d @ %dfps, interval: %dms\r\n",
               enc_param.width, enc_param.height, enc_param.fps, frame_interval_ms);
    
    start_timestamp_ms = osKernelGetTickCount();
    while (ctx->is_running) {
        if (!rtmp_publisher_is_connected(ctx->publisher)) {
            LOG_SIMPLE("[RTMP_TEST] Publisher not connected, reconnecting...\r\n");
            ret = rtmp_publisher_connect(ctx->publisher);
            if (ret != RTMP_PUB_OK) LOG_SIMPLE("[RTMP_TEST] Failed to reconnect: %d\r\n", ret);
            else LOG_SIMPLE("[RTMP_TEST] Reconnected successfully\r\n");
            osDelay(1000);
        } else {
            // Get camera frame
            int fb_len = device_ioctl(ctx->camera_dev, CAM_CMD_GET_PIPE1_BUFFER,
                                    (uint8_t *)&camera_buffer, 0);
            if (fb_len <= 0 || camera_buffer == NULL) {
                osDelay(10);
                continue;
            }
            
            // Encode frame
            int ret = device_ioctl(ctx->encoder_dev, ENC_CMD_INPUT_BUFFER,
                                camera_buffer, fb_len);
            if (ret != AICAM_OK) {
                device_ioctl(ctx->camera_dev, CAM_CMD_RETURN_PIPE1_BUFFER, camera_buffer, 0);
                ctx->error_count++;
                LOG_SIMPLE("[RTMP_TEST] ENC_CMD_INPUT_BUFFER failed, ret=%d\r\n", ret);
                osDelay(10);
                continue;
            }
            
            // Get encoded frame
            enc_out_frame_t enc_frame = {0};
            ret = device_ioctl(ctx->encoder_dev, ENC_CMD_OUTPUT_FRAME,
                            (uint8_t *)&enc_frame, 0);
            device_ioctl(ctx->camera_dev, CAM_CMD_RETURN_PIPE1_BUFFER, camera_buffer, 0);
            
            if (ret != AICAM_OK || enc_frame.data_size == 0) {
                ctx->error_count++;
                LOG_SIMPLE("[RTMP_TEST] ENC_CMD_OUTPUT_FRAME failed, ret=%d, size=%lu\r\n",
                        ret, (unsigned long)enc_frame.data_size);
                osDelay(10);
                continue;
            }
            
            // Extract SPS/PPS from first I-frame if not sent
            if (!sps_pps_sent && enc_frame.frame_info.codingType == H264ENC_INTRA_FRAME) {
                uint8_t *frame_data = enc_frame.frame_buffer + enc_frame.header_size;
                if (extract_sps_pps_from_h264(frame_data, enc_frame.data_size,
                                            &sps_data, &sps_size,
                                            &pps_data, &pps_size) == 0) {
                    ret = rtmp_publisher_send_sps_pps(ctx->publisher, 
                                                    sps_data, sps_size,
                                                    pps_data, pps_size);
                    if (ret == RTMP_PUB_OK) {
                        sps_pps_sent = true;
                        // LOG_SIMPLE("[RTMP_TEST] SPS/PPS sent successfully\r\n");
                    } else {
                        LOG_SIMPLE("[RTMP_TEST] Failed to send SPS/PPS: %d\r\n", ret);
                    }
                }
            }
            
            // Send frame if SPS/PPS already sent
            if (sps_pps_sent) {
                uint8_t *frame_data = enc_frame.frame_buffer + enc_frame.header_size;
                bool is_keyframe = (enc_frame.frame_info.codingType == H264ENC_INTRA_FRAME);
                // bool is_pframe   = (enc_frame.frame_info.codingType == H264ENC_PREDICTED_FRAME);
                
                // if (is_keyframe) {
                //     LOG_SIMPLE("[RTMP_TEST] I-frame first 4 bytes: %02X %02X %02X %02X, size=%lu\r\n",
                //                frame_data[0], frame_data[1], frame_data[2], frame_data[3],
                //                (unsigned long)enc_frame.data_size);
                // }

                timestamp_ms = osKernelGetTickCount() - start_timestamp_ms;
                ret = rtmp_publisher_send_video_frame(ctx->publisher,
                                                    frame_data,
                                                    enc_frame.data_size,
                                                    is_keyframe,
                                                    timestamp_ms);
                
                if (ret == RTMP_PUB_OK) {
                    ctx->frame_count++;
                } else {
                    ctx->error_count++;
                    rtmp_publisher_disconnect(ctx->publisher);
                    LOG_SIMPLE("[RTMP_TEST] Failed to send frame: %d (codingType=%d)\r\n",
                            ret, enc_frame.frame_info.codingType);
                }
            }
        }
        // Keep SPS/PPS data for re-sending before each I-frame (for HLS compatibility)
        // Don't free them here - they will be freed when stream stops
    }
    
cleanup:
    if (sps_data) {
        hal_mem_free(sps_data);
    }
    if (pps_data) {
        hal_mem_free(pps_data);
    }
    
    // Stop encoder and camera if still running
    // if (ctx->encoder_dev) {
    //     device_stop(ctx->encoder_dev);
    // }
    // if (ctx->camera_dev) {
    //     device_stop(ctx->camera_dev);
    // }
    
    LOG_SIMPLE("[RTMP_TEST] Push thread stopped\r\n");
    osThreadExit();
}

/* ==================== Command Functions ==================== */

/**
 * @brief Start RTMP push streaming
 * Usage: rtmp_start <url> [width] [height] [fps]
 */
static int rtmp_start_cmd(int argc, char* argv[])
{
    int ret = 0;
    if (argc < 2) {
        LOG_SIMPLE("Usage: rtmp_start <url> [width] [height] [fps]\r\n");
        LOG_SIMPLE("Example: rtmp_start rtmp://example.com/live/stream 1280 720 30\r\n");
        return -1;
    }
    
    rtmp_push_test_ctx_t *ctx = &g_rtmp_test_ctx;
    
    osMutexAcquire(ctx->mutex, osWaitForever);
    
    if (ctx->is_running) {
        LOG_SIMPLE("[RTMP_TEST] Already running, stop first\r\n");
        osMutexRelease(ctx->mutex);
        return -1;
    }
    
    // Find devices
    ctx->camera_dev = device_find_pattern(CAMERA_DEVICE_NAME, DEV_TYPE_VIDEO);
    ctx->encoder_dev = device_find_pattern(ENC_DEVICE_NAME, DEV_TYPE_VIDEO);
    
    if (!ctx->camera_dev || !ctx->encoder_dev) {
        LOG_SIMPLE("[RTMP_TEST] Camera or encoder device not found\r\n");
        osMutexRelease(ctx->mutex);
        return -1;
    }

    // Set RTMP log level to all
    // RTMP_LogSetLevel(RTMP_LOGDEBUG);

    // Create RTMP publisher
    rtmp_pub_config_t config = {0};
    rtmp_publisher_get_default_config(&config);
    strcpy(config.url, argv[1]);
    
    if (argc >= 3) {
        config.width = atoi(argv[2]);
    }
    if (argc >= 4) {
        config.height = atoi(argv[3]);
    }
    if (argc >= 5) {
        config.fps = atoi(argv[4]);
    }
    
    ctx->publisher = rtmp_publisher_create(&config);
    if (!ctx->publisher) {
        LOG_SIMPLE("[RTMP_TEST] Failed to create RTMP publisher\r\n");
        osMutexRelease(ctx->mutex);
        return -1;
    }
    
    // Connect to RTMP server
    LOG_SIMPLE("[RTMP_TEST] Connecting to %s...\r\n", config.url);
    ret = rtmp_publisher_connect(ctx->publisher);
    if (ret != RTMP_PUB_OK) {
        LOG_SIMPLE("[RTMP_TEST] Failed to connect: %d\r\n", ret);
        rtmp_publisher_destroy(ctx->publisher);
        ctx->publisher = NULL;
        osMutexRelease(ctx->mutex);
        return -1;
    }
    
    LOG_SIMPLE("[RTMP_TEST] Connected successfully\r\n");

    // Configure and start camera
    pipe_params_t pipe_param = {0};
    ret = device_ioctl(ctx->camera_dev, CAM_CMD_GET_PIPE1_PARAM,
                       (uint8_t *)&pipe_param, sizeof(pipe_params_t));
    if (ret != AICAM_OK) {
        LOG_SIMPLE("[RTMP_TEST] Failed to get camera pipe params, ret=%d\r\n", ret);
        rtmp_publisher_disconnect(ctx->publisher);
        rtmp_publisher_destroy(ctx->publisher);
        ctx->publisher = NULL;
        osMutexRelease(ctx->mutex);
        return -1;
    }
    
    if (config.width != pipe_param.width || config.height != pipe_param.height || config.fps != pipe_param.fps) {
        // Update camera parameters if provided
        pipe_param.width = config.width;
        pipe_param.height = config.height;
        pipe_param.fps = config.fps;

        ret = device_ioctl(ctx->camera_dev, CAM_CMD_SET_PIPE1_PARAM,
                        (uint8_t *)&pipe_param, sizeof(pipe_params_t));
        if (ret != AICAM_OK) {
            LOG_SIMPLE("[RTMP_TEST] Failed to set camera pipe params, ret=%d\r\n", ret);
            rtmp_publisher_disconnect(ctx->publisher);
            rtmp_publisher_destroy(ctx->publisher);
            ctx->publisher = NULL;
            osMutexRelease(ctx->mutex);
            return -1;
        }
        
        LOG_SIMPLE("[RTMP_TEST] Camera configured: %dx%d @ %dfps\r\n",
                pipe_param.width, pipe_param.height, pipe_param.fps);
    }
    
    // Configure encoder
    enc_param_t enc_param = {0};
    ret = device_ioctl(ctx->encoder_dev, ENC_CMD_GET_PARAM,
                       (uint8_t *)&enc_param, sizeof(enc_param_t));
    if (ret != AICAM_OK) {
        LOG_SIMPLE("[RTMP_TEST] Failed to get encoder params, ret=%d\r\n", ret);
        rtmp_publisher_disconnect(ctx->publisher);
        rtmp_publisher_destroy(ctx->publisher);
        ctx->publisher = NULL;
        osMutexRelease(ctx->mutex);
        return -1;
    }
    
    if (config.width != enc_param.width || config.height != enc_param.height || config.fps != enc_param.fps) {
        enc_param.width = pipe_param.width;
        enc_param.height = pipe_param.height;
        enc_param.fps = pipe_param.fps;
        enc_param.input_type = fmt_dcmipp_to_enc(pipe_param.format);
        enc_param.bpp = ENC_BYTES_PER_PIXEL(enc_param.input_type);
        
        if (enc_param.input_type < 0) {
            LOG_SIMPLE("[RTMP_TEST] Unsupported camera format: %d\r\n", pipe_param.format);
            rtmp_publisher_disconnect(ctx->publisher);
            rtmp_publisher_destroy(ctx->publisher);
            ctx->publisher = NULL;
            osMutexRelease(ctx->mutex);
            return -1;
        }
        
        ret = device_ioctl(ctx->encoder_dev, ENC_CMD_SET_PARAM,
                        (uint8_t *)&enc_param, sizeof(enc_param_t));
        if (ret != AICAM_OK) {
            LOG_SIMPLE("[RTMP_TEST] Failed to set encoder params, ret=%d\r\n", ret);
            rtmp_publisher_disconnect(ctx->publisher);
            rtmp_publisher_destroy(ctx->publisher);
            ctx->publisher = NULL;
            osMutexRelease(ctx->mutex);
            return -1;
        }
    
        LOG_SIMPLE("[RTMP_TEST] Encoder configured: %dx%d @ %dfps\r\n",
                enc_param.width, enc_param.height, enc_param.fps);
    }
    
    // Start camera
    ret = device_start(ctx->camera_dev);
    if (ret != AICAM_OK) {
        LOG_SIMPLE("[RTMP_TEST] Failed to start camera, ret=%d\r\n", ret);
        rtmp_publisher_disconnect(ctx->publisher);
        rtmp_publisher_destroy(ctx->publisher);
        ctx->publisher = NULL;
        osMutexRelease(ctx->mutex);
        return -1;
    }
    LOG_SIMPLE("[RTMP_TEST] Camera started\r\n");
    
    // Start encoder
    ret = device_start(ctx->encoder_dev);
    if (ret != AICAM_OK) {
        LOG_SIMPLE("[RTMP_TEST] Failed to start encoder, ret=%d\r\n", ret);
        // device_stop(ctx->camera_dev);
        rtmp_publisher_disconnect(ctx->publisher);
        rtmp_publisher_destroy(ctx->publisher);
        ctx->publisher = NULL;
        osMutexRelease(ctx->mutex);
        return -1;
    }
    LOG_SIMPLE("[RTMP_TEST] Encoder started\r\n");
    
    // Start push thread
    ctx->is_running = true;
    ctx->frame_count = 0;
    ctx->error_count = 0;
    
    ctx->push_thread = osThreadNew(rtmp_push_thread, ctx, &rtmp_push_test_task_attributes);
    
    if (!ctx->push_thread) {
        LOG_SIMPLE("[RTMP_TEST] Failed to create push thread\r\n");
        // device_stop(ctx->encoder_dev);
        // device_stop(ctx->camera_dev);
        rtmp_publisher_disconnect(ctx->publisher);
        rtmp_publisher_destroy(ctx->publisher);
        ctx->publisher = NULL;
        ctx->is_running = false;
        osMutexRelease(ctx->mutex);
        return -1;
    }
    
    osMutexRelease(ctx->mutex);
    LOG_SIMPLE("[RTMP_TEST] Push streaming started\r\n");
    
    return 0;
}

/**
 * @brief Stop RTMP push streaming
 * Usage: rtmp_stop
 */
static int rtmp_stop_cmd(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    
    rtmp_push_test_ctx_t *ctx = &g_rtmp_test_ctx;
    
    osMutexAcquire(ctx->mutex, osWaitForever);
    
    if (!ctx->is_running) {
        LOG_SIMPLE("[RTMP_TEST] Not running\r\n");
        osMutexRelease(ctx->mutex);
        return -1;
    }
    
    // Stop push thread
    ctx->is_running = false;
    osMutexRelease(ctx->mutex);
    
    // Wait for thread to exit
    if (ctx->push_thread) {
        osDelay(100);
        osThreadTerminate(ctx->push_thread);
        if (osThreadGetState(ctx->push_thread) != osThreadTerminated) {
            osThreadTerminate(ctx->push_thread);
        }
        ctx->push_thread = NULL;
    }
    
    // Stop encoder and camera
    osMutexAcquire(ctx->mutex, osWaitForever);
    // if (ctx->encoder_dev) {
    //     device_stop(ctx->encoder_dev);
    //     LOG_SIMPLE("[RTMP_TEST] Encoder stopped\r\n");
    // }
    // if (ctx->camera_dev) {
    //     device_stop(ctx->camera_dev);
    //     LOG_SIMPLE("[RTMP_TEST] Camera stopped\r\n");
    // }
    
    // Disconnect and destroy publisher
    if (ctx->publisher) {
        rtmp_publisher_disconnect(ctx->publisher);
        rtmp_publisher_destroy(ctx->publisher);
        ctx->publisher = NULL;
    }
    osMutexRelease(ctx->mutex);
    
    LOG_SIMPLE("[RTMP_TEST] Push streaming stopped\r\n");
    LOG_SIMPLE("[RTMP_TEST] Frames sent: %lu, Errors: %lu\r\n", 
               (unsigned long)ctx->frame_count, (unsigned long)ctx->error_count);
    
    return 0;
}

/**
 * @brief Show RTMP push statistics
 * Usage: rtmp_stats
 */
static int rtmp_stats_cmd(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    uint32_t last_frame_count = 0;
    float fps = 0.0f;
    rtmp_push_test_ctx_t *ctx = &g_rtmp_test_ctx;
    
    osMutexAcquire(ctx->mutex, osWaitForever);
    if (!ctx->publisher) {
        LOG_SIMPLE("[RTMP_TEST] Publisher not initialized\r\n");
        osMutexRelease(ctx->mutex);
        return -1;
    }
    
    last_frame_count = ctx->frame_count;
    osMutexRelease(ctx->mutex);
    osDelay(500);
    osMutexAcquire(ctx->mutex, osWaitForever);
    fps = (float)(ctx->frame_count - last_frame_count) / 0.5f;
    
    rtmp_pub_stats_t stats = {0};
    rtmp_publisher_get_stats(ctx->publisher, &stats);
    
    LOG_SIMPLE("[RTMP_TEST] === RTMP Push Statistics ===\r\n");
    LOG_SIMPLE("  Status: %s\r\n", ctx->is_running ? "Running" : "Stopped");
    LOG_SIMPLE("  Connected: %s\r\n", 
               rtmp_publisher_is_connected(ctx->publisher) ? "Yes" : "No");
    LOG_SIMPLE("  Frames sent: %lu\r\n", (unsigned long)stats.frames_sent);
    LOG_SIMPLE("  Bytes sent: %lu\r\n", (unsigned long)stats.bytes_sent);
    LOG_SIMPLE("  Errors: %lu\r\n", (unsigned long)stats.errors);
    LOG_SIMPLE("  Last frame size: %lu bytes\r\n", (unsigned long)stats.last_frame_size);
    LOG_SIMPLE("  Avg frame size: %lu bytes\r\n", (unsigned long)stats.avg_frame_size);
    LOG_SIMPLE("  Test frame count: %lu\r\n", (unsigned long)ctx->frame_count);
    LOG_SIMPLE("  Test error count: %lu\r\n", (unsigned long)ctx->error_count);
    LOG_SIMPLE("  FPS: %.2f\r\n", fps);
    LOG_SIMPLE("========================================\r\n");
    
    osMutexRelease(ctx->mutex);
    
    return 0;
}

/* ==================== Command Registration ==================== */

debug_cmd_reg_t rtmp_test_cmd_table[] = {
    {"rtmp_start", "Start RTMP push streaming: rtmp_start <url> [width] [height] [fps]", rtmp_start_cmd},
    {"rtmp_stop",  "Stop RTMP push streaming", rtmp_stop_cmd},
    {"rtmp_stats", "Show RTMP push statistics", rtmp_stats_cmd},
};

/**
 * @brief Register RTMP test commands
 */
void rtmp_push_test_register_commands(void)
{
    // Initialize mutex if not already done
    if (g_rtmp_test_ctx.mutex == NULL) {
        g_rtmp_test_ctx.mutex = osMutexNew(NULL);
    }
    
    debug_cmdline_register(rtmp_test_cmd_table, 
                          sizeof(rtmp_test_cmd_table) / sizeof(rtmp_test_cmd_table[0]));
    // LOG_SIMPLE("[RTMP_TEST] Commands registered\r\n");
}

