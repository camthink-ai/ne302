/**
 * @file video_ai_node.h
 * @brief Video AI Processing Node
 * @details AI inference node based on nn.h/nn.c device interaction with zero-copy
 */

#ifndef VIDEO_AI_NODE_H
#define VIDEO_AI_NODE_H

#include "video_pipeline.h"
#include "aicam_types.h"
#include "dev_manager.h"
#include "nn.h"
#include "ai_draw_service.h"
#include "camera.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NN_RESULT_CACHE_SIZE 5

/* Max person-anchored face ROIs copied per camera frame */
#define VIDEO_AI_FACE_ROI_MAX 4

/* ==================== AI Node Configuration ==================== */

/**
 * @brief AI node configuration
 */
typedef struct {
    uint32_t width;                       // Input width
    uint32_t height;                      // Input height
    uint32_t fps;                         // Frame rate
    uint32_t input_format;                // Input format
    uint32_t confidence_threshold;        // Confidence threshold (0-100)
    uint32_t nms_threshold;               // NMS threshold (0-100)
    uint32_t max_detections;              // Maximum detections per frame
    uint32_t processing_interval;         // Processing interval (frames)
    uint32_t inference_interval_ms;       // Inference pacing interval in ms (0 = every frame)
    uint32_t bpp;                         // Bits per pixel
    aicam_bool_t enabled;                 // AI processing enabled
    aicam_bool_t overlay_results;         // Overlay detection results
    aicam_bool_t enable_drawing;          // Enable AI result drawing
    ai_draw_config_t draw_config;         // Drawing configuration
} video_ai_config_t;

/**
 * @brief AI node statistics
 */
typedef struct {
    uint64_t frames_processed;            // Total frames processed
    uint64_t frames_skipped;              // Frames skipped (disabled/interval)
    uint64_t detections_found;            // Total detections found
    uint64_t processing_errors;           // Processing errors
    uint64_t drawing_errors;              // Drawing errors
    uint64_t avg_processing_time_us;      // Average processing time
    uint64_t max_processing_time_us;      // Maximum processing time
    uint64_t avg_drawing_time_us;         // Average drawing time
    uint64_t max_drawing_time_us;         // Maximum drawing time
    uint32_t current_detection_count;     // Current frame detection count
} video_ai_stats_t;

typedef struct {
    nn_result_t result;
    //uint64_t timestamp;        // Frame timestamp (milliseconds)
    uint32_t frame_id;         // Frame ID for synchronization
} nn_result_with_frame_id_t;

/**
 * @brief Inference result callback
 * @details Invoked on the AI node's processing thread immediately after a
 *          successful inference, while the postprocess output buffers still
 *          hold that inference's data. The callee must copy whatever it needs
 *          before returning and must not block.
 */
typedef void (*video_ai_result_callback_t)(const nn_result_t *result,
                                           uint32_t frame_id,
                                           uint32_t inference_time_ms,
                                           void *user_data);

/**
 * @brief AI node private data
 */
typedef struct {
    device_t *ai_device;                  // AI device handle
    video_ai_config_t config;             // AI configuration
    video_ai_stats_t stats;               // AI statistics
    nn_model_info_t model_info;           // NN model information
    uint32_t frame_counter;               // Frame counter for interval processing
    uint32_t last_inference_tick;         // Tick of the last inference attempt (pacing)
    video_ai_result_callback_t result_callback; // Post-inference result callback
    void *result_callback_user_data;      // Opaque pointer for result_callback
    uint8_t *current_buffer;              // Current frame buffer
    aicam_bool_t is_initialized;          // Initialization status
    aicam_bool_t is_running;              // Running status
    aicam_bool_t draw_service_initialized; // Drawing service initialization status

    /* Secondary model (e.g. face detection) running on a downscaled copy of
     * the pipe2 frame. Loaded explicitly via
     * video_ai_node_load_secondary_model(); inference is serialized with the
     * primary model through the global NPU mutex inside nn.c. */
    nn_handle_t secondary_handle;         // Secondary NN instance (NULL if unloaded)
    nn_model_info_t secondary_model_info; // Secondary model information
    uint8_t *secondary_scratch;           // Face ROI buffer block (ROI_MAX entries)
    uint32_t secondary_scratch_size;      // Total ROI block size in bytes
    uint32_t secondary_roi_stride;        // Per-ROI buffer size in bytes
    aicam_bool_t secondary_enabled;       // Runtime gate for secondary inference
    video_ai_result_callback_t secondary_callback; // Secondary result callback
    void *secondary_callback_user_data;   // Opaque pointer for secondary_callback
    uint32_t secondary_inference_count;   // Secondary inference counter (stats)
    uint32_t secondary_error_count;       // Secondary inference errors (stats)

    /* Face ROI mailbox: the camera draw callback (sole legal PIPE1 holder)
     * copies person head ROIs while the buffer is locked; the AI node
     * thread consumes them for secondary inference. */
    osMutexId_t roi_mutex;                          // Guards count/meta/seq
    volatile uint32_t roi_seq;                      // Bumped on every publish
    uint32_t roi_consumed_seq;                      // Last sequence consumed
    volatile aicam_bool_t roi_wanted;               // Consumer wants new ROIs
    uint32_t roi_count;                             // ROIs in the mailbox
    float roi_meta[VIDEO_AI_FACE_ROI_MAX][4];       // x, y, w, h (full-frame normalized)

    // NN result cache (circular queue with 3 buffers)
    nn_result_with_frame_id_t nn_result_cache[NN_RESULT_CACHE_SIZE];
    uint32_t write_index;                   // Write index for circular queue
    uint32_t read_index;                    // Read index for circular queue
    uint32_t cache_count;                   // Number of items in cache (0-NN_RESULT_CACHE_SIZE)
    aicam_bool_t cache_initialized;
    osMutexId_t cache_mutex;
} video_ai_node_data_t;

/* ==================== API Functions ==================== */

/**
 * @brief Create AI node
 * @param name Node name
 * @param config AI configuration
 * @return AI node handle or NULL on error
 */
video_node_t* video_ai_node_create(const char *name, const video_ai_config_t *config);

/**
 * @brief Get default AI configuration
 * @param config Configuration structure to fill
 */
void video_ai_get_default_config(video_ai_config_t *config);

/**
 * @brief Set AI parameters
 * @param node AI node handle
 * @param config New configuration
 * @return Operation result
 */
aicam_result_t video_ai_node_set_config(video_node_t *node, const video_ai_config_t *config);

/**
 * @brief Get AI parameters
 * @param node AI node handle
 * @param config Configuration structure to fill
 * @return Operation result
 */
aicam_result_t video_ai_node_get_config(video_node_t *node, video_ai_config_t *config);

/**
 * @brief Get AI statistics
 * @param node AI node handle
 * @param stats Statistics structure to fill
 * @return Operation result
 */
aicam_result_t video_ai_node_get_stats(video_node_t *node, video_ai_stats_t *stats);

/**
 * @brief Reset AI statistics
 * @param node AI node handle
 * @return Operation result
 */
aicam_result_t video_ai_node_reset_stats(video_node_t *node);

/**
 * @brief Start AI processing
 * @param node AI node handle
 * @return Operation result
 */
aicam_result_t video_ai_node_start(video_node_t *node);

/**
 * @brief Stop AI processing
 * @param node AI node handle
 * @return Operation result
 */
aicam_result_t video_ai_node_stop(video_node_t *node);

/**
 * @brief Check if AI is running
 * @param node AI node handle
 * @return Running status
 */
aicam_bool_t video_ai_node_is_running(video_node_t *node);

/**
 * @brief Load NN model
 * @param node AI node handle
 * @param model_ptr Model pointer
 * @return Operation result
 */
aicam_result_t video_ai_node_load_model(video_node_t *node, uintptr_t model_ptr);

/**
 * @brief Unload NN model
 * @param node AI node handle
 * @return Operation result
 */
aicam_result_t video_ai_node_unload_model(video_node_t *node);

/**
 * @brief Get NN model information
 * @param node AI node handle
 * @param model_info Model info structure to fill
 * @return Operation result
 */
aicam_result_t video_ai_node_get_model_info(video_node_t *node, nn_model_info_t *model_info);

/**
 * @brief Reload AI model
 * @param node AI node handle
 * @return Operation result
 */
aicam_result_t video_ai_node_reload_model(video_node_t *node);

/**
 * @brief Get oldest NN result from cache (FIFO)
 * @param node AI node handle
 * @param result NN result structure to fill
 * @return Operation result
 */
aicam_result_t video_ai_node_get_nn_result(video_node_t *node, nn_result_t *result);

/**
 * @brief Get latest NN result from cache (newest, non-destructive)
 * @param node AI node handle
 * @param result NN result structure to fill
 * @return Operation result
 */
aicam_result_t video_ai_node_get_best_nn_result(video_node_t *node, nn_result_t *result, uint32_t frame_id);

/**
 * @brief Register a callback invoked after each successful inference
 * @param node AI node handle
 * @param callback Callback function (NULL to unregister)
 * @param user_data Opaque pointer passed to the callback
 * @return Operation result
 */
aicam_result_t video_ai_node_set_result_callback(video_node_t *node,
                                                 video_ai_result_callback_t callback,
                                                 void *user_data);

/* ==================== Secondary Model API ==================== */

/**
 * @brief Load a secondary model (e.g. face detection) on this AI node
 * @details The secondary model runs on a downscaled copy of the pipe2 frame
 *          right after each primary inference. The handle/scratch are owned
 *          by the node and released on unload/deinit.
 * @param node AI node handle
 * @param model_ptr Model package address in flash
 * @return Operation result
 */
aicam_result_t video_ai_node_load_secondary_model(video_node_t *node, uintptr_t model_ptr);

/**
 * @brief Unload the secondary model and release its resources
 * @param node AI node handle
 * @return Operation result
 */
aicam_result_t video_ai_node_unload_secondary_model(video_node_t *node);

/**
 * @brief Check whether a secondary model is loaded
 * @param node AI node handle
 * @return AICAM_TRUE when loaded
 */
aicam_bool_t video_ai_node_secondary_is_loaded(video_node_t *node);

/**
 * @brief Enable/disable secondary model inference at runtime
 * @param node AI node handle
 * @param enabled AICAM_TRUE to run the secondary model
 * @return Operation result
 */
aicam_result_t video_ai_node_set_secondary_enabled(video_node_t *node, aicam_bool_t enabled);

/**
 * @brief Set the secondary model postprocess thresholds at runtime
 * @param node AI node handle
 * @param confidence_pct Confidence threshold x100 (1-99)
 * @param nms_pct NMS/IoU threshold x100 (1-99)
 * @return Operation result (AICAM_ERROR_NOT_INITIALIZED when no model loaded)
 */
aicam_result_t video_ai_node_set_secondary_thresholds(video_node_t *node,
                                                      uint32_t confidence_pct,
                                                      uint32_t nms_pct);

/**
 * @brief Register the secondary model result callback
 * @param node AI node handle
 * @param callback Callback function (NULL to unregister)
 * @param user_data Opaque pointer passed to the callback
 * @return Operation result
 */
aicam_result_t video_ai_node_set_secondary_result_callback(video_node_t *node,
                                                            video_ai_result_callback_t callback,
                                                            void *user_data);

/**
 * @brief Get the secondary model information
 * @param node AI node handle
 * @param model_info Model info structure to fill (zeroed when not loaded)
 * @return AICAM_OK when loaded, AICAM_ERROR_NOT_INITIALIZED otherwise
 */
aicam_result_t video_ai_node_get_secondary_model_info(video_node_t *node, nn_model_info_t *model_info);

/**
 * @brief Publish person-anchored face ROIs copied from a PIPE1 frame
 * @details Called from the camera draw callback while the PIPE1 buffer is
 *          locked. For each person detection the head region (screen-square,
 *          clamped to the frame) is copied, converted RGB565 -> RGB888 and
 *          area-averaged into the model input size. A NULL/empty
 *          person_result publishes an empty set so the consumer clears
 *          stale faces. Copies happen only when the AI node asked for new
 *          ROIs (throttled to the secondary inference pace).
 * @param node AI node handle
 * @param fb PIPE1 frame buffer (RGB565)
 * @param fb_width Frame width in pixels
 * @param fb_height Frame height in pixels
 * @param person_result Primary OD result for THIS frame (NULL when none)
 * @return aicam_result_t
 */
aicam_result_t video_ai_node_push_face_rois(video_node_t *node, const uint8_t *fb,
                                            uint32_t fb_width, uint32_t fb_height,
                                            const nn_result_t *person_result);

/* ==================== Control Commands ==================== */

#define AI_CMD_START_PROCESSING           0x3001
#define AI_CMD_STOP_PROCESSING            0x3002
#define AI_CMD_SET_CONFIDENCE             0x3003
#define AI_CMD_SET_MAX_DETECTIONS         0x3004
#define AI_CMD_LOAD_MODEL                 0x3005
#define AI_CMD_UNLOAD_MODEL               0x3006
#define AI_CMD_GET_MODEL_INFO             0x3007
#define AI_CMD_ENABLE_DRAWING             0x3008
#define AI_CMD_DISABLE_DRAWING            0x3009
#define AI_CMD_SET_DRAW_CONFIG            0x300A
#define AI_CMD_GET_DRAW_CONFIG            0x300B

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_AI_NODE_H */