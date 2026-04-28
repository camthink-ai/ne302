#ifndef FSBL_APP_COMMON_H
#define FSBL_APP_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include "mem_map.h"
#include "stm32n6xx_hal.h"
#include "isp_core.h"
#include "cmw_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_CLK_CONFIG_SAVE_FLASH_BASE          (SWAP_BASE + 0x0000)  /* offset: 0K*/
#define SYS_CLK_CONFIG_SAVE_FLASH_SIZE          (0x1000)
#define QUICK_SNAPSHOT_CONFIG_SAVE_FLASH_BASE   (SWAP_BASE + 0x1000) /* offset: 4K */
#define QUICK_SNAPSHOT_CONFIG_SAVE_FLASH_SIZE   (0x8000)

#define QUICK_SNAPSHOT_RESULT_PSRAM_BASE        (PSRAM_SWAP_BASE)
#define QUICK_SNAPSHOT_FB_PSRAM_BASE            (PSRAM_REGION_BASE)

/**
 * Snapshot capture time; binary layout matches legacy ms_bridging_time_t
 * (see ms_bridging.h) for interchange with the application.
 */
#pragma pack(push, 1)
typedef struct {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t week;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
} fsbl_app_snapshot_time_t;

/**
 * Persisted CPU clock profile for next FSBL boot (see FSBL SysClk_Profile_t / sysclk.h).
 * Stored at SYS_CLK_CONFIG_SAVE_FLASH_BASE; crc32 computed by fsbl_app_write_sys_clk_config().
 */
#define FSBL_APP_SYSCLK_PROFILE_HSE_200MHZ   1U
#define FSBL_APP_SYSCLK_PROFILE_HSE_400MHZ   2U
#define FSBL_APP_SYSCLK_PROFILE_HSI_800MHZ   3U
#define FSBL_APP_SYSCLK_PROFILE_HSE_800MHZ   4U

typedef struct {
  uint32_t sys_clk_profile;
  uint32_t crc32;
} sys_clk_config_t;

typedef struct {
  ISP_IQParamTypeDef isp_iq_param;
  CMW_DCMIPP_Conf_t main_pipe_conf;
  CMW_DCMIPP_Conf_t ai_pipe_conf;

  uint8_t mirror_flip;
  uint8_t skip_frames;

  uint8_t light_mode;
  uint8_t light_threshold;
  uint8_t light_brightness;
  uint32_t light_start_time;
  uint32_t light_end_time;

  uint32_t wakeup_flag_mask;
  uint32_t crc32;
} quick_snapshot_config_t;

typedef struct {
  quick_snapshot_config_t config;

  uint8_t *main_pipe_fb;
  uint32_t main_pipe_fb_size;
  uint8_t *ai_pipe_fb;
  uint32_t ai_pipe_fb_size;

  uint32_t wakeup_flag;
  fsbl_app_snapshot_time_t time;
  uint32_t crc32;
} quick_snapshot_result_t;
#pragma pack(pop)

typedef int (*common_flash_read_func_t)(uint32_t address, void *data, size_t size);
typedef int (*common_flash_write_func_t)(uint32_t address, void *data, size_t size);
typedef int (*common_flash_erase_func_t)(uint32_t address, size_t size);
typedef uint32_t (*common_crc32_func_t)(void *data, size_t size);

typedef struct {
  common_flash_read_func_t flash_read;
  common_flash_write_func_t flash_write;
  common_flash_erase_func_t flash_erase;
  common_crc32_func_t crc32;
} common_flash_ops_t;

int fsbl_app_common_init(common_flash_ops_t *flash_ops);
int fsbl_app_read_sys_clk_config(sys_clk_config_t *config);
int fsbl_app_write_sys_clk_config(sys_clk_config_t *config);
int fsbl_app_read_quick_snapshot_config(quick_snapshot_config_t *config);
int fsbl_app_write_quick_snapshot_config(quick_snapshot_config_t *config);
quick_snapshot_result_t *fsbl_app_get_quick_snapshot_result(void);

#ifdef __cplusplus
}
#endif

#endif /* FSBL_APP_COMMON_H */
