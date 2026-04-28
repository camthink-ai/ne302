#include "sys_config.h"
#include "pwr_manager.h"
#include "n6_comm.h"
#include "rtc.h"
#include "pir.h"
#include "app.h"

#if APP_IS_USER_STR_CMD == 1

#define MAX_ARGC 10
static unsigned int app_run_seconds = 0;

void parse_args(char *input, char *argv[], int *argc)
{
    *argc = 0;
    char *p = strtok(input, " ");
    while (p != NULL && *argc < MAX_ARGC)
    {
        argv[(*argc)++] = p;
        p = strtok(NULL, " ");
    }
}

void n6_comm_recv_callback(uint8_t *data, uint16_t len) 
{
    const char *state = NULL;
    char *argv[MAX_ARGC] = {0};
    char send_buf[128] = {0};
    int argc = 0, sleep_second = 0, year = 0;
    uint32_t wakeup_flags = 0, switch_bits = 0;
    RTC_TimeTypeDef time = {0};
    RTC_DateTypeDef date = {0};
    pwr_rtc_wakeup_config_t rtc_wakeup_config = {0};
    if (data == NULL || len == 0 || len > 512 || strlen((char *)data) < 3) return;

    parse_args((char *)data, argv, &argc);
    if (argc < 1) {
        n6_comm_send_str("Err: Please enter the command\r\n");
        return;
    }
    if (strncmp(argv[0], "pwr", 3) == 0) {
        if (argc < 2) {
            n6_comm_send_str("Err: Missing parameter\r\n");
            return;
        }
        if (argc > 2) {
            pwr_ctrl(argv[1], argv[2]);
            n6_comm_send_str("OK\r\n");
        } else {
            state = pwr_get_state(argv[1]);
            if (state) {
                n6_comm_send_str(state);
            } else {
                n6_comm_send_str("Err: Unknown pwr module\r\n");
                return;
            }
        }
    } else if (strncmp(argv[0], "rtc", 3) == 0) {
        if (argc >= 7) {
            year = atoi(argv[1]);
            if (year > 2000) year -= 2000;
            date.Year = year;
            date.Month = atoi(argv[2]);
            date.Date = atoi(argv[3]);
            time.Hours = atoi(argv[4]);
            time.Minutes = atoi(argv[5]);
            time.Seconds = atoi(argv[6]);
            if (argc > 7) date.WeekDay = atoi(argv[7]);
            else HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN);
            if (date.Year < 0 || date.Year > 99 || date.Month < 1 || date.Month > 12 || date.Date < 1 || date.Date > 31 ||
                time.Hours < 0 || time.Hours > 23 || time.Minutes < 0 || time.Minutes > 59 || time.Seconds < 0 || time.Seconds > 59 || date.WeekDay < 1 || date.WeekDay > 7) {
                n6_comm_send_str("Err: Invalid Date/time\r\n");
                return;
            }
            HAL_RTC_SetTime(&hrtc, &time, RTC_FORMAT_BIN);
            HAL_RTC_SetDate(&hrtc, &date, RTC_FORMAT_BIN);
            n6_comm_send_str("OK\r\n");
        } else {
            HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BIN);
            HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN);
            char buf[64] = {0};
            snprintf(buf, sizeof(buf), "20%02d-%02d-%02d %02d:%02d:%02d %d\r\n", 
                     date.Year, date.Month, date.Date, time.Hours, time.Minutes, time.Seconds, date.WeekDay);
            n6_comm_send_str(buf);
        }
    } else if (strncmp(argv[0], "standby", 7) == 0) {
        n6_comm_send_str("OK\r\n");
        if (argc > 1) sleep_second = atoi(argv[1]);
        wakeup_flags = PWR_WAKEUP_FLAG_RTC_TIMING | PWR_WAKEUP_FLAG_CONFIG_KEY;
        rtc_wakeup_config.wakeup_time_s = sleep_second;
        pwr_enter_standby(wakeup_flags, &rtc_wakeup_config);
    } else if (strncmp(argv[0], "stop2", 5) == 0) {
        n6_comm_send_str("OK\r\n");
        if (argc > 1) sleep_second = atoi(argv[1]);
        for (int i = 2; i < argc; i++) switch_bits |= pwr_get_switch_bit(argv[i]);
        wakeup_flags = PWR_WAKEUP_FLAG_RTC_TIMING | PWR_WAKEUP_FLAG_CONFIG_KEY;
        if (switch_bits & (PWR_WIFI_SWITCH_BIT | PWR_3V3_SWITCH_BIT)) wakeup_flags |= PWR_WAKEUP_FLAG_SI91X;
        if (switch_bits & (PWR_EXT_SWITCH_BIT | PWR_3V3_SWITCH_BIT)) wakeup_flags |= PWR_WAKEUP_FLAG_NET;

        rtc_wakeup_config.wakeup_time_s = sleep_second;
        pwr_enter_stop2(wakeup_flags, switch_bits, &rtc_wakeup_config);
    } else if (strncmp(argv[0], "state", 5) == 0) {
        snprintf(send_buf, sizeof(send_buf), "Version: %s\r\n", APP_VERSION);
        n6_comm_send_str(send_buf);
        snprintf(send_buf, sizeof(send_buf), "Run time: %d day, %02d:%02d:%02d\r\n", app_run_seconds / 86400, (app_run_seconds % 86400) / 3600, (app_run_seconds % 3600) / 60, app_run_seconds % 60);
        n6_comm_send_str(send_buf);
        snprintf(send_buf, sizeof(send_buf), "Wakeup flag: 0x%08lX\r\n", pwr_get_wakeup_flags());
        n6_comm_send_str(send_buf);
    } else {
        snprintf(send_buf, sizeof(send_buf), "Err: Unknown command(%s)\r\n", argv[0]);
        n6_comm_send_str(send_buf);
        return;
    }
}

void app_task(void *argument) 
{
    for (;;) {
        app_run_seconds++;
        osDelay(1000);
    }
}

#else

#include "ms_bridging.h"

static ms_bridging_handler_t *g_ms_bridging_handler = NULL;
static app_n6_state_t n6_state = N6_STATE_STARTUP;
static uint8_t pir_is_inited = 0;
#if MS_BD_KEEPLIVE_ENABLE
static uint32_t last_keep_alive_time_ms = 0;
#endif
static osEventFlagsId_t app_task_event = NULL;

#define APP_TASK_EVENT_FLAG_STOP        (1 << 0)
#define APP_TASK_EVENT_FLAG_STOP_ACK    (1 << 1)
#define APP_TASK_EVENT_FLAG_START       (1 << 2)
#define APP_TASK_EVENT_FLAG_START_ACK   (1 << 3)

void n6_comm_recv_callback(uint8_t *data, uint16_t len) {
    // WIC_LOGD("n6_comm_recv_callback: len = %d", len);
    ms_bridging_recv(g_ms_bridging_handler, data, len);
}

void ms_bridging_polling_task(void *argument) 
{
    for (;;) {
        ms_bridging_polling(g_ms_bridging_handler);
    }
}

void ms_bridging_notify_callback(void *handler, ms_bridging_frame_t *frame)
{
    uint32_t usb_in_status = 0;
    uint32_t pin_value = 0, pir_cfg_result = 0;
    uint32_t switch_bits = 0, wakeup_flags = 0;
    RTC_TimeTypeDef time = {0};
    RTC_DateTypeDef date = {0};
    ms_bridging_time_t data_time = {0};
    ms_bridging_power_ctrl_t power_ctrl = {0};
    ms_bridging_pir_cfg_t *ms_pir_cfg = NULL;
    pwr_rtc_wakeup_config_t rtc_wakeup_config = {0};
    ms_bridging_version_t version = {0};
    pir_config_t pir_cfg = {0};

#if MS_BD_KEEPLIVE_ENABLE
    last_keep_alive_time_ms = osKernelGetTickCount();
#endif
    WIC_LOGD("ms_bridging_notify_cb_t: id = %d cmd = %d type = %d", frame->header.id, frame->header.cmd, frame->header.type);
    if (frame->header.type == MS_BR_FRAME_TYPE_REQUEST) {
        switch (frame->header.cmd) {
            case MS_BR_FRAME_CMD_GET_TIME:
                HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BIN);
                HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN);
                data_time.year = date.Year;
                data_time.month = date.Month;
                data_time.day = date.Date;
                data_time.week = date.WeekDay;
                data_time.hour = time.Hours;
                data_time.minute = time.Minutes;
                data_time.second = time.Seconds;
                ms_bridging_response(handler, frame, &data_time, sizeof(data_time));
                break;
            case MS_BR_FRAME_CMD_SET_TIME:
                memcpy(&data_time, frame->data, sizeof(data_time));
                date.Year = data_time.year;
                date.Month = data_time.month;
                date.Date = data_time.day;
                date.WeekDay = data_time.week;
                time.Hours = data_time.hour;
                time.Minutes = data_time.minute;
                time.Seconds = data_time.second;
                HAL_RTC_SetTime(&hrtc, &time, RTC_FORMAT_BIN);
                HAL_RTC_SetDate(&hrtc, &date, RTC_FORMAT_BIN);
                ms_bridging_response(handler, frame, NULL, 0);
                break;
            case MS_BR_FRAME_CMD_PWR_CTRL:
                ms_bridging_response(handler, frame, NULL, 0);
                memcpy(&power_ctrl, frame->data, sizeof(power_ctrl));
                rtc_wakeup_config.alarm_a.is_valid = power_ctrl.alarm_a.is_valid;
                rtc_wakeup_config.alarm_a.week_day = power_ctrl.alarm_a.week_day;
                rtc_wakeup_config.alarm_a.date = power_ctrl.alarm_a.date;
                rtc_wakeup_config.alarm_a.hour = power_ctrl.alarm_a.hour;
                rtc_wakeup_config.alarm_a.minute = power_ctrl.alarm_a.minute;
                rtc_wakeup_config.alarm_a.second = power_ctrl.alarm_a.second;
                rtc_wakeup_config.alarm_b.is_valid = power_ctrl.alarm_b.is_valid;
                rtc_wakeup_config.alarm_b.week_day = power_ctrl.alarm_b.week_day;
                rtc_wakeup_config.alarm_b.date = power_ctrl.alarm_b.date;
                rtc_wakeup_config.alarm_b.hour = power_ctrl.alarm_b.hour;
                rtc_wakeup_config.alarm_b.minute = power_ctrl.alarm_b.minute;
                rtc_wakeup_config.alarm_b.second = power_ctrl.alarm_b.second;
                rtc_wakeup_config.wakeup_time_s = power_ctrl.sleep_second;
                usb_in_status = pwr_usb_is_active();
                if (power_ctrl.power_mode == MS_BR_PWR_MODE_STANDBY && (usb_in_status == 0)) {
                    WIC_LOGD("pwr_enter_standby: wakeup_flags = 0x%08lX, wakeup_time_s = %d", power_ctrl.wakeup_flags, rtc_wakeup_config.wakeup_time_s);
                    pwr_enter_standby(power_ctrl.wakeup_flags, &rtc_wakeup_config);
                } else if (power_ctrl.power_mode == MS_BR_PWR_MODE_STOP2 || (power_ctrl.power_mode == MS_BR_PWR_MODE_STANDBY && (usb_in_status == 1))) {
                    WIC_LOGD("pwr_enter_stop2: wakeup_flags = 0x%08lX, switch_bits = 0x%08lX, wakeup_time_s = %d", power_ctrl.wakeup_flags, power_ctrl.switch_bits, rtc_wakeup_config.wakeup_time_s);
                    osEventFlagsSet(app_task_event, APP_TASK_EVENT_FLAG_STOP);
                    osEventFlagsWait(app_task_event, APP_TASK_EVENT_FLAG_STOP_ACK, osFlagsWaitAny, osWaitForever);

                    vTaskSuspendAll();
                    pwr_enter_stop2(power_ctrl.wakeup_flags, power_ctrl.switch_bits, &rtc_wakeup_config);
                    xTaskResumeAll();
                    n6_comm_set_event_isr(N6_COMM_EVENT_ERR);
                    osDelay(30);

                    osEventFlagsSet(app_task_event, APP_TASK_EVENT_FLAG_START);
                    osEventFlagsWait(app_task_event, APP_TASK_EVENT_FLAG_START_ACK, osFlagsWaitAny, osWaitForever);
                } else {
                    pwr_ctrl_bits(power_ctrl.switch_bits);
                }
                break;
            case MS_BR_FRAME_CMD_PWR_STATUS:
                switch_bits = pwr_get_switch_bits();
                ms_bridging_response(handler, frame, &switch_bits, sizeof(switch_bits));
                break;
            case MS_BR_FRAME_CMD_WKUP_FLAG:
                wakeup_flags = pwr_get_wakeup_flags();
                ms_bridging_response(handler, frame, &wakeup_flags, sizeof(wakeup_flags));
                break;
            case MS_BR_FRAME_CMD_CLEAR_FLAG:
                pwr_clear_wakeup_flags();
                ms_bridging_response(handler, frame, NULL, 0);
                break;
            case MS_BR_FRAME_CMD_RST_N6:
                ms_bridging_response(handler, frame, NULL, 0);
                osEventFlagsSet(app_task_event, APP_TASK_EVENT_FLAG_STOP);
                osEventFlagsWait(app_task_event, APP_TASK_EVENT_FLAG_STOP_ACK, osFlagsWaitAny, osWaitForever);
                pwr_n6_restart(300, 30);
                osEventFlagsSet(app_task_event, APP_TASK_EVENT_FLAG_START);
                osEventFlagsWait(app_task_event, APP_TASK_EVENT_FLAG_START_ACK, osFlagsWaitAny, osWaitForever);
                break;
            case MS_BR_FRAME_CMD_KEY_VALUE:
                pin_value = HAL_GPIO_ReadPin(CONFIG_KEY_GPIO_Port, CONFIG_KEY_Pin);
                ms_bridging_response(handler, frame, &pin_value, sizeof(pin_value));
                break;
            case MS_BR_FRAME_CMD_PIR_VALUE:
                pin_value = HAL_GPIO_ReadPin(PIR_TRIGGER_GPIO_Port, PIR_TRIGGER_Pin);
                ms_bridging_response(handler, frame, &pin_value, sizeof(pin_value));
                break;
            case MS_BR_FRAME_CMD_USB_VIN_VALUE:
                usb_in_status = pwr_usb_is_active();
                ms_bridging_response(handler, frame, &usb_in_status, sizeof(usb_in_status));
                break;
            case MS_BR_FRAME_CMD_PIR_CFG:
                ms_pir_cfg = (ms_bridging_pir_cfg_t *)frame->data;
                if (ms_pir_cfg == NULL) {
                    pir_cfg_result = pir_config(NULL);
                } else {
                    pir_cfg.SENS = ms_pir_cfg->sensitivity_level;
                    pir_cfg.BLIND = ms_pir_cfg->ignore_time_s;
                    pir_cfg.PULSE = ms_pir_cfg->pulse_count;
                    pir_cfg.WINDOW = ms_pir_cfg->window_time_s;
                    pir_cfg.MOTION = ms_pir_cfg->motion_enable;
                    pir_cfg.INT = ms_pir_cfg->interrupt_src;
                    pir_cfg.VOLT = ms_pir_cfg->volt_select;
                    pir_cfg.SUPP = ms_pir_cfg->reserved1;
                    pir_cfg.RSV = ms_pir_cfg->reserved2;
                    pir_cfg_result = pir_config(&pir_cfg);
                }
                if (pir_cfg_result == 0) pir_is_inited = 1;
                ms_bridging_response(handler, frame, &pir_cfg_result, sizeof(pir_cfg_result));
                break;
            case MS_BR_FRAME_CMD_GET_VERSION:
                ms_bridging_get_version_from_str(APP_VERSION, &version);
                ms_bridging_response(handler, frame, &version, sizeof(version));
                break;
            default:
                WIC_LOGW("ms_bridging_notify_cb_t: unknown request cmd = %d", frame->header.cmd);
                break;
        }
    } else if (frame->header.type == MS_BR_FRAME_TYPE_EVENT) {
        WIC_LOGW("ms_bridging_notify_cb_t: unknown event cmd = %d", frame->header.cmd);
    }
}

void app_task(void *argument) 
{
    int ret = 0;
    uint32_t task_event = 0;
#if MS_BD_KEEPLIVE_ENABLE
    uint32_t now_time_ms = 0, diff_time_ms = 0;
#endif
    GPIO_PinState last_key_state = GPIO_PIN_RESET, key_state = GPIO_PIN_RESET;
    GPIO_PinState last_pir_state = GPIO_PIN_RESET, pir_state = GPIO_PIN_RESET;


#if MS_BD_KEEPLIVE_ENABLE
    last_keep_alive_time_ms = osKernelGetTickCount();
#endif
    for (;;) {
        switch (n6_state) {
            case N6_STATE_STARTUP:
                ret = ms_bridging_request_keep_alive(g_ms_bridging_handler);
            #if MS_BD_KEEPLIVE_ENABLE
                if (ret != MS_BR_OK) {
                    WIC_LOGW("app: keep alive failed = %d", ret);
                    now_time_ms = osKernelGetTickCount();
                    diff_time_ms = (now_time_ms >= last_keep_alive_time_ms) ? (now_time_ms - last_keep_alive_time_ms) : (now_time_ms + (osWaitForever - last_keep_alive_time_ms));
                    if (diff_time_ms > MS_BD_STARTUP_TIMEOUT_MS) {
                        WIC_LOGW("app: keep alive timeout = %dms, restart N6", diff_time_ms);
                        n6_state = N6_STATE_WAIT_REBOOT;
                        break;
                    }
                } else {
                    last_key_state = HAL_GPIO_ReadPin(CONFIG_KEY_GPIO_Port, CONFIG_KEY_Pin);
                    ret = ms_bridging_event_key_value(g_ms_bridging_handler, last_key_state);
                    if (ret != MS_BR_OK) {
                        WIC_LOGW("app: send key value event failed = %d", ret);
                        n6_state = N6_STATE_WAIT_REBOOT;
                    } else {
                        last_keep_alive_time_ms = osKernelGetTickCount();
                        n6_state = N6_STATE_RUNNING;
                    }
                }
            #else
                if (ret == MS_BR_OK) {
                    last_key_state = HAL_GPIO_ReadPin(CONFIG_KEY_GPIO_Port, CONFIG_KEY_Pin);
                    ret = ms_bridging_event_key_value(g_ms_bridging_handler, last_key_state);
                    if (ret == MS_BR_OK) n6_state = N6_STATE_RUNNING;
                }
            #endif
                break;
            case N6_STATE_RUNNING:
                key_state = HAL_GPIO_ReadPin(CONFIG_KEY_GPIO_Port, CONFIG_KEY_Pin);
                if (key_state != last_key_state) {
                    ret = ms_bridging_event_key_value(g_ms_bridging_handler, key_state);
                    if (ret != MS_BR_OK) {
                        WIC_LOGW("app: send key value event failed = %d", ret);
                        n6_state = N6_STATE_WAIT_REBOOT;
                    }
                #if MS_BD_KEEPLIVE_ENABLE
                    else {
                        last_keep_alive_time_ms = osKernelGetTickCount();
                    }
                #endif
                    WIC_LOGD("app: key state changed = %d", key_state);
                    last_key_state = key_state;
                }

                // if (pir_is_inited) {
                    pir_state = HAL_GPIO_ReadPin(PIR_TRIGGER_GPIO_Port, PIR_TRIGGER_Pin);
                    if (pir_state != last_pir_state) {
                        if (pir_state == GPIO_PIN_SET) pir_trigger_reset();
                        ret = ms_bridging_event_pir_value(g_ms_bridging_handler, pir_state);
                        if (ret != MS_BR_OK) {
                            WIC_LOGW("app: send pir value event failed = %d", ret);
                            n6_state = N6_STATE_WAIT_REBOOT;
                        }
                    #if MS_BD_KEEPLIVE_ENABLE
                        else {
                            last_keep_alive_time_ms = osKernelGetTickCount();
                        }
                    #endif
                        WIC_LOGD("app: pir state changed = %d", pir_state);
                        last_pir_state = pir_state;
                    }
                // }
                
            #if MS_BD_KEEPLIVE_ENABLE
                else {
                    now_time_ms = osKernelGetTickCount();
                    diff_time_ms = (now_time_ms >= last_keep_alive_time_ms) ? (now_time_ms - last_keep_alive_time_ms) : (now_time_ms + (osWaitForever - last_keep_alive_time_ms));
                    if (diff_time_ms > MS_BD_KEEPLIVE_INTERVAL_MS) {
                        ret = ms_bridging_request_keep_alive(g_ms_bridging_handler);
                        if (ret != MS_BR_OK) {
                            WIC_LOGW("app: keep alive failed = %d", ret);
                            n6_state = N6_STATE_WAIT_REBOOT;
                            break;
                        } else {
                            last_keep_alive_time_ms = osKernelGetTickCount();
                        }
                    } else if (diff_time_ms > MS_BD_KEEPLIVE_INTERVAL_MS / 2) {
                        ret = ms_bridging_request_keep_alive(g_ms_bridging_handler);
                        if (ret == MS_BR_OK) last_keep_alive_time_ms = osKernelGetTickCount();
                    }
                }
            #endif
                break;
            case N6_STATE_STOPPED:
                task_event = osEventFlagsWait(app_task_event, APP_TASK_EVENT_FLAG_START, osFlagsWaitAny, osWaitForever);
                if (!(task_event & osFlagsError) && (task_event & APP_TASK_EVENT_FLAG_START)) {
                #if MS_BD_KEEPLIVE_ENABLE
                    last_keep_alive_time_ms = osKernelGetTickCount();
                #endif
                    n6_state = N6_STATE_STARTUP;
                    osEventFlagsSet(app_task_event, APP_TASK_EVENT_FLAG_START_ACK);
                }
                break;
            case N6_STATE_WAIT_REBOOT:
            #if MS_BD_KEEPLIVE_ENABLE
                WIC_LOGD("app: reboot N6...");
                pwr_n6_restart(500, 100);
            #endif
                n6_comm_set_event_isr(N6_COMM_EVENT_ERR);
            #if MS_BD_KEEPLIVE_ENABLE
                last_keep_alive_time_ms = osKernelGetTickCount();
            #endif
                n6_state = N6_STATE_STARTUP;
                break;
        }
        // osDelay(1);
        task_event = osEventFlagsWait(app_task_event, APP_TASK_EVENT_FLAG_STOP, osFlagsWaitAny, 1);
        if (!(task_event & osFlagsError) && (task_event & APP_TASK_EVENT_FLAG_STOP)) {
            n6_state = N6_STATE_STOPPED;
            osEventFlagsSet(app_task_event, APP_TASK_EVENT_FLAG_STOP_ACK);
        }
    }
}

#endif

void app_init(void)
{
    uint32_t wakeup_flag = 0;

    wakeup_flag = pwr_get_wakeup_flags();
    // IF PWR_WAKEUP_FLAG_CONFIG_KEY, we should wait for the key released
    if (wakeup_flag & PWR_WAKEUP_FLAG_CONFIG_KEY) {
        pwr_wait_for_key_release();
    }
    pwr_ctrl_bits(PWR_DEFAULT_SWITCH_BITS);
    n6_comm_init();
    n6_comm_set_recv_callback(n6_comm_recv_callback);
#if APP_IS_USER_STR_CMD == 0
    app_task_event = osEventFlagsNew(NULL);
    g_ms_bridging_handler = ms_bridging_init(n6_comm_send, ms_bridging_notify_callback);
    xTaskCreate(ms_bridging_polling_task, MS_BD_TASK_NAME, MS_BD_TASK_STACK_SIZE, NULL, MS_BD_TASK_PRIORITY, NULL);
#endif
    xTaskCreate(app_task, APP_TASK_NAME, APP_TASK_STACK_SIZE, NULL, APP_TASK_PRIORITY, NULL);
    WIC_LOGD("app_init ok!");
}
