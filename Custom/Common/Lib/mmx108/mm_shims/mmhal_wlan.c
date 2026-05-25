/*
 * Copyright 2021-2025 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mmhal_wlan.h"
#include "mmosal.h"
//#include "mmconfig.h"
#include "mmutils.h"

#include "main.h"

/**
 * The number of bytes to send as part of @ref mmhal_wlan_send_training_seq for SD over SPI.
 * This must be at least 74 bits (>= 10 bytes), see section 6.4.1.1 of the SD spec
 * "Physical Layer Simplified Specification Version 9.10" for more detail.
 */
#define BYTE_TRAIN 16

MM_STATIC_ASSERT((BYTE_TRAIN >= 10), "BYTE_TRAIN must be at least 10.");

/** Binary Semaphore for indicating DMA transaction complete */
static struct mmosal_semb *dma_semb_handle;
/** Maximum time we expect a DMA transaction to take (for the maximum block size) in ms. */
#define DMA_WAIT_TMO (10)

/** Minimum length of data to be transferred we require before we do a DMA transfer. */
#define DMA_TRANSFER_MIN_LENGTH (-1)

/** SPI hw interrupt handler. Must be set before enabling irq */
static mmhal_irq_handler_t spi_irq_handler = NULL;

/** busy interrupt handler. Must be set before enabling irq */
static mmhal_irq_handler_t busy_irq_handler = NULL;

extern SPI_HandleTypeDef hspi6;


void mmhal_wlan_hard_reset(void)
{
	HAL_GPIO_WritePin(MM_HALOW_RESET_GPIO_Port, MM_HALOW_RESET_Pin, GPIO_PIN_RESET);
	
	mmosal_task_sleep(5);

	HAL_GPIO_WritePin(MM_HALOW_RESET_GPIO_Port, MM_HALOW_RESET_Pin, GPIO_PIN_SET);

	mmosal_task_sleep(20);
}

#if defined(ENABLE_EXT_XTAL_INIT) && ENABLE_EXT_XTAL_INIT
bool mmhal_wlan_ext_xtal_init_is_required(void)
{
    return true;
}
#endif

void mmhal_wlan_spi_cs_assert(void)
{
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_RESET);
}

void mmhal_wlan_spi_cs_deassert(void)
{
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_SET);
}

uint8_t mmhal_wlan_spi_rw(uint8_t data)
{
	HAL_StatusTypeDef status = HAL_OK;
	uint8_t read_data = 0;

	status = HAL_SPI_TransmitReceive(&hspi6, &data, &read_data, 1, 100);
	if (status != HAL_OK) 
	{
		printf("HAL_SPI_TransmitReceive failed! (status = %d)!\r\n", status);
	}

	return read_data;
}

static void mmhal_wlan_spi_read_buf_dma(uint8_t *buf, unsigned len)
{
	if (HAL_SPI_Receive_DMA(&hspi6, buf, len) != HAL_OK)
	{
		printf("HAL_SPI_Receive_DMA failed!\r\n");
	}
}

void mmhal_wlan_spi_read_buf(uint8_t *buf, unsigned len)
{
	if (len < DMA_TRANSFER_MIN_LENGTH)
	{
		uint8_t *tx_data = NULL;
	
		tx_data = (uint8_t *)malloc(len);
		if (tx_data == NULL)
		{
			printf("[%s-%d]malloc %d failed!\r\n", __FUNCTION__, __LINE__, len);
			return;
		}			
	
		memset(tx_data, 0xFF, len);
		
		HAL_SPI_TransmitReceive(&hspi6, tx_data, buf, len, 10);

		free(tx_data);
	}
	else
	{
		mmhal_wlan_spi_read_buf_dma(buf, len);
	}

}

static void mmhal_wlan_spi_write_buf_dma(const uint8_t *buf, unsigned len)
{
	if (HAL_SPI_Transmit_DMA(&hspi6, buf, len) != HAL_OK)
	{
		printf("HAL_SPI_Transmit_DMA failed!\r\n");
	}
}

void mmhal_wlan_spi_write_buf(const uint8_t *buf, unsigned len)
{
	if (len < DMA_TRANSFER_MIN_LENGTH)
	{
		uint8_t *rx_data = NULL;
	
		rx_data = (uint8_t *)malloc(len);
		if (rx_data == NULL)
		{
			printf("[%s-%d]malloc %d failed!\r\n", __FUNCTION__, __LINE__, len);
			return;
		}			
	
		memset(rx_data, 0, len);
		
		HAL_SPI_TransmitReceive(&hspi6, buf, rx_data, len, 100);

		free(rx_data);
	}
	else
	{
		mmhal_wlan_spi_write_buf_dma(buf, len);
	}
}

void mmhal_wlan_send_training_seq(void)
{
	uint8_t *tx_data = NULL;
	uint8_t *rx_data = NULL;
	int data_len = 1024;

	mmhal_wlan_spi_cs_deassert();
	
	tx_data = (uint8_t *)malloc(data_len);
	if (tx_data == NULL)
	{
		printf("[%s-%d]malloc %d failed!\r\n", __FUNCTION__, __LINE__, data_len);
		return;
	}			

	memset(tx_data, 0xFF, data_len);

	rx_data = (uint8_t *)malloc(data_len);
	if (rx_data == NULL)
	{
		printf("[%s-%d]malloc %d failed!\r\n", __FUNCTION__, __LINE__, data_len);
		return;
	}			

	memset(rx_data, 0, data_len);
	
	HAL_SPI_TransmitReceive(&hspi6, tx_data, rx_data, data_len, 10);

	free(tx_data);
	free(rx_data);	
}

void mmhal_wlan_register_spi_irq_handler(mmhal_irq_handler_t handler)
{
    spi_irq_handler = handler;
}

bool mmhal_wlan_spi_irq_is_asserted(void)
{
	GPIO_PinState state = GPIO_PIN_RESET;
	
	state = HAL_GPIO_ReadPin(MM_HALOW_SPI_IRQ_GPIO_Port, MM_HALOW_SPI_IRQ_Pin);
	
    return state == GPIO_PIN_RESET ? true : false;
}

void mmhal_wlan_set_spi_irq_enabled(bool enabled)
{
	if (enabled)
	{
		HAL_NVIC_SetPriority(MM_HALOW_SPI_IRQn, 5, 0);
		HAL_NVIC_EnableIRQ(MM_HALOW_SPI_IRQn);
		
		if (mmhal_wlan_spi_irq_is_asserted())
        {			
            if (spi_irq_handler != NULL)
            {
                spi_irq_handler();
            }
        }
	}
	else
	{
		HAL_NVIC_DisableIRQ(MM_HALOW_SPI_IRQn);
	}
}

void mmhal_wlan_init(void)
{
    dma_semb_handle = mmosal_semb_create("dma_semb_handle");

}

void mmhal_wlan_deinit(void)
{
    mmosal_semb_delete(dma_semb_handle);

}

void mmhal_wlan_wake_assert(void)
{
	HAL_GPIO_WritePin(MM_HALOW_WAKE_GPIO_Port, MM_HALOW_WAKE_Pin, GPIO_PIN_SET);
}

void mmhal_wlan_wake_deassert(void)
{
	HAL_GPIO_WritePin(MM_HALOW_WAKE_GPIO_Port, MM_HALOW_WAKE_Pin, GPIO_PIN_RESET);
}

bool mmhal_wlan_busy_is_asserted(void)
{
	GPIO_PinState state = GPIO_PIN_RESET;
	
	state = HAL_GPIO_ReadPin(MM_HALOW_BUSY_GPIO_Port, MM_HALOW_BUSY_Pin);
	
    return state == GPIO_PIN_SET ? true : false;

}

void mmhal_wlan_register_busy_irq_handler(mmhal_irq_handler_t handler)
{
    busy_irq_handler = handler;
}

void mmhal_wlan_set_busy_irq_enabled(bool enabled)
{
	if (enabled)
	{
		HAL_NVIC_SetPriority(MM_HALOW_BUSY_IRQn, 6, 0);
		HAL_NVIC_EnableIRQ(MM_HALOW_BUSY_IRQn);
		
		if (mmhal_wlan_busy_is_asserted())
        {
            if (busy_irq_handler != NULL)
            {
                busy_irq_handler();
            }
        }
	}
	else
	{
		HAL_NVIC_DisableIRQ(MM_HALOW_BUSY_IRQn);
	}
}

/**
 * @brief This function handles BUSY interrupt.
 */
void MM_HALOW_BUSY_IRQ_HANDLER(void)
{
	HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_15);

	if (busy_irq_handler != NULL)
    {
        busy_irq_handler();
    }
}

/**
 * @brief This function handles SPI IRQ interrupts.
 */
void MM_HALOW_SPI_IRQ_HANDLER(void)
{
	HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_4);

	if (spi_irq_handler != NULL)
    {
        spi_irq_handler();
    }
}

void mmhal_read_mac_addr(uint8_t *mac_addr)
{
	mac_addr[0] = 0x0c;
	mac_addr[1] = 0xc0;
	mac_addr[2] = 0x01;
	mac_addr[3] = 0x10;
	mac_addr[4] = 0x02;
	mac_addr[5] = 0x03;
}

const struct mmhal_chip *mmhal_get_chip(void)
{
    /* This is a define that is set by the build system in the platform-xxx.mk file to select the
     * Morse chip type. See the API documentation for mmhal_get_chip() for more information. */
    return &mmhal_mm6108;
}
