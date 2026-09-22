/**
 * @file video_ai_node.c
 * @brief Video AI Processing Node Implementation
 * @details AI inference node based on nn.h/nn.c device interaction with zero-copy
 */

#include "video_ai_node.h"
#include "video_frame_mgr.h"
#include "debug.h"
#include "buffer_mgr.h"
#include "nn.h"
#include "ai_draw_service.h"
#include "people_counting.h"
#include <string.h>
#include <stdio.h>
#include "mem_map.h"
#include "cmsis_os2.h"
#include "upgrade_manager.h"
#include "drtc.h"


/* ==================== Internal Function Declarations ==================== */
static aicam_result_t video_ai_node_load_model_active(video_node_t *node);
static aicam_result_t video_ai_node_init_callback(video_node_t *node);
static aicam_result_t video_ai_node_deinit_callback(video_node_t *node);
static aicam_result_t video_ai_node_process_callback(video_node_t *node,
                                                    video_frame_t **input_frames,
                                                    uint32_t input_count,
                                                    video_frame_t **output_frames,
                                                    uint32_t *output_count);
static aicam_result_t video_ai_node_control_callback(video_node_t *node,
                                                    uint32_t cmd,
                                                    void *param);

static aicam_result_t video_ai_start_device(video_ai_node_data_t *data);
static aicam_result_t video_ai_stop_device(video_ai_node_data_t *data);
static aicam_result_t video_ai_process_frame(video_ai_node_data_t *data,
                                            video_frame_t **output_frame);
static aicam_result_t video_ai_init_draw_service(video_ai_node_data_t *data);
static aicam_result_t video_ai_deinit_draw_service(video_ai_node_data_t *data);
static void video_ai_run_secondary_model(video_ai_node_data_t *data, uint32_t frame_id);


/* ==================== API Implementation ==================== */


void video_ai_get_default_config(video_ai_config_t *config) {
    if (!config) return;
    
    memset(config, 0, sizeof(video_ai_config_t));
    config->width = PIPE1_DEFAULT_WIDTH;
    config->height = PIPE1_DEFAULT_HEIGHT;
    config->fps = CAMERA_FPS;
    config->input_format = PIPE1_DEFAULT_FORMAT;
    config->bpp = PIPE1_DEFAULT_BPP;
    config->confidence_threshold = 50;
    config->nms_threshold = 50;
    config->max_detections = 32;
    config->processing_interval = 1;  // Process every frame
    config->enabled = AICAM_TRUE;
    config->overlay_results = AICAM_TRUE;  // Overlay on by default (stock behavior)
    config->enable_drawing = AICAM_TRUE;
    
    // Initialize drawing configuration
    ai_draw_get_default_config(&config->draw_config);
}

video_node_t* video_ai_node_create(const char *name, const video_ai_config_t *config) {
    if (!name || !config) {
        LOG_CORE_ERROR("Invalid parameters for AI node creation");
        return NULL;
    }
    
    // Create node
    video_node_t *node = video_node_create(name, VIDEO_NODE_TYPE_SOURCE);
    if (!node) {
        LOG_CORE_ERROR("Failed to create AI node");
        return NULL;
    }
    
    // Allocate private data
    video_ai_node_data_t *data = buffer_calloc(1, sizeof(video_ai_node_data_t));
    if (!data) {
        LOG_CORE_ERROR("Failed to allocate AI node data");
        video_node_destroy(node);
        return NULL;
    }
    
    // Initialize private data
    memset(data, 0, sizeof(video_ai_node_data_t));
    memcpy(&data->config, config, sizeof(video_ai_config_t));
    
    // Initialize NN result cache (circular queue)
    memset(data->nn_result_cache, 0, sizeof(data->nn_result_cache));
    data->write_index = 0;
    data->read_index = 0;
    data->cache_count = 0;
    data->cache_initialized = AICAM_FALSE;
    
    // Create cache mutex
    data->cache_mutex = osMutexNew(NULL);
    if (!data->cache_mutex) {
        LOG_CORE_ERROR("Failed to create cache mutex");
        buffer_free(data);
        video_node_destroy(node);
        return NULL;
    }
    
    // Set node callbacks
    video_node_callbacks_t callbacks = {
        .init = video_ai_node_init_callback,
        .deinit = video_ai_node_deinit_callback,
        .process = video_ai_node_process_callback,
        .control = video_ai_node_control_callback
    };
    
    aicam_result_t result = video_node_set_callbacks(node, &callbacks);
    if (result != AICAM_OK) {
        LOG_CORE_ERROR("Failed to set AI node callbacks");
        buffer_free(data);
        video_node_destroy(node);
        return NULL;
    }
    
    // Set private data
    result = video_node_set_private_data(node, data);
    if (result != AICAM_OK) {
        LOG_CORE_ERROR("Failed to set AI node private data");
        buffer_free(data);
        video_node_destroy(node);
        return NULL;
    }
    
    LOG_CORE_INFO("AI node created: %s", name);
    return node;
}



aicam_result_t video_ai_node_set_config(video_node_t *node, const video_ai_config_t *config) {
    if (!node || !config) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    memcpy(&data->config, config, sizeof(video_ai_config_t));

    
    LOG_CORE_INFO("AI node config updated");
    return AICAM_OK;
}

aicam_result_t video_ai_node_get_config(video_node_t *node, video_ai_config_t *config) {
    if (!node || !config) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    memcpy(config, &data->config, sizeof(video_ai_config_t));
    return AICAM_OK;
}

aicam_result_t video_ai_node_get_stats(video_node_t *node, video_ai_stats_t *stats) {
    if (!node || !stats) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    memcpy(stats, &data->stats, sizeof(video_ai_stats_t));
    return AICAM_OK;
}

aicam_result_t video_ai_node_reset_stats(video_node_t *node) {
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    memset(&data->stats, 0, sizeof(video_ai_stats_t));
    LOG_CORE_INFO("AI node statistics reset");
    return AICAM_OK;
}

aicam_result_t video_ai_node_start(video_node_t *node) {
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    if (data->is_running) {
        LOG_CORE_WARN("AI node already running");
        return AICAM_OK;
    }
    
    aicam_result_t result = video_ai_start_device(data);
    if (result == AICAM_OK) {
        data->is_running = AICAM_TRUE;
        LOG_CORE_INFO("AI node started");
    }
    
    return result;
}

aicam_result_t video_ai_node_stop(video_node_t *node) {
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    if (!data->is_running) {
        LOG_CORE_WARN("AI node not running");
        return AICAM_OK;
    }
    
    aicam_result_t result = video_ai_stop_device(data);
    if (result == AICAM_OK) {
        data->is_running = AICAM_FALSE;
        LOG_CORE_INFO("AI node stopped");
    }
    
    return result;
}

aicam_bool_t video_ai_node_is_running(video_node_t *node) {
    if (!node) {
        return AICAM_FALSE;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_FALSE;
    }
    
    return data->is_running;
}

aicam_result_t video_ai_node_load_model(video_node_t *node, uintptr_t model_ptr) {
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    if(model_ptr == 0) {
        LOG_CORE_INFO("Load active AI model");
        aicam_result_t result = video_ai_node_load_model_active(node);
        if (result != AICAM_OK) {
            LOG_CORE_ERROR("Failed to load active AI model: %d", result);
            return result;
        }
        return AICAM_OK;
    }
    
    int nn_ret = nn_load_model(model_ptr);
    if (nn_ret == 0) {
        // Get model information
        nn_get_model_info(&data->model_info);
        LOG_CORE_INFO("AI model loaded: %s", data->model_info.name);
        json_config_sync_ai_pipe_nvs_from_input_size(data->model_info.input_width, data->model_info.input_height);
        return AICAM_OK;
    } else {
        LOG_CORE_ERROR("Failed to load AI model: %d", nn_ret);
        return AICAM_ERROR;
    }
}

aicam_result_t video_ai_node_unload_model(video_node_t *node) {
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    int nn_ret = nn_unload_model();
    if (nn_ret == 0) {
        memset(&data->model_info, 0, sizeof(nn_model_info_t));
        LOG_CORE_INFO("AI model unloaded");
        return AICAM_OK;
    } else {
        LOG_CORE_ERROR("Failed to unload AI model: %d", nn_ret);
        return AICAM_ERROR;
    }
}

aicam_result_t video_ai_node_get_model_info(video_node_t *node, nn_model_info_t *model_info) {
    if (!node || !model_info) {
        LOG_CORE_ERROR("Invalid parameters for AI node get model info");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        LOG_CORE_ERROR("Invalid parameters for AI node get model info");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    memcpy(model_info, &data->model_info, sizeof(nn_model_info_t));
    return AICAM_OK;
}

aicam_result_t video_ai_node_reload_model(video_node_t *node) {
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    aicam_result_t nn_ret = video_ai_node_unload_model(node);
    if (nn_ret != AICAM_OK) {
        LOG_CORE_ERROR("Failed to unload AI model: %d", nn_ret);
        return nn_ret;
    }

    //reset cache   
    memset(data->nn_result_cache, 0, sizeof(data->nn_result_cache));
    data->write_index = 0;
    data->read_index = 0;
    data->cache_count = 0;
    data->cache_initialized = AICAM_FALSE;

    nn_ret = video_ai_node_load_model(node, 0);
    if (nn_ret != AICAM_OK) {
        LOG_CORE_ERROR("Failed to load AI model: %d", nn_ret);
        return nn_ret;
    }

    LOG_CORE_INFO("AI model reloaded");

    video_ai_node_init_callback(node);

    return AICAM_OK;
}

aicam_result_t video_ai_node_get_nn_result(video_node_t *node, nn_result_t *result) {
    if (!node || !result) {
        LOG_CORE_ERROR("Invalid parameters for AI node get nn result");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        LOG_CORE_ERROR("Invalid AI node data");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // Lock cache mutex
    osStatus_t mutex_status = osMutexAcquire(data->cache_mutex, osWaitForever); 
    if (mutex_status != osOK) {
        LOG_CORE_ERROR("Failed to lock cache mutex: %d", mutex_status);
        return AICAM_ERROR;
    }
    
    // Check if cache is empty
    if (data->cache_count == 0) {
        // Release mutex first
        osMutexRelease(data->cache_mutex);
        LOG_CORE_DEBUG("NN result cache is empty");
        result->od.nb_detect = 0;
        return AICAM_OK;
    }
    
    // Copy the oldest result (FIFO) - extract nn_result_t from nn_result_with_frame_id_t
    memcpy(result, &data->nn_result_cache[data->read_index].result, sizeof(nn_result_t));
    
    // Move read index (circular)
    data->read_index = (data->read_index + 1) % NN_RESULT_CACHE_SIZE;
    data->cache_count--;
    
    // Release cache mutex
    osMutexRelease(data->cache_mutex);
    
    LOG_CORE_DEBUG("Retrieved NN result from cache: %d detections", result->od.nb_detect);
    return AICAM_OK;
}

aicam_result_t video_ai_node_get_best_nn_result(video_node_t *node, nn_result_t *result, uint32_t frame_id) {
    if (!node || !result) {
        LOG_CORE_ERROR("Invalid parameters for AI node get latest nn result");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        LOG_CORE_ERROR("Invalid AI node data");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // Lock cache mutex
    osStatus_t mutex_status = osMutexAcquire(data->cache_mutex, osWaitForever); 
    if (mutex_status != osOK) {
        LOG_CORE_ERROR("Failed to lock cache mutex: %d", mutex_status);
        return AICAM_ERROR;
    }
    
    // Check if cache is empty
    if (data->cache_count == 0) {
        // Release mutex first
        osMutexRelease(data->cache_mutex);
        LOG_CORE_DEBUG("NN result cache is empty");
        result->od.nb_detect = 0;
        return AICAM_OK;
    }

    uint32_t best_index = 0;
    uint32_t min_diff = UINT32_MAX;

    for (uint32_t i = 0; i < data->cache_count; i++)
    {
        uint32_t index = (data->write_index + NN_RESULT_CACHE_SIZE - data->cache_count + i) % NN_RESULT_CACHE_SIZE;
        nn_result_with_frame_id_t *entry = &data->nn_result_cache[index];

        uint32_t diff = (entry->frame_id > frame_id) ? (entry->frame_id - frame_id) : (frame_id - entry->frame_id);

        if (diff < min_diff)
        {
            min_diff = diff;
            best_index = index;
        }
    }

    // Copy best match result
    memcpy(result, &data->nn_result_cache[best_index].result, sizeof(nn_result_t));

    // Release cache mutex (no index changes for non-destructive read)
    osMutexRelease(data->cache_mutex);

    //LOG_CORE_DEBUG("Retrieved latest NN result from cache: %d detections", result->od.nb_detect);
    return AICAM_OK;
}

aicam_result_t video_ai_node_set_result_callback(video_node_t *node,
                                                 video_ai_result_callback_t callback,
                                                 void *user_data)
{
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    data->result_callback = callback;
    data->result_callback_user_data = user_data;
    return AICAM_OK;
}

/* ==================== Secondary Model API Implementation ==================== */

aicam_result_t video_ai_node_load_secondary_model(video_node_t *node, uintptr_t model_ptr)
{
    if (!node || model_ptr == 0) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    if (data->secondary_handle) {
        LOG_CORE_WARN("Secondary model already loaded");
        return AICAM_OK;
    }

    nn_handle_t handle = nn_instance_create();
    if (!handle) {
        LOG_CORE_ERROR("Failed to create secondary NN instance");
        return AICAM_ERROR_NO_MEMORY;
    }

    if (nn_instance_load_model(handle, model_ptr) != 0) {
        LOG_CORE_ERROR("Failed to load secondary model from %p", model_ptr);
        nn_instance_destroy(handle);
        return AICAM_ERROR;
    }

    nn_model_info_t info;
    if (nn_instance_get_model_info(handle, &info) != 0) {
        LOG_CORE_ERROR("Failed to get secondary model info");
        nn_instance_unload_model(handle);
        nn_instance_destroy(handle);
        return AICAM_ERROR;
    }

    /* Size the scratch from the compiled network's real input buffer — the
     * authoritative source. The JSON metadata can disagree when a different
     * model variant is flashed (e.g. float-input or another resolution),
     * which would otherwise fail EVERY inference with
     * "input_buffer_size[0] != input_size". */
    uint8_t *model_input = NULL;
    uint32_t input_buffer_size = 0;
    if (nn_instance_get_model_input_buffer(handle, &model_input, &input_buffer_size) != 0 ||
        input_buffer_size == 0) {
        LOG_CORE_ERROR("Failed to query secondary model input buffer");
        nn_instance_unload_model(handle);
        nn_instance_destroy(handle);
        return AICAM_ERROR;
    }

    uint32_t expected_size = info.input_width * info.input_height * 3;
    if (info.input_channels != 3 || input_buffer_size != expected_size) {
        LOG_CORE_ERROR("Secondary model input mismatch: network wants %lu bytes, "
                       "%lux%lux3 RGB888 uint8 expects %lu — wrong model variant flashed?",
                       (unsigned long)input_buffer_size,
                       (unsigned long)info.input_width, (unsigned long)info.input_height,
                       (unsigned long)expected_size);
        nn_instance_unload_model(handle);
        nn_instance_destroy(handle);
        return AICAM_ERROR_INVALID_DATA;
    }

    /* ROI buffer block: one model-input-sized RGB888 buffer per ROI */
    uint32_t roi_stride = input_buffer_size;
    uint32_t scratch_size = roi_stride * VIDEO_AI_FACE_ROI_MAX;
    uint8_t *scratch = hal_mem_alloc_large(scratch_size);
    if (!scratch) {
        LOG_CORE_ERROR("Failed to allocate secondary ROI buffers (%u bytes)", scratch_size);
        nn_instance_unload_model(handle);
        nn_instance_destroy(handle);
        return AICAM_ERROR_NO_MEMORY;
    }
    osMutexId_t roi_mutex = osMutexNew(NULL);
    if (!roi_mutex) {
        LOG_CORE_ERROR("Failed to create secondary ROI mutex");
        hal_mem_free(scratch);
        nn_instance_unload_model(handle);
        nn_instance_destroy(handle);
        return AICAM_ERROR;
    }

    data->secondary_handle = handle;
    data->secondary_scratch = scratch;
    data->secondary_scratch_size = scratch_size;
    data->secondary_roi_stride = roi_stride;
    data->roi_mutex = roi_mutex;
    data->roi_seq = 0;
    data->roi_consumed_seq = 0;
    data->roi_wanted = AICAM_TRUE;
    data->roi_count = 0;
    memset(data->roi_meta, 0, sizeof(data->roi_meta));
    memcpy(&data->secondary_model_info, &info, sizeof(nn_model_info_t));
    data->secondary_enabled = AICAM_TRUE;

    LOG_CORE_INFO("Secondary model loaded: %s %ux%ux%u from %p (PIPE1 ROI mailbox mode)",
                  info.name, info.input_width, info.input_height, info.input_channels,
                  model_ptr);
    return AICAM_OK;
}

aicam_result_t video_ai_node_unload_secondary_model(video_node_t *node)
{
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    if (!data->secondary_handle) {
        return AICAM_OK;
    }

    /* The AI node thread reads the handle every frame; only unload while the
     * node is not processing (callers stop the pipeline first). */
    nn_handle_t handle = data->secondary_handle;
    data->secondary_enabled = AICAM_FALSE;
    data->secondary_handle = NULL; /* detach before freeing resources */
    if (data->secondary_scratch) {
        hal_mem_free(data->secondary_scratch);
        data->secondary_scratch = NULL;
        data->secondary_scratch_size = 0;
        data->secondary_roi_stride = 0;
    }
    if (data->roi_mutex) {
        osMutexDelete(data->roi_mutex);
        data->roi_mutex = NULL;
    }
    memset(&data->secondary_model_info, 0, sizeof(nn_model_info_t));
    if (handle) {
        nn_instance_unload_model(handle);
        nn_instance_destroy(handle);
    }
    return AICAM_OK;
}

aicam_bool_t video_ai_node_secondary_is_loaded(video_node_t *node)
{
    if (!node) {
        return AICAM_FALSE;
    }
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    return (data && data->secondary_handle) ? AICAM_TRUE : AICAM_FALSE;
}

aicam_result_t video_ai_node_set_secondary_enabled(video_node_t *node, aicam_bool_t enabled)
{
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (enabled && !data->secondary_handle) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }
    if (enabled) {
        /* Fresh start for the fail-fast counter when (re-)enabling */
        data->secondary_error_count = 0;
    }
    data->secondary_enabled = enabled;
    return AICAM_OK;
}

aicam_result_t video_ai_node_set_secondary_thresholds(video_node_t *node,
                                                      uint32_t confidence_pct,
                                                      uint32_t nms_pct)
{
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!data->secondary_handle) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }
    nn_instance_set_confidence_threshold(data->secondary_handle,
                                         (float)confidence_pct / 100.0f);
    nn_instance_set_nms_threshold(data->secondary_handle,
                                  (float)nms_pct / 100.0f);
    return AICAM_OK;
}

aicam_result_t video_ai_node_set_secondary_result_callback(video_node_t *node,
                                                            video_ai_result_callback_t callback,
                                                            void *user_data)
{
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    data->secondary_callback = callback;
    data->secondary_callback_user_data = user_data;
    return AICAM_OK;
}

aicam_result_t video_ai_node_get_secondary_model_info(video_node_t *node, nn_model_info_t *model_info)
{
    if (!node || !model_info) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data || !data->secondary_handle) {
        memset(model_info, 0, sizeof(nn_model_info_t));
        return AICAM_ERROR_NOT_INITIALIZED;
    }
    memcpy(model_info, &data->secondary_model_info, sizeof(nn_model_info_t));
    return AICAM_OK;
}

/* ==================== Internal Functions ==================== */

static aicam_result_t video_ai_start_device(video_ai_node_data_t *data) {
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // Check NN state
    nn_state_t nn_state = nn_get_state();
    if (nn_state == NN_STATE_READY) {
        // Start NN inference
        int nn_ret = nn_start_inference();
        if (nn_ret != 0) {
            LOG_CORE_ERROR("Failed to start NN inference: %d", nn_ret);
            return AICAM_ERROR;
        }
        //LOG_CORE_INFO("NN inference started");
        data->is_running = AICAM_TRUE;
    } else if (nn_state == NN_STATE_UNINIT) {
        LOG_CORE_WARN("NN module not initialized, AI will work in pass-through mode");
    } else {
        LOG_CORE_INFO("NN state: %d", nn_state);
    }
    
    return AICAM_OK;
}

static aicam_result_t video_ai_stop_device(video_ai_node_data_t *data) {
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    LOG_CORE_INFO("AI node stop device");

    // if(!data->config.enabled) {
    //     return AICAM_OK;
    // }

    //unload model
    nn_state_t nn_state = nn_get_state();
    
    // Stop NN inference
    if (nn_state == NN_STATE_RUNNING) {
        int nn_ret = nn_stop_inference();
        if (nn_ret != 0) {
            LOG_CORE_ERROR("Failed to stop NN inference: %d", nn_ret);
            return AICAM_ERROR;
        }
        LOG_CORE_INFO("NN inference stopped");
    }

    // nn_state = nn_get_state(); 
    // if(nn_state == NN_STATE_RUNNING || nn_state == NN_STATE_READY) {
    //     nn_unload_model();
    // }
    
    return AICAM_OK;
}

/**
 * @brief Bilinear-resampled copy of an RGB565 sub-image into an RGB888 buffer
 * @details Used on the camera thread to lift person head ROIs out of the
 *          locked PIPE1 framebuffer. Cost is O(dst) regardless of ROI size —
 *          the previous area-average version was O(src) and cost ~5 ms per
 *          large ROI, visibly dropping the encoded stream fps. (DMA2D could
 *          do the crop + RGB565->RGB888 via M2M+PFC but has no scaler, so
 *          the resample stays on the CPU.)
 */
static void video_ai_copy_roi_rgb565_to_rgb888(const uint16_t *src, uint32_t src_stride,
                                               uint32_t roi_x, uint32_t roi_y,
                                               uint32_t roi_w, uint32_t roi_h,
                                               uint8_t *dst, uint32_t dst_w, uint32_t dst_h)
{
    static const uint32_t shift[3] = {11, 5, 0};
    static const uint32_t mask[3] = {0x1Fu, 0x3Fu, 0x1Fu};
    static const uint32_t expand[3] = {255u / 31u, 255u / 63u, 255u / 31u};

    for (uint32_t dy = 0; dy < dst_h; dy++) {
        float fy = ((float)dy + 0.5f) * (float)roi_h / (float)dst_h - 0.5f;
        if (fy < 0.0f) fy = 0.0f;
        if (fy > (float)roi_h - 1.0f) fy = (float)roi_h - 1.0f;
        uint32_t y0 = (uint32_t)fy;
        uint32_t y1 = (y0 + 1 < roi_h) ? y0 + 1 : y0;
        float wy = fy - (float)y0;
        const uint16_t *row0 = src + (size_t)(roi_y + y0) * src_stride + roi_x;
        const uint16_t *row1 = src + (size_t)(roi_y + y1) * src_stride + roi_x;
        uint8_t *out = dst + (size_t)dy * dst_w * 3;

        for (uint32_t dx = 0; dx < dst_w; dx++) {
            float fx = ((float)dx + 0.5f) * (float)roi_w / (float)dst_w - 0.5f;
            if (fx < 0.0f) fx = 0.0f;
            if (fx > (float)roi_w - 1.0f) fx = (float)roi_w - 1.0f;
            uint32_t x0 = (uint32_t)fx;
            uint32_t x1 = (x0 + 1 < roi_w) ? x0 + 1 : x0;
            float wx = fx - (float)x0;

            uint32_t p00 = row0[x0], p01 = row0[x1];
            uint32_t p10 = row1[x0], p11 = row1[x1];
            /* Bilinear per 5/6-bit channel, expanded to 8-bit */
            for (uint32_t c = 0; c < 3; c++) {
                float v00 = (float)((p00 >> shift[c]) & mask[c]);
                float v01 = (float)((p01 >> shift[c]) & mask[c]);
                float v10 = (float)((p10 >> shift[c]) & mask[c]);
                float v11 = (float)((p11 >> shift[c]) & mask[c]);
                float top = v00 + (v01 - v00) * wx;
                float bot = v10 + (v11 - v10) * wx;
                float v = (top + (bot - top) * wy) * (float)expand[c] + 0.5f;
                *out++ = (uint8_t)v;
            }
        }
    }
}

/**
 * @brief Publish person-anchored face ROIs copied from a PIPE1 frame
 * @details Camera-thread side of the ROI mailbox; see video_ai_node.h. For
 *          each person detection the head region (screen-square in PIPE1
 *          pixels, clamped to the frame) is copied + converted into one
 *          model-input buffer. A NULL/empty person_result publishes an
 *          empty set so the consumer clears stale faces.
 */
aicam_result_t video_ai_node_push_face_rois(video_node_t *node, const uint8_t *fb,
                                            uint32_t fb_width, uint32_t fb_height,
                                            const nn_result_t *person_result)
{
    if (!node || !fb || fb_width == 0 || fb_height == 0) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data || !data->secondary_handle || !data->secondary_scratch || !data->roi_mutex) {
        return AICAM_OK; /* secondary model not loaded: nothing to feed */
    }
    if (!data->roi_wanted) {
        return AICAM_OK; /* consumer has not finished the previous batch */
    }

    const nn_model_info_t *info = &data->secondary_model_info;
    uint32_t dw = info->input_width;
    uint32_t dh = info->input_height;

    uint32_t count = 0;
    float meta[VIDEO_AI_FACE_ROI_MAX][4];

    if (person_result && person_result->is_valid && person_result->type == PP_TYPE_OD) {
        uint32_t nb = person_result->od.nb_detect;
        for (uint32_t i = 0; i < nb && count < VIDEO_AI_FACE_ROI_MAX; i++) {
            const od_detect_t *p = &person_result->od.detects[i];
            if (p->width <= 0.0f || p->height <= 0.0f) {
                continue;
            }

            /* Head ROI: square in PIPE1 pixels, a bit wider than the person
             * box, anchored at its top edge where the face is. The square is
             * clamped to the frame (staying square) so close persons whose
             * head square would overflow still get an inference. */
            float side = p->width * (float)fb_width * 1.15f;
            if (side > (float)fb_height) side = (float)fb_height;
            if (side > (float)fb_width) side = (float)fb_width;
            if (side < 16.0f) {
                continue; /* too small to plausibly contain a face */
            }
            /* Motion compensation: the person box is 1+ inference cycles old
             * while this PIPE1 frame is current — shift the crop to where
             * the person is now using the tracker's velocity estimate. */
            float mdx = 0.0f, mdy = 0.0f;
            people_counting_motion_compensate(p->x + p->width * 0.5f,
                                              p->y + p->height * 0.5f,
                                              &mdx, &mdy);
            float cx = (p->x + p->width * 0.5f) * (float)fb_width + mdx * (float)fb_width;
            float x0 = cx - side * 0.5f;
            float y0 = (p->y + mdy) * (float)fb_height - side * 0.05f;
            if (x0 < 0.0f) x0 = 0.0f;
            if (y0 < 0.0f) y0 = 0.0f;
            if (x0 + side > (float)fb_width) x0 = (float)fb_width - side;
            if (y0 + side > (float)fb_height) y0 = (float)fb_height - side;

            uint32_t rx = (uint32_t)x0;
            uint32_t ry = (uint32_t)y0;
            uint32_t rs = (uint32_t)side;
            if (rs < 8 || rx + rs > fb_width || ry + rs > fb_height) {
                continue;
            }

            video_ai_copy_roi_rgb565_to_rgb888((const uint16_t *)fb, fb_width,
                                               rx, ry, rs, rs,
                                               data->secondary_scratch +
                                                   (size_t)count * data->secondary_roi_stride,
                                               dw, dh);
            meta[count][0] = (float)rx / (float)fb_width;
            meta[count][1] = (float)ry / (float)fb_height;
            meta[count][2] = (float)rs / (float)fb_width;
            meta[count][3] = (float)rs / (float)fb_height;
            count++;
        }
    }

    if (osMutexAcquire(data->roi_mutex, 20) == osOK) {
        memcpy(data->roi_meta, meta, sizeof(meta));
        data->roi_count = count;
        data->roi_seq++;
        data->roi_wanted = AICAM_FALSE;
        osMutexRelease(data->roi_mutex);
    }
    return AICAM_OK;
}

/**
 * @brief Run the face (secondary) model on the PIPE1 ROI mailbox
 * @details AI-node-thread side of the mailbox: the camera draw callback
 *          copies person head ROIs from the locked PIPE1 buffer at full
 *          resolution. Each ROI is inferred and face boxes are mapped from
 *          ROI-local normalized coordinates back to full-frame normalized
 *          ones. One merged result goes to the callback; an empty/invalid
 *          result (no persons, or the draw callback stopped) clears
 *          consumer state.
 */
#define SECONDARY_MAX_FACES 8

static void video_ai_run_secondary_model(video_ai_node_data_t *data, uint32_t frame_id)
{
    const nn_model_info_t *info = &data->secondary_model_info;
    if (!data->secondary_scratch || !data->roi_mutex || !data->secondary_handle) {
        return;
    }
    if (info->input_width == 0 || info->input_height == 0 || info->input_channels != 3) {
        return;
    }

    /* Take the latest mailbox batch. Nothing new -> return and keep the
     * current faces; an EMPTY fresh batch falls through and publishes an
     * empty result so consumers clear stale faces. */
    float meta[VIDEO_AI_FACE_ROI_MAX][4];
    uint32_t roi_count = 0;
    aicam_bool_t fresh_batch = AICAM_FALSE;
    if (osMutexAcquire(data->roi_mutex, 20) == osOK) {
        if (data->roi_seq != data->roi_consumed_seq) {
            fresh_batch = AICAM_TRUE;
            roi_count = data->roi_count;
            memcpy(meta, data->roi_meta, sizeof(meta));
            data->roi_consumed_seq = data->roi_seq;
        }
        osMutexRelease(data->roi_mutex);
    }
    if (!fresh_batch) {
        return;
    }

    /* Merged face output (AI node thread only; mpe_detect_t is large) */
    static mpe_detect_t merged[SECONDARY_MAX_FACES];
    uint32_t merged_count = 0;
    uint32_t total_time_ms = 0;

    for (uint32_t r = 0; r < roi_count; r++) {
        uint8_t *input = data->secondary_scratch + (size_t)r * data->secondary_roi_stride;

        nn_result_t result;
        memset(&result, 0, sizeof(nn_result_t));
        uint32_t start_tick = osKernelGetTickCount();
        if (nn_instance_inference_frame(data->secondary_handle, input,
                                        data->secondary_roi_stride, &result) == 0) {
            data->secondary_inference_count++;
            data->secondary_error_count = 0;
            total_time_ms += osKernelGetTickCount() - start_tick;

            if (result.is_valid && result.type == PP_TYPE_MPE) {
                uint32_t faces = result.mpe.nb_detect;
                for (uint32_t f = 0; f < faces && merged_count < SECONDARY_MAX_FACES; f++) {
                    const mpe_detect_t *d = &result.mpe.detects[f];
                    mpe_detect_t *out = &merged[merged_count++];
                    memset(out, 0, sizeof(mpe_detect_t));
                    /* ROI-local normalized -> full-frame normalized */
                    out->x = meta[r][0] + d->x * meta[r][2];
                    out->y = meta[r][1] + d->y * meta[r][3];
                    out->width = d->width * meta[r][2];
                    out->height = d->height * meta[r][3];
                    out->conf = d->conf;
                    out->class_name = d->class_name;
                    if (out->x < 0.0f) out->x = 0.0f;
                    if (out->y < 0.0f) out->y = 0.0f;
                    if (out->x > 1.0f) out->x = 1.0f;
                    if (out->y > 1.0f) out->y = 1.0f;
                    if (out->width > 1.0f - out->x) out->width = 1.0f - out->x;
                    if (out->height > 1.0f - out->y) out->height = 1.0f - out->y;
                }
            }
        } else {
            /* Fail fast instead of letting the driver error spam every frame */
            if (++data->secondary_error_count >= 3) {
                data->secondary_enabled = AICAM_FALSE;
                LOG_CORE_ERROR("Secondary inference failed %lu times, disabled until model reload",
                               (unsigned long)data->secondary_error_count);
                return;
            }
        }
    }

    /* Ask the camera thread for a fresh ROI batch — after inference, so the
     * buffers are never overwritten mid-run */
    data->roi_wanted = AICAM_TRUE;

    /* Single merged delivery; invalid when no faces so consumers clear state */
    nn_result_t out;
    memset(&out, 0, sizeof(nn_result_t));
    out.type = PP_TYPE_MPE;
    out.is_valid = (merged_count > 0) ? 1 : 0;
    out.mpe.detects = merged;
    out.mpe.nb_detect = (uint8_t)merged_count;
    if (data->secondary_callback) {
        data->secondary_callback(&out, frame_id, total_time_ms,
                                 data->secondary_callback_user_data);
    }
}
#undef SECONDARY_MAX_FACES

static aicam_result_t video_ai_process_frame(video_ai_node_data_t *data,
                                             video_frame_t **output_frame)
{
    if (!data || !output_frame)
    {
        return AICAM_ERROR_INVALID_PARAM;
    }

    // Pace inference to the configured interval. Sleep in bounded slices so
    // config changes and pipeline stop are picked up within ~100 ms, and
    // never inside a camera ioctl (it holds the camera mutex while blocking).
    if (data->config.inference_interval_ms != 0)
    {
        uint32_t elapsed_ms = osKernelGetTickCount() - data->last_inference_tick;
        if (elapsed_ms < data->config.inference_interval_ms)
        {
            uint32_t remaining_ms = data->config.inference_interval_ms - elapsed_ms;
            osDelay(remaining_ms < 100 ? remaining_ms : 100);
            *output_frame = NULL;
            return AICAM_OK;
        }
    }

    // Check processing interval
    if (data->config.processing_interval > 1)
    {
        data->frame_counter++;
        if (data->frame_counter % data->config.processing_interval != 0)
        {
            // Skip this frame
            *output_frame = NULL;
            data->stats.frames_skipped++;
            return AICAM_OK;
        }
    }

    // Check NN state
    nn_state_t nn_state = nn_get_state();
    if (nn_state != NN_STATE_READY && nn_state != NN_STATE_RUNNING)
    {
        LOG_CORE_WARN("NN not ready (state=%d), passing through frame", nn_state);
        *output_frame = NULL;
        data->stats.frames_skipped++;
        return AICAM_OK;
    }

    // get input buffer from pipe2 (use cached device handle)
    device_t *camera_dev = device_find_pattern(CAMERA_DEVICE_NAME, DEV_TYPE_VIDEO);
    if (!camera_dev)
    {
        LOG_CORE_ERROR("Camera device not found");
        *output_frame = NULL;
        return AICAM_ERROR;
    }

    uint8_t *input_frame_buffer = NULL;
    uint32_t frame_id = 0;
    camera_buffer_with_frame_id_t camera_buffer_with_frame_id = {0};
    int result = device_ioctl(camera_dev, CAM_CMD_GET_PIPE2_BUFFER_WITH_FRAME_ID,
                            (uint8_t *)&camera_buffer_with_frame_id, 0);


    if (result == AICAM_OK)
    {
        // Reduce logging frequency - only log every 200 frames
        input_frame_buffer = camera_buffer_with_frame_id.buffer;
        frame_id = camera_buffer_with_frame_id.frame_id;
        static uint32_t frame_log_count = 0;
        if ((++frame_log_count % 200) == 1)
        {
            LOG_CORE_DEBUG("Got pipe2 buffer for AI processing: %p (frame %u)", input_frame_buffer, frame_log_count);
        }
    }
    else if (result == AICAM_ERROR_NOT_FOUND)
    {
        *output_frame = NULL;
        return AICAM_OK;
    }
    else if (result == AICAM_ERROR_NOT_SUPPORTED)
    {
        /* Camera pipes are stopped (e.g. an interrupted OTA upload stopped the
         * device and its cleanup path never ran). That is a persistent state
         * problem, not a per-frame failure: anchor the pacing tick + yield so
         * the node thread cannot busy-spin, log rate-limited (once per 5 s),
         * and report OK-no-frame instead of erroring so the pipeline error log
         * is not flooded at the retry rate. ai_pipeline_start() normally
         * restarts the camera before this state can be observed. */
        static uint32_t pipe_stopped_log_tick = 0;
        uint32_t now = osKernelGetTickCount();
        if (pipe_stopped_log_tick == 0 || (now - pipe_stopped_log_tick) >= 5000U) {
            pipe_stopped_log_tick = now;
            LOG_CORE_ERROR("Camera pipe2 stopped, AI inference idle until camera restart");
        }
        data->last_inference_tick = now;
        osDelay(5);
        *output_frame = NULL;
        data->stats.frames_skipped++;
        return AICAM_OK;
    }
    else
    {
        LOG_CORE_ERROR("Failed to get pipe2 buffer for AI processing, size: %d", camera_buffer_with_frame_id.size);
        // Camera pipe not producing (stopped/deinit'ed): yield so the node thread
        // cannot busy-spin against a dead camera, and anchor the pacing tick so
        // retries keep the configured inference cadence instead of bypassing it
        // (the gate at the top of this function only sleeps after an anchor).
        data->last_inference_tick = osKernelGetTickCount();
        osDelay(5);
        *output_frame = NULL;
        return AICAM_ERROR;
    }

    // Prepare NN result structure (use stack allocation for temporary result)
    nn_result_t nn_result;
    memset(&nn_result, 0, sizeof(nn_result_t));


    // Call NN module to process frame
    uint32_t infer_start_tick = osKernelGetTickCount();
    int nn_ret = nn_inference_frame(input_frame_buffer, camera_buffer_with_frame_id.size, &nn_result);
    uint32_t inference_time_ms = osKernelGetTickCount() - infer_start_tick;
    // Anchor pacing to the attempt so inference errors keep the configured cadence
    data->last_inference_tick = infer_start_tick;

    // Secondary model (face detection) on PIPE1 ROIs published by the
    // camera draw callback; NPU access is serialized with the primary model
    // through the global NPU mutex inside nn.c.
    if (data->secondary_handle && data->secondary_enabled) {
        video_ai_run_secondary_model(data, frame_id);
    }

    // return pipe2 buffer
    device_ioctl(camera_dev, CAM_CMD_RETURN_PIPE2_BUFFER, input_frame_buffer, 0);

    // Save result to cache if inference was successful
    if (nn_ret == 0)
    {
        // Optimized cache write with reduced lock time
        uint32_t current_write_idx = data->write_index;

        // Prepare result with timestamp and frame_id
        nn_result_with_frame_id_t result_with_ts;
        memcpy(&result_with_ts.result, &nn_result, sizeof(nn_result_t));
        result_with_ts.frame_id = frame_id;

        // Copy result to cache before acquiring lock (reduce critical section)
        memcpy(&data->nn_result_cache[current_write_idx], &result_with_ts, sizeof(nn_result_with_frame_id_t));

        // Lock cache mutex for minimal time
        osStatus_t mutex_status = osMutexAcquire(data->cache_mutex, 10); // Reduced timeout
        if (mutex_status == osOK)
        {
            // Update indices atomically
            data->write_index = (data->write_index + 1) % NN_RESULT_CACHE_SIZE;

            if (data->cache_count >= NN_RESULT_CACHE_SIZE)
            {
                data->read_index = (data->read_index + 1) % NN_RESULT_CACHE_SIZE;
            }
            else
            {
                data->cache_count++;
            }

            if (!data->cache_initialized)
            {
                data->cache_initialized = AICAM_TRUE;
            }

            osMutexRelease(data->cache_mutex);
        }
        else
        {
            // If lock fails, just log once to avoid spam
            static uint32_t lock_fail_count = 0;
            if ((++lock_fail_count % 100) == 1)
            {
                LOG_CORE_WARN("Cache mutex timeout, failed %lu times", lock_fail_count);
            }
        }

        // Hand the fresh result to the registered consumer while the
        // postprocess output buffers still hold this inference's data
        if (data->result_callback)
        {
            data->result_callback(&nn_result, frame_id, inference_time_ms,
                                  data->result_callback_user_data);
        }
    }

    // No output frame generated - results are cached internally
    *output_frame = NULL;

    return AICAM_OK;
}

/* ==================== Callback Functions ==================== */

static aicam_result_t video_ai_node_load_model_active(video_node_t *node) {
    LOG_CORE_INFO("AI node load model");
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    

    uintptr_t model_ptr = json_config_get_ai_1_active() ? AI_2_BASE + 1024 : AI_1_BASE + 1024;
    LOG_CORE_INFO("Load model from %p", model_ptr);
    int nn_ret = nn_load_model(model_ptr);
    if(nn_ret != 0) {
        LOG_CORE_ERROR("Failed to load model: %d", nn_ret);
        return AICAM_ERROR;
    }

    if(nn_get_model_info(&data->model_info) != AICAM_OK) {
        LOG_CORE_ERROR("Failed to get model info");
        return AICAM_ERROR;
    }

    json_config_sync_ai_pipe_nvs_from_input_size(data->model_info.input_width, data->model_info.input_height);

    LOG_CORE_INFO("AI model loaded: %dx%d from %p", data->model_info.input_width, data->model_info.input_height, model_ptr);
    return AICAM_OK;
}

static aicam_result_t video_ai_node_init_callback(video_node_t *node) {
    LOG_CORE_INFO("AI node init callback");
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        LOG_CORE_ERROR("Invalid AI node data");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // Find AI device
    data->ai_device = device_find_pattern("nn", DEV_TYPE_AI);
    if (!data->ai_device) {
        LOG_CORE_ERROR("AI device not found");
        return AICAM_ERROR;
    }
    
    // Check NN state
    nn_state_t nn_state = nn_get_state();
    if (nn_state == NN_STATE_READY || nn_state == NN_STATE_RUNNING) {
        LOG_CORE_INFO("NN module is ready, AI processing enabled");
        
        // Get model information if available
        nn_get_model_info(&data->model_info);
    }else if(nn_state == NN_STATE_INIT || nn_state == NN_STATE_UNINIT) {
        LOG_CORE_INFO("NN module is initialized, AI node will work in pass-through mode");
        // load active model
        aicam_result_t result = video_ai_node_load_model_active(node);
        if(result != AICAM_OK) {
            LOG_CORE_ERROR("Failed to load active model: %d", result);
            return result;
        }
    }

    aicam_result_t result = video_ai_start_device(data);
    if(result != AICAM_OK) {
        LOG_CORE_ERROR("Failed to start AI device: %d", result);
        return result;
    }

    // set confidence threshold
    nn_set_confidence_threshold((float)data->config.confidence_threshold / 100.0f);
    nn_set_nms_threshold((float)data->config.nms_threshold / 100.0f);

    //set pipe2 config and cache camera device
    device_t* camera_dev = device_find_pattern(CAMERA_DEVICE_NAME, DEV_TYPE_VIDEO);
    if (!camera_dev) {
        LOG_CORE_ERROR("Camera device not found");
        return AICAM_ERROR;
    }
    pipe_params_t pipe_param = {0};
    if(data->config.width != 0 && data->config.height != 0) {
        pipe_param.width = data->model_info.input_width;
        pipe_param.height = data->model_info.input_height;
    } else {
        pipe_param.width = data->config.width;
        pipe_param.height = data->config.height;
    }
    pipe_param.fps = data->config.fps;
    pipe_param.format = data->config.input_format;
    pipe_param.bpp = data->config.bpp;
    pipe_param.buffer_nb = 3;
    printf("Set pipe2 param: %dx%d@%dfps, format=%d, bpp=%d\r\n",
                 pipe_param.width, pipe_param.height, pipe_param.fps, pipe_param.format, pipe_param.bpp);
    device_ioctl(camera_dev, CAM_CMD_SET_PIPE2_PARAM, (uint8_t*)&pipe_param, sizeof(pipe_params_t));


    data->config.width = data->model_info.input_width;
    data->config.height = data->model_info.input_height;
    LOG_CORE_INFO("AI model loaded: %dx%d", data->config.width, data->config.height);

    uint8_t pipe_ctrl = CAMERA_CTRL_PIPE1_BIT | CAMERA_CTRL_PIPE2_BIT;
    device_ioctl(camera_dev, CAM_CMD_SET_PIPE_CTRL, 
        &pipe_ctrl, 0);


    data->is_initialized = AICAM_TRUE;
    LOG_CORE_INFO("AI node initialized: %dx%d@%dfps, enabled=%d, drawing=%d, nn_state=%d",
                 data->config.width, data->config.height, data->config.fps,
                 data->config.enabled, data->config.enable_drawing, nn_state);
    
    return AICAM_OK;
}

static aicam_result_t video_ai_node_deinit_callback(video_node_t *node) {
    LOG_CORE_INFO("AI node deinit callback");
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    data->is_initialized = AICAM_FALSE;
    data->is_running = AICAM_FALSE;

    /* Release the secondary model (instance + ROI mailbox) */
    if (data->secondary_handle) {
        nn_handle_t handle = data->secondary_handle;
        data->secondary_handle = NULL;
        data->secondary_enabled = AICAM_FALSE;
        if (data->secondary_scratch) {
            hal_mem_free(data->secondary_scratch);
            data->secondary_scratch = NULL;
            data->secondary_scratch_size = 0;
            data->secondary_roi_stride = 0;
        }
        if (data->roi_mutex) {
            osMutexDelete(data->roi_mutex);
            data->roi_mutex = NULL;
        }
        nn_instance_unload_model(handle);
        nn_instance_destroy(handle);
        memset(&data->secondary_model_info, 0, sizeof(nn_model_info_t));
        LOG_CORE_INFO("Secondary model unloaded (AI node deinit)");
    }

    // Deinitialize drawing service
    if (data->draw_service_initialized) {
        video_ai_deinit_draw_service(data);
        data->draw_service_initialized = AICAM_FALSE;
    }

    // Stop device if running
    if (data->is_running) {
        video_ai_stop_device(data);
    }
    
    // Clean up cache mutex
    if (data->cache_mutex) {
        osMutexDelete(data->cache_mutex);
        data->cache_mutex = NULL;
    }
    
    // Clear cache
    data->cache_initialized = AICAM_FALSE;
    memset(data->nn_result_cache, 0, sizeof(data->nn_result_cache));
    
    LOG_CORE_INFO("AI node deinitialized");
    return AICAM_OK;
}

static aicam_result_t video_ai_node_process_callback(video_node_t *node,
                                                    video_frame_t **input_frames,
                                                    uint32_t input_count,
                                                    video_frame_t **output_frames,
                                                    uint32_t *output_count) {
    if (!node || !input_frames || !output_frames || !output_count) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data || !data->is_initialized) {
        return AICAM_OK;
    }

    //printf("AI node process callback\r\n");
    
    *output_count = 0;
    
    // Process AI inference
    video_frame_t *output_frame = NULL;
    aicam_result_t result = video_ai_process_frame(data, &output_frame);

    if (result != AICAM_OK) {
        LOG_CORE_ERROR("AI node process failed: %d", result);
        *output_count = 0;
        return result;
    }
    
    // AI node does not generate output frames - results are cached internally
    *output_count = 0;
    //LOG_CORE_INFO("AI node processing completed, results cached");

    return result;
}

static aicam_result_t video_ai_node_control_callback(video_node_t *node,
                                                    uint32_t cmd,
                                                    void *param) {
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    switch (cmd) {
        case AI_CMD_START_PROCESSING:
            return video_ai_node_start(node);
            
        case AI_CMD_STOP_PROCESSING:
            return video_ai_node_stop(node);
            
        case AI_CMD_SET_CONFIDENCE:
            if (param) {
                uint32_t threshold = *(uint32_t*)param;
                data->config.confidence_threshold = threshold;
                
                // Update NN module
                nn_state_t nn_state = nn_get_state();
                if (nn_state == NN_STATE_READY || nn_state == NN_STATE_RUNNING) {
                    float confidence_threshold = (float)threshold / 100.0f;
                    nn_set_confidence_threshold(confidence_threshold);
                }
                
                LOG_CORE_INFO("AI confidence threshold set to %d", threshold);
                return AICAM_OK;
            }
            break;
            
        case AI_CMD_SET_MAX_DETECTIONS:
            if (param) {
                uint32_t max_detections = *(uint32_t*)param;
                data->config.max_detections = max_detections;
                
                // Update NN module
                nn_state_t nn_state = nn_get_state();
                if (nn_state == NN_STATE_READY || nn_state == NN_STATE_RUNNING) {
                    //nn_set_max_detection_count(max_detections);
                }
                
                LOG_CORE_INFO("AI max detections set to %d", max_detections);
                return AICAM_OK;
            }
            break;
            
        case AI_CMD_LOAD_MODEL:
            if (param) {
                uintptr_t model_ptr = *(uintptr_t*)param;
                return video_ai_node_load_model(node, model_ptr);
            }
            break;
            
        case AI_CMD_UNLOAD_MODEL:
            return video_ai_node_unload_model(node);
            
        case AI_CMD_GET_MODEL_INFO:
            if (param) {
                nn_model_info_t *model_info = (nn_model_info_t*)param;
                return video_ai_node_get_model_info(node, model_info);
            }
            break;
            
        case AI_CMD_ENABLE_DRAWING:
            data->config.enable_drawing = AICAM_TRUE;
            if (!data->draw_service_initialized) {
                aicam_result_t result = video_ai_init_draw_service(data);
                if (result == AICAM_OK) {
                    data->draw_service_initialized = AICAM_TRUE;
                    LOG_CORE_INFO("AI drawing enabled");
                } else {
                    LOG_CORE_ERROR("Failed to enable AI drawing: %d", result);
                    return result;
                }
            }
            return AICAM_OK;
            
        case AI_CMD_DISABLE_DRAWING:
            data->config.enable_drawing = AICAM_FALSE;
            if (data->draw_service_initialized) {
                video_ai_deinit_draw_service(data);
                data->draw_service_initialized = AICAM_FALSE;
                LOG_CORE_INFO("AI drawing disabled");
            }
            return AICAM_OK;
            
        case AI_CMD_SET_DRAW_CONFIG:
            if (param) {
                ai_draw_config_t *draw_config = (ai_draw_config_t*)param;
                memcpy(&data->config.draw_config, draw_config, sizeof(ai_draw_config_t));
                
                // Update drawing service configuration if initialized
                if (data->draw_service_initialized) {
                    aicam_result_t result = ai_draw_set_config(draw_config);
                    if (result != AICAM_OK) {
                        LOG_CORE_ERROR("Failed to update draw config: %d", result);
                        return result;
                    }
                }
                
                LOG_CORE_INFO("AI draw configuration updated");
                return AICAM_OK;
            }
            break;
            
        case AI_CMD_GET_DRAW_CONFIG:
            if (param) {
                ai_draw_config_t *draw_config = (ai_draw_config_t*)param;
                memcpy(draw_config, &data->config.draw_config, sizeof(ai_draw_config_t));
                return AICAM_OK;
            }
            break;
            
        default:
            LOG_CORE_WARN("Unknown AI control command: 0x%x", cmd);
            return AICAM_ERROR_INVALID_PARAM;
    }
    
    return AICAM_OK;
}

/* ==================== AI Drawing Service Integration ==================== */

static aicam_result_t video_ai_init_draw_service(video_ai_node_data_t *data)
{
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // Update drawing configuration with current AI node settings
    data->config.draw_config.image_width = data->config.width;
    data->config.draw_config.image_height = data->config.height;
    
    // Initialize AI drawing service
    aicam_result_t result = ai_draw_service_init(&data->config.draw_config);
    if (result != AICAM_OK) {
        LOG_CORE_ERROR("Failed to initialize AI draw service: %d", result);
        return result;
    }
    
    LOG_CORE_INFO("AI draw service initialized for %dx%d", 
                  data->config.width, data->config.height);
    
    return AICAM_OK;
}

static aicam_result_t video_ai_deinit_draw_service(video_ai_node_data_t *data)
{
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // Deinitialize AI drawing service
    aicam_result_t result = ai_draw_service_deinit();
    if (result != AICAM_OK) {
        LOG_CORE_ERROR("Failed to deinitialize AI draw service: %d", result);
        return result;
    }
    
    LOG_CORE_INFO("AI draw service deinitialized");
    
    return AICAM_OK;
}

