/* Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public Semtech HAL, bound directly to our RP2350 radio and devicetree.
 * The RF owner serializes calls. The launch alarm is armed only after a call
 * returns, so its five-byte trigger never overlaps a transfer here.
 */
#include "platform/slot_timer.h"
#include "platform/radio.h"
#include "lr20xx_hal.h"

#include <hardware/gpio.h>
#include <hardware/structs/spi.h>
#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/kernel.h>

#define RADIO_NODE  DT_CHOSEN(zephyr_lorawan_transceiver)
#define CS_PIN      DT_SPI_DEV_CS_GPIOS_PIN(RADIO_NODE)
#define BUSY_PIN    DT_GPIO_PIN(RADIO_NODE, busy_gpios)
#define POLL_MAX    32U
#define IO_MAX      544U
#define DUMMY_BYTES 2U

BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_BUS(RADIO_NODE), arm_pl022));
BUILD_ASSERT((DT_SPI_DEV_CS_GPIOS_FLAGS(RADIO_NODE) & GPIO_ACTIVE_LOW) != 0U);
BUILD_ASSERT(CS_PIN < 32U && BUSY_PIN < 32U);

static const struct device *const radio_dev = DEVICE_DT_GET(RADIO_NODE);
static const struct spi_dt_spec radio_spi = SPI_DT_SPEC_GET(RADIO_NODE, SPI_WORD_SET(8), 0);
static const struct gpio_dt_spec reset_gpio = GPIO_DT_SPEC_GET(RADIO_NODE, reset_gpios);
static spi_hw_t *const spi = (spi_hw_t *)DT_REG_ADDR(DT_BUS(RADIO_NODE));
static bool spi_configured;
static bool asleep;
static uint8_t tx_bytes[IO_MAX];
static uint8_t rx_bytes[IO_MAX];
static const struct device *const dma = DEVICE_DT_GET(DT_DMAS_CTLR_BY_NAME(DT_BUS(RADIO_NODE), tx));
static int tx_channel = -1;
static int rx_channel = -1;
static K_SEM_DEFINE(dma_done, 0, 1);
static volatile int dma_result;
static volatile uint32_t dma_irq_us;
static uint32_t busy_us_last;
static struct zlrs_radio_io_diag io_diag;
static struct dma_block_config dma_blocks[2];
static struct dma_config dma_configs[2];

static void dma_complete(const struct device *dev, void *user, uint32_t channel, int result)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user);
	ARG_UNUSED(channel);
	dma_result = result;
	dma_irq_us = zlrs_slot_timer_now_us();
	k_sem_give(&dma_done);
}

static int dma_init(void)
{
	if (rx_channel >= 0) {
		return 0;
	}
	/* Leave the generic SPI driver's devicetree channels available to it. */
	uint32_t mask = ~(BIT(DT_DMAS_CELL_BY_NAME(DT_BUS(RADIO_NODE), tx, channel)) |
			  BIT(DT_DMAS_CELL_BY_NAME(DT_BUS(RADIO_NODE), rx, channel)));

	tx_channel = dma_request_channel(dma, &mask);
	if (tx_channel < 0) {
		return tx_channel;
	}
	rx_channel = dma_request_channel(dma, &mask);
	if (rx_channel < 0) {
		dma_release_channel(dma, tx_channel);
		tx_channel = -1;
		return rx_channel;
	}
	dma_blocks[0] = (struct dma_block_config){
		.source_address = (uint32_t)(uintptr_t)tx_bytes,
		.dest_address = (uint32_t)(uintptr_t)&spi->dr,
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
	};
	dma_blocks[1] = (struct dma_block_config){
		.source_address = (uint32_t)(uintptr_t)&spi->dr,
		.dest_address = (uint32_t)(uintptr_t)rx_bytes,
		.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
		.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT,
	};
	for (size_t i = 0U; i < ARRAY_SIZE(dma_configs); i++) {
		dma_configs[i] = (struct dma_config){
			.channel_direction = i == 0U ? MEMORY_TO_PERIPHERAL : PERIPHERAL_TO_MEMORY,
			.dma_slot = i == 0U ? DT_DMAS_CELL_BY_NAME(DT_BUS(RADIO_NODE), tx, slot)
					    : DT_DMAS_CELL_BY_NAME(DT_BUS(RADIO_NODE), rx, slot),
			.source_data_size = 1U,
			.dest_data_size = 1U,
			.source_burst_length = 1U,
			.dest_burst_length = 1U,
			.block_count = 1U,
			.head_block = &dma_blocks[i],
			.dma_callback = i == 1U ? dma_complete : NULL,
		};
	}
	return 0;
}

static int dma_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
	const uint32_t start_us = zlrs_slot_timer_now_us();
	const uint32_t ready_us = busy_us_last;

	if (len > IO_MAX || dma_init() != 0) {
		io_diag.dma_errors++;
		return -EIO;
	}
	if (tx == NULL) {
		memset(tx_bytes, 0, len);
	} else if (tx != tx_bytes) {
		memcpy(tx_bytes, tx, len);
	}
	dma_blocks[0].block_size = len;
	dma_blocks[1].block_size = len;
	if (dma_config(dma, tx_channel, &dma_configs[0]) != 0 ||
	    dma_config(dma, rx_channel, &dma_configs[1]) != 0) {
		io_diag.dma_errors++;
		return -EIO;
	}
	k_sem_reset(&dma_done);
	dma_result = 0;
	spi->dmacr = SPI_SSPDMACR_TXDMAE_BITS | SPI_SSPDMACR_RXDMAE_BITS;
	gpio_put(CS_PIN, false);
	int result = dma_start(dma, rx_channel);
	const uint32_t active_us = zlrs_slot_timer_now_us();

	if (result == 0) {
		result = dma_start(dma, tx_channel);
	}
	if (result == 0) {
		result = k_sem_take(&dma_done, K_MSEC(2));
		if (result == 0) {
			result = dma_result;
		}
	}
	if (result != 0) {
		(void)dma_stop(dma, tx_channel);
		(void)dma_stop(dma, rx_channel);
	}
	const uint32_t finish = zlrs_slot_timer_now_us();

	while ((spi->sr & SPI_SSPSR_BSY_BITS) != 0U) {
		if (zlrs_slot_timer_now_us() - finish > 1000U) {
			result = -ETIMEDOUT;
			break;
		}
	}
	gpio_put(CS_PIN, true);
	if (result == 0 && rx != NULL && rx != rx_bytes) {
		memcpy(rx, rx_bytes, len);
	}
	if (result == 0) {
		io_diag.dma_transfers++;
		io_diag.dma_bytes += len;
		io_diag.dma_len_last = len;
		io_diag.setup_us_last = active_us - start_us;
		io_diag.active_us_last = dma_irq_us - active_us;
		io_diag.wake_us_last = finish - dma_irq_us;
		io_diag.finish_us_last = zlrs_slot_timer_now_us() - finish;
		io_diag.busy_us_last = ready_us;
		io_diag.setup_us_sum += io_diag.setup_us_last;
		io_diag.active_us_sum += io_diag.active_us_last;
		io_diag.wake_us_sum += io_diag.wake_us_last;
		io_diag.finish_us_sum += io_diag.finish_us_last;
		io_diag.busy_us_sum += ready_us;
	} else {
		io_diag.dma_errors++;
	}
	return result;
}

static bool ready(const void *context)
{
	if (context != radio_dev) {
		return false;
	}
	if (asleep) {
		gpio_put(CS_PIN, false);
		gpio_put(CS_PIN, true);
		asleep = false;
	}
	const uint32_t start = zlrs_slot_timer_now_us();
	const uint32_t timeout =
		CONFIG_LORA_BASICS_MODEM_DRIVERS_HAL_WAIT_ON_BUSY_TIMEOUT_MSEC * 1000U;

	while (gpio_get(BUSY_PIN)) {
		if (zlrs_slot_timer_now_us() - start > timeout) {
			return false;
		}
	}
	busy_us_last = zlrs_slot_timer_now_us() - start;
	return true;
}

void zlrs_radio_io_diag(struct zlrs_radio_io_diag *out)
{
	const struct device *const clk = DEVICE_DT_GET(DT_CLOCKS_CTLR(DT_BUS(RADIO_NODE)));
	const clock_control_subsys_t clk_id =
		(clock_control_subsys_t)DT_CLOCKS_CELL_BY_IDX(DT_BUS(RADIO_NODE), 0, clk_id);
	uint32_t peri_hz;
	const uint32_t divider =
		spi->cpsr * (((spi->cr0 & SPI_SSPCR0_SCR_BITS) >> SPI_SSPCR0_SCR_LSB) + 1U);

	*out = io_diag;
	if (divider > 0U && clock_control_get_rate(clk, clk_id, &peri_hz) == 0) {
		out->spi_hz = peri_hz / divider;
	}
}

static int __ramfunc poll_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
	const uint32_t start = zlrs_slot_timer_now_us();
	size_t sent = 0U;
	size_t received = 0U;
	int result = 0;

	if ((spi->sr & (SPI_SSPSR_BSY_BITS | SPI_SSPSR_RNE_BITS)) != 0U) {
		return -EBUSY;
	}
	gpio_put(CS_PIN, false);
	while (received < len) {
		if (sent < len && (spi->sr & SPI_SSPSR_TNF_BITS) != 0U) {
			spi->dr = tx != NULL ? tx[sent] : 0U;
			sent++;
		}
		if ((spi->sr & SPI_SSPSR_RNE_BITS) != 0U) {
			const uint8_t byte = (uint8_t)spi->dr;

			if (rx != NULL) {
				rx[received] = byte;
			}
			received++;
		}
		if (zlrs_slot_timer_now_us() - start > 1000U) {
			result = -ETIMEDOUT;
			break;
		}
	}
	while (result == 0 && (spi->sr & SPI_SSPSR_BSY_BITS) != 0U) {
		if (zlrs_slot_timer_now_us() - start > 1000U) {
			result = -ETIMEDOUT;
		}
	}
	gpio_put(CS_PIN, true);
	return result;
}

static int transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
	if (spi_configured) {
		return len <= POLL_MAX ? poll_transfer(tx, rx, len) : dma_transfer(tx, rx, len);
	}
	const struct spi_buf tx_buf = {.buf = (void *)tx, .len = len};
	const struct spi_buf rx_buf = {.buf = rx, .len = len};
	const struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1U};
	const struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1U};
	const int result = spi_transceive_dt(&radio_spi, &tx_set, &rx_set);

	spi_configured |= result == 0;
	return result;
}

lr20xx_hal_status_t lr20xx_hal_reset(const void *context)
{
	if (context != radio_dev || gpio_pin_set_dt(&reset_gpio, 1) != 0) {
		return LR20XX_HAL_STATUS_ERROR;
	}
	k_usleep(100);
	if (gpio_pin_set_dt(&reset_gpio, 0) != 0) {
		return LR20XX_HAL_STATUS_ERROR;
	}
	k_usleep(3500);
	asleep = false;
	return LR20XX_HAL_STATUS_OK;
}

lr20xx_hal_status_t lr20xx_hal_wakeup(const void *context)
{
	return ready(context) ? LR20XX_HAL_STATUS_OK : LR20XX_HAL_STATUS_ERROR;
}

lr20xx_hal_status_t lr20xx_hal_write(const void *context, const uint8_t *command,
				     uint16_t command_length, const uint8_t *data,
				     uint16_t data_length)
{
	const size_t len = (size_t)command_length + data_length;

	if (command_length < 2U || len > IO_MAX || !ready(context)) {
		return LR20XX_HAL_STATUS_ERROR;
	}
	memcpy(tx_bytes, command, command_length);
	if (data_length > 0U) {
		memcpy(tx_bytes + command_length, data, data_length);
	}
	if (transfer(tx_bytes, NULL, len) != 0) {
		return LR20XX_HAL_STATUS_ERROR;
	}
	if (command[0] == 0x01U && command[1] == 0x27U) {
		asleep = true;
		k_usleep(500);
	}
	return LR20XX_HAL_STATUS_OK;
}

lr20xx_hal_status_t lr20xx_hal_read(const void *context, const uint8_t *command,
				    uint16_t command_length, uint8_t *data, uint16_t data_length)
{
	const size_t len = DUMMY_BYTES + (size_t)data_length;

	if (len > IO_MAX || !ready(context) || transfer(command, NULL, command_length) != 0) {
		return LR20XX_HAL_STATUS_ERROR;
	}
	if (data_length > 0U) {
		if (!ready(context) || transfer(NULL, rx_bytes, len) != 0) {
			return LR20XX_HAL_STATUS_ERROR;
		}
		memcpy(data, rx_bytes + DUMMY_BYTES, data_length);
	}
	return LR20XX_HAL_STATUS_OK;
}

lr20xx_hal_status_t lr20xx_hal_direct_read(const void *context, uint8_t *data, uint16_t data_length)
{
	if (!ready(context) || transfer(NULL, data, data_length) != 0) {
		return LR20XX_HAL_STATUS_ERROR;
	}
	return LR20XX_HAL_STATUS_OK;
}

lr20xx_hal_status_t lr20xx_hal_direct_read_fifo(const void *context, const uint8_t *command,
						uint16_t command_length, uint8_t *data,
						uint16_t data_length)
{
	const size_t len = (size_t)command_length + data_length;

	if (command_length != DUMMY_BYTES || len > IO_MAX || !ready(context)) {
		return LR20XX_HAL_STATUS_ERROR;
	}
	memcpy(tx_bytes, command, command_length);
	/* LR2021 requires NOPs on MOSI while reading. Never echo stale RX data. */
	memset(tx_bytes + command_length, 0, data_length);
	if (transfer(tx_bytes, rx_bytes, len) != 0) {
		return LR20XX_HAL_STATUS_ERROR;
	}
	memcpy(data, rx_bytes + command_length, data_length);
	return LR20XX_HAL_STATUS_OK;
}
