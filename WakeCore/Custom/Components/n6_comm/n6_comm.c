#include "usart.h"
#include "n6_comm.h"

static TaskHandle_t n6_comm_task_handle = NULL;
static SemaphoreHandle_t n6_comm_mutex = NULL;
static EventGroupHandle_t n6_comm_event_group = NULL;
static n6_comm_recv_callback_t n6_comm_recv_callback = NULL;
static uint8_t n6_comm_rx_buffer[N6_COMM_MAX_LEN];

static void n6_comm_task(void *pvParameters)
{
    uint16_t rlen = 0;
    EventBits_t event = 0;
    HAL_StatusTypeDef ret = HAL_ERROR;
    
    ret = HAL_UARTEx_ReceiveToIdle_DMA(&hlpuart2, n6_comm_rx_buffer, (N6_COMM_MAX_LEN - 1));
    while (1) {
        if (ret != HAL_OK) {
            ret = HAL_UARTEx_ReceiveToIdle_DMA(&hlpuart2, n6_comm_rx_buffer, (N6_COMM_MAX_LEN - 1));
            if (ret != HAL_OK) {
                HAL_UART_AbortReceive(&hlpuart2);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            event = xEventGroupWaitBits(n6_comm_event_group, N6_COMM_EVENT_RX_DONE | N6_COMM_EVENT_ERR, pdTRUE, pdFALSE, portMAX_DELAY);
            if (event & N6_COMM_EVENT_RX_DONE) {
                rlen = (N6_COMM_MAX_LEN - 1) - hlpuart2.RxXferCount;
                if (rlen > 0 && n6_comm_recv_callback) {
                    n6_comm_rx_buffer[rlen] = 0x00;
                    n6_comm_recv_callback(n6_comm_rx_buffer, rlen);
                }
                ret = HAL_UARTEx_ReceiveToIdle_DMA(&hlpuart2, n6_comm_rx_buffer, (N6_COMM_MAX_LEN - 1));
                if (ret != HAL_OK) {
                    HAL_UART_AbortReceive(&hlpuart2);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            if (event & N6_COMM_EVENT_ERR) {
                WIC_LOGE("hlpuart2 error(0x%02X).", hlpuart2.ErrorCode);
                HAL_UART_AbortReceive(&hlpuart2);
                ret = HAL_UARTEx_ReceiveToIdle_DMA(&hlpuart2, n6_comm_rx_buffer, (N6_COMM_MAX_LEN - 1));
            }
        }
    }
}


int n6_comm_init(void)
{
    int ret = 0;
    if (n6_comm_task_handle != NULL) return SYS_ERR_INVALID_STATE;

    n6_comm_mutex = xSemaphoreCreateMutex();
    if (n6_comm_mutex == NULL) {
        ret = SYS_ERR_NO_MEM;
        goto n6_comm_init_fail;
    }

    n6_comm_event_group = xEventGroupCreate();
    if (n6_comm_event_group == NULL) {
        ret = SYS_ERR_NO_MEM;
        goto n6_comm_init_fail;
    }

    if (xTaskCreate(n6_comm_task, N6_COMM_TASK_NAME, N6_COMM_TASK_STACK_SIZE, NULL, N6_COMM_TASK_PRIORITY, &n6_comm_task_handle) != pdPASS) {
        ret = SYS_ERR_NO_MEM;
        goto n6_comm_init_fail;
    }

    memset(n6_comm_rx_buffer, 0, (N6_COMM_MAX_LEN - 1));
    return SYS_OK;
n6_comm_init_fail:
    n6_comm_deinit();
    return ret;
}

int n6_comm_send(uint8_t *wbuf, uint16_t wlen, uint32_t timeout_ms)
{
    EventBits_t event = 0;
    if (n6_comm_mutex == NULL) return SYS_ERR_INVALID_STATE;
    if (wbuf == NULL || wlen == 0 || wlen > N6_COMM_MAX_LEN) return SYS_ERR_INVALID_ARG;

    if (xSemaphoreTake(n6_comm_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return SYS_ERR_MUTEX;
    }

    if (HAL_UART_Transmit_DMA(&hlpuart2, wbuf, wlen) != HAL_OK) {
        WIC_LOGE("HAL_UART_Transmit_DMA error");
        HAL_UART_AbortTransmit(&hlpuart2);
        xSemaphoreGive(n6_comm_mutex);
        return SYS_ERR_HAL;
    }

    event = xEventGroupWaitBits(n6_comm_event_group, N6_COMM_EVENT_TX_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (!(event & N6_COMM_EVENT_TX_DONE)) {
        WIC_LOGE("n6_comm_send timeout, event = 0x%08lX", event);
        HAL_UART_AbortTransmit(&hlpuart2);
        xSemaphoreGive(n6_comm_mutex);
        return SYS_ERR_TIMEOUT;
    }
    
    xSemaphoreGive(n6_comm_mutex);
    return SYS_OK;
}

int n6_comm_send_str(const char *str)
{
    return n6_comm_send((uint8_t *)str, strlen(str), 1000);
}

void n6_comm_set_recv_callback(n6_comm_recv_callback_t callback)
{
    n6_comm_recv_callback = callback;
}

void n6_comm_deinit(void)
{
    if (n6_comm_task_handle != NULL) {
        vTaskDelete(n6_comm_task_handle);
        n6_comm_task_handle = NULL;
    }

    if (n6_comm_mutex != NULL) {
        xSemaphoreTake(n6_comm_mutex, portMAX_DELAY);
        vSemaphoreDelete(n6_comm_mutex);
        n6_comm_mutex = NULL;
    }

    if (n6_comm_event_group != NULL) {
        vEventGroupDelete(n6_comm_event_group);
        n6_comm_event_group = NULL;
    }

    n6_comm_recv_callback = NULL;
}

void n6_comm_set_event_isr(uint32_t event)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (n6_comm_event_group == NULL) return;

    xEventGroupSetBitsFromISR(n6_comm_event_group, event, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

