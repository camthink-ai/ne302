/**
 ******************************************************************************
 * @file    image_utils.c
 * @author  GPM Application Team
 * @brief   Image processing utilities implementation for AI model cascade
 *
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2023 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

#include "image_utils.h"
#include "draw.h"
#include "dev_manager.h"
#include "pixel_format_map.h"
#include "mem.h"
#include "debug.h"
#include "stm32n6xx_hal.h"
#include "stm32n6xx_hal_dma2d.h"
#include "cmsis_os2.h"
#include <string.h>
#include <math.h>
#include <stdint.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

/**
 * @brief Bilinear interpolation for a single pixel
 */
static void bilinear_interpolate_pixel(
    const uint8_t *src,
    uint32_t src_width,
    uint32_t src_height,
    uint32_t src_bytes_per_pixel,
    float x,
    float y,
    uint8_t *dst,
    uint32_t dst_bytes_per_pixel
)
{
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    int x1 = MIN(x0 + 1, (int)src_width - 1);
    int y1 = MIN(y0 + 1, (int)src_height - 1);
    
    float fx = x - x0;
    float fy = y - y0;
    float fx1 = 1.0f - fx;
    float fy1 = 1.0f - fy;
    
    // Clamp coordinates
    x0 = MAX(0, MIN(x0, (int)src_width - 1));
    y0 = MAX(0, MIN(y0, (int)src_height - 1));
    
    // Get four corner pixels
    const uint8_t *p00 = src + (y0 * src_width + x0) * src_bytes_per_pixel;
    const uint8_t *p01 = src + (y0 * src_width + x1) * src_bytes_per_pixel;
    const uint8_t *p10 = src + (y1 * src_width + x0) * src_bytes_per_pixel;
    const uint8_t *p11 = src + (y1 * src_width + x1) * src_bytes_per_pixel;
    
    // Interpolate each channel
    for (uint32_t c = 0; c < dst_bytes_per_pixel; c++) {
        float val = (p00[c] * fx1 + p01[c] * fx) * fy1 +
                    (p10[c] * fx1 + p11[c] * fx) * fy;
        dst[c] = (uint8_t)MAX(0, MIN(255, (int)roundf(val)));
    }
}

aicam_result_t image_resize(
    const uint8_t *src_image,
    uint32_t src_width,
    uint32_t src_height,
    uint32_t src_format,
    uint8_t *dst_image,
    uint32_t dst_width,
    uint32_t dst_height,
    uint32_t dst_format
)
{
    if (!src_image || !dst_image || src_width == 0 || src_height == 0 || 
        dst_width == 0 || dst_height == 0) {
        LOG_DRV_ERROR("Invalid parameters for image_resize");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    int src_bpp = DMA2D_BYTES_PER_PIXEL(src_format);
    int dst_bpp = DMA2D_BYTES_PER_PIXEL(dst_format);
    
    if (src_bpp == 0 || dst_bpp == 0) {
        LOG_DRV_ERROR("Unsupported pixel format for resize");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // Calculate scale factors
    float scale_x = (float)src_width / dst_width;
    float scale_y = (float)src_height / dst_height;
    
    // Resize using bilinear interpolation
    for (uint32_t dy = 0; dy < dst_height; dy++) {
        for (uint32_t dx = 0; dx < dst_width; dx++) {
            // Map destination coordinates to source coordinates
            float sx = (dx + 0.5f) * scale_x - 0.5f;
            float sy = (dy + 0.5f) * scale_y - 0.5f;
            
            uint8_t *dst_pixel = dst_image + (dy * dst_width + dx) * dst_bpp;
            bilinear_interpolate_pixel(src_image, src_width, src_height, src_bpp,
                                      sx, sy, dst_pixel, dst_bpp);
        }
    }
    
    return AICAM_OK;
}

aicam_result_t image_crop_roi(
    const uint8_t *src_image,
    uint32_t src_width,
    uint32_t src_height,
    uint32_t src_format,
    uint32_t roi_x,
    uint32_t roi_y,
    uint32_t roi_width,
    uint32_t roi_height,
    uint8_t *dst_buffer,
    uint32_t dst_format
)
{
    if (!src_image || !dst_buffer) {
        LOG_DRV_ERROR("Invalid parameters for image_crop_roi");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // Boundary check
    if (roi_x + roi_width > src_width) {
        roi_width = src_width - roi_x;
    }
    if (roi_y + roi_height > src_height) {
        roi_height = src_height - roi_y;
    }
    
    if (roi_width == 0 || roi_height == 0) {
        LOG_DRV_ERROR("Invalid ROI dimensions");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // Bytes per pixel of the source format
    int src_bpp = DMA2D_BYTES_PER_PIXEL(src_format);
    if (src_bpp == 0) {
        LOG_DRV_ERROR("Unsupported source pixel format");
        return AICAM_ERROR_INVALID_PARAM;
    }

    /* Row-wise CPU copy. The previous DRAW_CMD_COPY implementation was
     * broken for any ROI: draw_copy_hw hardwires the foreground
     * InputOffset to 0, so the hardware read the ROI rows as if they were
     * contiguous and produced sheared output (the source frame's row
     * stride is src_width, not roi_width). A strided DMA2D copy would
     * need a driver extension; at crop sizes a CPU row copy is exact and
     * fast enough, and works with both cacheable and uncached memory. */
    const uint8_t *src_row = src_image + ((size_t)roi_y * src_width + roi_x) * src_bpp;
    size_t row_bytes = (size_t)roi_width * src_bpp;
    for (uint32_t row = 0; row < roi_height; row++) {
        memcpy(dst_buffer + (size_t)row * roi_width * src_bpp,
               src_row + (size_t)row * src_width * src_bpp,
               row_bytes);
    }

    return AICAM_OK;
}

aicam_result_t image_extract_and_resize_roi(
    const uint8_t *src_image,
    uint32_t src_width,
    uint32_t src_height,
    uint32_t src_format,
    const od_detect_t *bbox,
    uint32_t dst_width,
    uint32_t dst_height,
    uint32_t dst_format,
    uint8_t **dst_buffer,
    uint32_t *dst_size
)
{
    if (!src_image || !bbox || !dst_buffer || !dst_size) {
        LOG_DRV_ERROR("Invalid parameters for image_extract_and_resize_roi");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // 1. Convert normalized coordinates to pixel coordinates
    uint32_t roi_x = (uint32_t)(bbox->x * src_width);
    uint32_t roi_y = (uint32_t)(bbox->y * src_height);
    uint32_t roi_w = (uint32_t)(bbox->width * src_width);
    uint32_t roi_h = (uint32_t)(bbox->height * src_height);
    
    // Boundary check and clamp
    if (roi_x >= src_width || roi_y >= src_height) {
        LOG_DRV_ERROR("ROI out of bounds");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    if (roi_x + roi_w > src_width) {
        roi_w = src_width - roi_x;
    }
    if (roi_y + roi_h > src_height) {
        roi_h = src_height - roi_y;
    }
    
    if (roi_w == 0 || roi_h == 0) {
        LOG_DRV_ERROR("Invalid ROI dimensions after clamping");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // 2. Calculate buffer sizes
    int src_bpp = DMA2D_BYTES_PER_PIXEL(src_format);
    int dst_bpp = DMA2D_BYTES_PER_PIXEL(dst_format);
    
    if (src_bpp == 0 || dst_bpp == 0) {
        LOG_DRV_ERROR("Unsupported pixel format");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    uint32_t temp_size = roi_w * roi_h * src_bpp;
    uint32_t final_size = dst_width * dst_height * dst_bpp;
    
    // 3. Allocate temporary buffer for cropped ROI
    uint8_t *temp_buffer = hal_mem_alloc_large(temp_size);
    if (!temp_buffer) {
        LOG_DRV_ERROR("Failed to allocate temporary buffer for ROI");
        return AICAM_ERROR_NO_MEMORY;
    }
    
    // 4. Crop ROI using DMA2D hardware
    aicam_result_t ret = image_crop_roi(src_image, src_width, src_height, src_format,
                                       roi_x, roi_y, roi_w, roi_h,
                                       temp_buffer, src_format);
    if (ret != AICAM_OK) {
        LOG_DRV_ERROR("Failed to crop ROI: %d", ret);
        hal_mem_free(temp_buffer);
        return ret;
    }
    
    // 5. Allocate destination buffer
    *dst_buffer = hal_mem_alloc_large(final_size);
    if (!*dst_buffer) {
        LOG_DRV_ERROR("Failed to allocate destination buffer");
        hal_mem_free(temp_buffer);
        return AICAM_ERROR_NO_MEMORY;
    }
    
    // 6. Resize cropped ROI to target dimensions
    ret = image_resize(temp_buffer, roi_w, roi_h, src_format,
                      *dst_buffer, dst_width, dst_height, dst_format);
    
    // Free temporary buffer
    hal_mem_free(temp_buffer);
    
    if (ret != AICAM_OK) {
        LOG_DRV_ERROR("Failed to resize ROI: %d", ret);
        hal_mem_free(*dst_buffer);
        *dst_buffer = NULL;
        return ret;
    }
    
    *dst_size = final_size;
    return AICAM_OK;
}
