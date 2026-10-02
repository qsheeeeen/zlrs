/* Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public Semtech HAL, bound directly to our RP2350 radio and devicetree.
 * The RF owner serializes calls. The launch alarm is armed only after a call
 * returns, so its five-byte trigger never overlaps a transfer here.
 */
#include "platform/slot_timer.h"
#include "lr20xx_hal.h"

#include <hardware/gpio.h>
#include <hardware/structs/spi.h>
#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
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
	return true;
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
	if (spi_configured && len <= POLL_MAX) {
		return poll_transfer(tx, rx, len);
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
