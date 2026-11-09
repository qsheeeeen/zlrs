/* Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * One hardware transaction owned by RF. RAL and the LR20xx HAL are reused; RAC and the
 * radio planner do not participate in initialization, IRQs or transactions.
 */
#include "platform/lr2021.h"
#include "platform/slot_timer.h"

#include "trace.h"
#include <hardware/gpio.h>
#include <hardware/structs/spi.h>
#include <string.h>
#include <zephyr/dt-bindings/gpio/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usp/lora_lbm_transceiver.h>

#include "lr20xx_hal.h"
#include "lr20xx_radio_common.h"
#include "lr20xx_radio_fifo.h"
#include "lr20xx_system.h"
#include "ral_lr20xx_bsp.h"
#include "ralf_lr20xx.h"

LOG_MODULE_REGISTER(zlrs_lr2021, CONFIG_APP_LOG_LEVEL);

static const struct device *const transceiver =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_lorawan_transceiver));
static ralf_t radio = RALF_LR20XX_INSTANTIATE(DEVICE_DT_GET(DT_CHOSEN(zephyr_lorawan_transceiver)));
static struct k_sem *owner_wake;
static const zlrs_radio_req_t *request;

enum radio_job_state {
	RADIO_IDLE,
	RADIO_IDLE_FS,
	RADIO_PREPARED,
	RADIO_ARMED,
	RADIO_ACTIVE,
};
static volatile enum radio_job_state job_state;
static volatile bool irq_pending;
static volatile uint32_t irq_us;
static volatile uint32_t begin_us;
static uint32_t slot_end_us;
static uint32_t tx_at_us;

static struct modem_config {
	zlrs_phy_t phy;
	uint32_t frequency;
	uint16_t length;
	uint8_t slot;
	uint8_t lora_sync;
} modem = {.phy = ZLRS_PHY_COUNT};

static bool sync_valid[3];
static uint8_t sync_words[3][ZLRS_SYNC_WORD_LEN];
static bool tx_cfg_valid;
static uint32_t tx_cfg_freq;
static int8_t tx_cfg_power;
static zlrs_lr2021_diag_t diag;

static const ral_flrc_raw_bit_rate_t flrc_rates[ZLRS_PHY_COUNT] = {
	[ZLRS_PHY_FLRC_2600] = RAL_FLRC_RAW_BIT_RATE_2_600_MBPS,
	[ZLRS_PHY_FLRC_650] = RAL_FLRC_RAW_BIT_RATE_0_650_MBPS,
	[ZLRS_PHY_FLRC_325] = RAL_FLRC_RAW_BIT_RATE_0_325_MBPS,
	[ZLRS_PHY_FLRC_1300] = RAL_FLRC_RAW_BIT_RATE_1_300_MBPS,
};

static void radio_irq(const struct device *dev)
{
	ARG_UNUSED(dev);
	if (job_state == RADIO_ACTIVE && !irq_pending) {
		irq_us = zlrs_slot_timer_now_us();
		irq_pending = true;
	}
	k_sem_give(owner_wake);
}

void zlrs_lr2021_slot(uint32_t tx_us, uint32_t end_us)
{
	tx_at_us = tx_us;
	slot_end_us = end_us;
	if ((slot_end_us != 0U)) {
		zlrs_slot_timer_deadline(end_us);
	}
}

static void make_flrc(zlrs_phy_t phy, uint16_t len, uint8_t slot, ralf_params_flrc_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->mod_params.raw_bit_rate = flrc_rates[phy];
	cfg->mod_params.cr = RAL_FLRC_CR_3_4;
	cfg->mod_params.pulse_shape = RAL_FLRC_PULSE_SHAPE_BT_05;
	cfg->pkt_params.preamble_len = (ZLRS_FLRC_PREAMBLE_BITS / 4U) - 1U;
	cfg->pkt_params.tx_syncword =
		slot == ZLRS_SYNC_SLOT_PAIRING ? RAL_FLRC_TX_SYNCWORD_2 : RAL_FLRC_TX_SYNCWORD_1;
	cfg->pkt_params.match_sync_word = slot == ZLRS_SYNC_SLOT_PAIRING
						  ? RAL_FLRC_RX_MATCH_SYNCWORD_2
						  : RAL_FLRC_RX_MATCH_SYNCWORD_1;
	cfg->pkt_params.sync_word_len = RAL_FLRC_SYNCWORD_LENGTH_4_BYTES;
	cfg->pkt_params.pld_is_fix = false;
	cfg->pkt_params.pld_len_in_bytes = len;
	cfg->pkt_params.crc_type = RAL_FLRC_CRC_2_BYTES;
}

static void make_lora(zlrs_phy_t phy, uint16_t len, ralf_params_lora_t *cfg)
{
	const zlrs_phy_spec_t *mod = &zlrs_phys[phy];
	memset(cfg, 0, sizeof(*cfg));
	cfg->mod_params.sf = mod->lora_sf == 6U ? RAL_LORA_SF6 : RAL_LORA_SF7;
	cfg->mod_params.bw = RAL_LORA_BW_1000_KHZ;
	cfg->mod_params.cr = RAL_LORA_CR_4_5;
	cfg->pkt_params.preamble_len_in_symb = ZLRS_LORA_PREAMBLE_SYMBOLS;
	cfg->pkt_params.header_type = RAL_LORA_PKT_EXPLICIT;
	cfg->pkt_params.pld_len_in_bytes = MIN(len, ZLRS_LORA_FRAME_MAX_LEN);
	cfg->pkt_params.crc_is_on = true;
}

static int setup_tx_power(const zlrs_radio_req_t *req)
{
	if (!tx_cfg_valid || tx_cfg_power != req->tx_power_dbm ||
	    (tx_cfg_freq >= 1500000000U) != (req->freq_hz >= 1500000000U)) {
		if (ral_set_tx_cfg(&radio.ral, req->tx_power_dbm, req->freq_hz) != RAL_STATUS_OK) {
			return -EIO;
		}
		tx_cfg_valid = true;
		tx_cfg_freq = req->freq_hz;
		tx_cfg_power = req->tx_power_dbm;
	}
	return 0;
}

static int setup_frequency(uint32_t freq_hz)
{
	lr20xx_radio_common_rx_path_t path;
	lr20xx_radio_common_rx_path_boost_mode_t boost;

	ral_lr20xx_bsp_get_rx_cfg(radio.ral.context, freq_hz, &path, &boost);
	/* Select the band before changing frequency, avoiding calibration on the old RX path. */
	if (lr20xx_radio_common_set_rx_path(radio.ral.context, path, boost) != LR20XX_STATUS_OK ||
	    lr20xx_radio_common_set_rf_freq(radio.ral.context, freq_hz) != LR20XX_STATUS_OK) {
		return -EIO;
	}
	return 0;
}

static int setup_radio(const zlrs_radio_req_t *req, const struct modem_config *next)
{
	const bool lora = zlrs_phys[next->phy].lora_sf != 0U;
	const bool full =
		modem.phy == ZLRS_PHY_COUNT || (lora != (zlrs_phys[modem.phy].lora_sf != 0U));
	const bool frequency = full || next->frequency != modem.frequency;
	const bool modulation = full || next->phy != modem.phy;
	const bool packet = full || next->length != modem.length || next->slot != modem.slot;
	const bool sync = full || next->lora_sync != modem.lora_sync;

	/* A partially programmed radio must be configured fully on the next request. */
	modem.phy = ZLRS_PHY_COUNT;
	if ((full && ral_set_pkt_type(&radio.ral, lora ? RAL_PKT_TYPE_LORA : RAL_PKT_TYPE_FLRC) !=
			     RAL_STATUS_OK) ||
	    (frequency && setup_frequency(next->frequency) != 0)) {
		return -EIO;
	}
	if (lora) {
		ralf_params_lora_t cfg;
		make_lora(next->phy, next->length, &cfg);
		if ((modulation &&
		     ral_set_lora_mod_params(&radio.ral, &cfg.mod_params) != RAL_STATUS_OK) ||
		    (packet &&
		     ral_set_lora_pkt_params(&radio.ral, &cfg.pkt_params) != RAL_STATUS_OK) ||
		    (sync &&
		     ral_set_lora_sync_word(&radio.ral, next->lora_sync) != RAL_STATUS_OK) ||
		    (full && ral_set_lora_symb_nb_timeout(&radio.ral, 0U) != RAL_STATUS_OK)) {
			return -EIO;
		}
	} else {
		ralf_params_flrc_t cfg;
		const uint8_t id = next->slot - 1U;
		make_flrc(next->phy, next->length, next->slot, &cfg);
		if ((modulation &&
		     ral_set_flrc_mod_params(&radio.ral, &cfg.mod_params) != RAL_STATUS_OK) ||
		    (packet &&
		     ral_set_flrc_pkt_params(&radio.ral, &cfg.pkt_params) != RAL_STATUS_OK) ||
		    (full && ral_set_flrc_crc_params(&radio.ral, 0U, 0U) != RAL_STATUS_OK) ||
		    ((full || !sync_valid[id] ||
		      memcmp(sync_words[id], req->sync_word, ZLRS_SYNC_WORD_LEN) != 0) &&
		     ral_set_flrc_sync_word(&radio.ral, next->slot, req->sync_word,
					    RAL_FLRC_SYNCWORD_LENGTH_4_BYTES) != RAL_STATUS_OK)) {
			return -EIO;
		}
		if (full) {
			memset(sync_valid, 0, sizeof(sync_valid));
		}
		memcpy(sync_words[id], req->sync_word, ZLRS_SYNC_WORD_LEN);
		sync_valid[id] = true;
	}
	modem = *next;
	return setup_tx_power(req);
}

static int late_tx_prepare(uint32_t target_us)
{
	diag.prepare_late_starts++;
	zlrs_timing_record(&diag.launch_late_us,
			   zlrs_slot_timer_now_us() - (target_us - ZLRS_SLOT_TIMER_LAUNCH_LEAD_US));
	return -ETIME;
}

int zlrs_lr2021_prepare(const zlrs_radio_req_t *req)
{
	const uint32_t start = zlrs_slot_timer_now_us();
	uint32_t stages[5] = {start};
	ral_irq_t stale = 0;
	int rc;

	if ((unsigned int)req->phy >= ZLRS_PHY_COUNT) {
		return -EINVAL;
	}
	if (job_state > RADIO_IDLE_FS) {
		return -EBUSY;
	}
	const bool lora = zlrs_phys[req->phy].lora_sf != 0U;
	if ((lora && req->is_tx && req->size > ZLRS_LORA_FRAME_MAX_LEN) ||
	    (!lora && (req->sync_slot < 1U || req->sync_slot > ARRAY_SIZE(sync_valid)))) {
		return -EINVAL;
	}
	const struct modem_config next = {
		.phy = req->phy,
		.frequency = req->freq_hz,
		.length = lora ? MIN(req->is_tx ? req->size : req->max_rx_size,
				     ZLRS_LORA_FRAME_MAX_LEN)
			  : req->is_tx ? req->size
				       : req->max_rx_size,
		.slot = lora ? 0U : req->sync_slot,
		.lora_sync = lora ? req->sync_word[0] ^ req->sync_word[1] ^ req->sync_word[2] ^
					     req->sync_word[3]
				  : 0U,
	};
	bool reuse_fs = job_state == RADIO_IDLE_FS && tx_cfg_valid &&
			tx_cfg_power == req->tx_power_dbm && modem.phy == next.phy &&
			modem.frequency == next.frequency && modem.length == next.length &&
			modem.slot == next.slot && modem.lora_sync == next.lora_sync &&
			(lora || (sync_valid[next.slot - 1U] &&
				  memcmp(sync_words[next.slot - 1U], req->sync_word,
					 ZLRS_SYNC_WORD_LEN) == 0));

	job_state = RADIO_IDLE;
	request = req;
	if ((!reuse_fs && ral_set_standby(&radio.ral, RAL_STANDBY_CFG_XOSC) != RAL_STATUS_OK) ||
	    ral_get_and_clear_irq_status(&radio.ral, &stale) != RAL_STATUS_OK) {
		return -EIO;
	}
	if ((stale & (RAL_IRQ_CMD_ERROR | RAL_IRQ_ERROR)) != 0U) {
		(void)lr20xx_system_get_errors(radio.ral.context, &diag.chip_errors);
		diag.chip_error_freq_hz = modem.phy == ZLRS_PHY_COUNT ? 0U : modem.frequency;
		(void)lr20xx_system_clear_errors(radio.ral.context);
		if (reuse_fs &&
		    ral_set_standby(&radio.ral, RAL_STANDBY_CFG_XOSC) != RAL_STATUS_OK) {
			return -EIO;
		}
		reuse_fs = false;
	}
	if (stale != 0U) {
		diag.stale_irq++;
		diag.stale_irq_bits = stale;
	}
	stages[1] = zlrs_slot_timer_now_us();
	rc = setup_radio(req, &next);
	if (rc != 0) {
		return rc;
	}
	/* The IRQ mask is constant and configured once by radio_init(). */
	stages[2] = zlrs_slot_timer_now_us();
	/* One finalized frame, one complete FIFO load before SET_TX. */
	if (req->is_tx) {
		const uint32_t load_started = zlrs_slot_timer_now_us();
		ZLRS_TRACE_EVENT("rf_fifo_tx_begin", req->size, 0);
		if (req->size == 0U || req->size > ZLRS_FRAME_MAX_LEN ||
		    lr20xx_radio_fifo_clear_tx(radio.ral.context) != LR20XX_STATUS_OK ||
		    ral_set_pkt_payload(&radio.ral, req->buf, req->size) != RAL_STATUS_OK) {
			diag.tx_fifo_errors++;
			return -EIO;
		}
		diag.tx_fifo_loads++;
		diag.tx_fifo_bytes += req->size;
		zlrs_timing_record(&diag.tx_fifo_load_us, zlrs_slot_timer_now_us() - load_started);
		ZLRS_TRACE_EVENT("rf_fifo_tx_end", req->size, diag.tx_fifo_load_us.last);
	}
	stages[3] = zlrs_slot_timer_now_us();
	/* A completed single RX/TX returns to FS. When all settings match, FIFO access
	 * can keep that locked synthesizer; configuration changes still use standby. */
	if ((!reuse_fs && ral_set_fs(&radio.ral) != RAL_STATUS_OK) ||
	    lr20xx_hal_wakeup(radio.ral.context) != LR20XX_HAL_STATUS_OK) {
		return -EIO;
	}
	stages[4] = zlrs_slot_timer_now_us();
	if (req->is_tx) {
		for (size_t i = 0U; i < ARRAY_SIZE(diag.tx_stage_us); i++) {
			diag.tx_stage_us[i] = stages[i + 1U] - stages[i];
		}
	}
	zlrs_timing_record(&diag.submit_us, stages[4] - start);
	if ((slot_end_us != 0U)) {
		if (req->is_tx) {
			/* TX has a fixed launch point; receivers configure before it. */
			if (!zlrs_slot_timer_launch_ready(zlrs_slot_timer_now_us(), tx_at_us,
							  ZLRS_SLOT_TIMER_LAUNCH_LEAD_US)) {
				return late_tx_prepare(tx_at_us);
			}
			if ((int32_t)(tx_at_us + ZLRS_LR2021_STARTUP_US +
				      zlrs_phy_airtime_us(req->phy, req->size) - slot_end_us) >=
			    0) {
				return -EMSGSIZE;
			}
		}
		if ((int32_t)(zlrs_slot_timer_now_us() - slot_end_us) >= 0) {
			return -ETIME;
		}
	}
	job_state = RADIO_PREPARED;
	diag.fs_reused += reuse_fs;
	if (req->is_tx) {
		const uint32_t ready_us = zlrs_slot_timer_now_us();

		diag.tx_ready_lead_us_last =
			(slot_end_us != 0U) && (int32_t)(tx_at_us - ready_us) > 0
				? tx_at_us - ready_us
				: 0U;
	}
	return 0;
}

static spi_hw_t *const launch_spi =
	(spi_hw_t *)DT_REG_ADDR(DT_BUS(DT_CHOSEN(zephyr_lorawan_transceiver)));

/* Published only after the RAL has completed every SPI/DMA preparation operation.
 * The RF owner blocks while ARMED. The ISR publishes its result before leaving ARMED. */
static volatile struct {
	uint8_t command[5];
	uint32_t latest_us;
	uint32_t irq_us;
	uint32_t command_us;
	int result;
} launch_mailbox;

static void __ramfunc launch_command_isr(uint32_t irq_time_us)
{
	uint32_t sent = 0U;
	uint32_t received = 0U;
	int result = -EIO;

	launch_mailbox.irq_us = irq_time_us;
	begin_us = zlrs_slot_timer_now_us();
	if ((int32_t)(begin_us - launch_mailbox.latest_us) > 0) {
		result = -ETIME;
		goto done;
	}
	/* The public RAL preparation leaves SPI idle and the LR2021 in FS, BUSY low.
	 * Reject an unexpected bus state rather than interfere with another transfer. */
	if (gpio_get(DT_GPIO_PIN(DT_CHOSEN(zephyr_lorawan_transceiver), busy_gpios)) ||
	    (launch_spi->sr & (SPI_SSPSR_BSY_BITS | SPI_SSPSR_RNE_BITS)) != 0U ||
	    (launch_spi->cr1 & SPI_SSPCR1_SSE_BITS) == 0U) {
		goto done;
	}
	gpio_put(DT_SPI_DEV_CS_GPIOS_PIN(DT_CHOSEN(zephyr_lorawan_transceiver)), false);
	while (received < sizeof(launch_mailbox.command)) {
		if (sent < sizeof(launch_mailbox.command) &&
		    (launch_spi->sr & SPI_SSPSR_TNF_BITS) != 0U) {
			launch_spi->dr = launch_mailbox.command[sent++];
		}
		if ((launch_spi->sr & SPI_SSPSR_RNE_BITS) != 0U) {
			(void)launch_spi->dr;
			received++;
		}
		if (zlrs_slot_timer_now_us() - begin_us > ZLRS_LR2021_TX_COMMAND_US) {
			goto release_cs;
		}
	}
	while ((launch_spi->sr & SPI_SSPSR_BSY_BITS) != 0U) {
		if (zlrs_slot_timer_now_us() - begin_us > ZLRS_LR2021_TX_COMMAND_US) {
			goto release_cs;
		}
	}
	result = 0;
release_cs:
	gpio_put(DT_SPI_DEV_CS_GPIOS_PIN(DT_CHOSEN(zephyr_lorawan_transceiver)), true);
done:
	launch_mailbox.command_us = zlrs_slot_timer_now_us() - begin_us;
	launch_mailbox.result = result;
	job_state = result == 0 ? RADIO_ACTIVE : RADIO_IDLE;
}

int zlrs_lr2021_launch(void)
{
	if (job_state != RADIO_PREPARED || request == NULL ||
	    (request->is_tx && slot_end_us == 0U)) {
		return -EINVAL;
	}
	job_state = RADIO_IDLE_FS;
	if (!request->is_tx) {
		if (slot_end_us != 0U && (int32_t)(zlrs_slot_timer_now_us() - slot_end_us) >= 0) {
			return -ETIME;
		}
		begin_us = zlrs_slot_timer_now_us();
		irq_pending = false;
		job_state = RADIO_ACTIVE;
		diag.transactions++;
		const int rc = ral_set_rx(&radio.ral, 0U);
		zlrs_timing_record(&diag.launch_cmd_us, zlrs_slot_timer_now_us() - begin_us);
		if (rc != RAL_STATUS_OK) {
			job_state = RADIO_IDLE;
			return -EIO;
		}
		return 0;
	}
	BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_BUS(DT_CHOSEN(zephyr_lorawan_transceiver)), arm_pl022));
	BUILD_ASSERT(DT_SPI_DEV_CS_GPIOS_PIN(DT_CHOSEN(zephyr_lorawan_transceiver)) < 32U &&
		     DT_GPIO_PIN(DT_CHOSEN(zephyr_lorawan_transceiver), busy_gpios) < 32U);
	BUILD_ASSERT(DT_SPI_DEV_CS_GPIOS_FLAGS(DT_CHOSEN(zephyr_lorawan_transceiver)) &
		     GPIO_ACTIVE_LOW);
	uint32_t latest_us;
	if (!zlrs_lr2021_tx_deadline(tx_at_us, slot_end_us,
				     zlrs_phy_airtime_us(request->phy, request->size),
				     &latest_us)) {
		return -EMSGSIZE;
	}
	/* SET_TX (0x020d), RTC timeout 0: identical to ral_lr20xx_set_tx(). The
	 * command is staged in SRAM; no flash, HAL, DMA or kernel calls from the ISR. */
	launch_mailbox.command[0] = 0x02U;
	launch_mailbox.command[1] = 0x0dU;
	launch_mailbox.command[2] = 0U;
	launch_mailbox.command[3] = 0U;
	launch_mailbox.command[4] = 0U;
	launch_mailbox.latest_us = latest_us;
	irq_pending = false;
	job_state = RADIO_ARMED;
	if (!zlrs_slot_timer_launch_at(tx_at_us, launch_command_isr)) {
		job_state = RADIO_IDLE;
		return late_tx_prepare(tx_at_us);
	}
	diag.transactions++;
	while (job_state == RADIO_ARMED) {
		const int32_t remaining = (int32_t)(slot_end_us - zlrs_slot_timer_now_us());

		if (remaining <= 0) {
			zlrs_slot_timer_cancel_launch();
			job_state = RADIO_IDLE;
			return -ETIMEDOUT;
		}
		(void)k_sem_take(owner_wake, K_USEC(remaining));
	}
	ZLRS_TRACE_EVENT("rf_launch", tx_at_us, launch_mailbox.irq_us);
	ZLRS_TRACE_EVENT("rf_launch_result", (uint32_t)launch_mailbox.result,
			 launch_mailbox.command_us);
	zlrs_timing_record(&diag.launch_irq_late_us, launch_mailbox.irq_us - tx_at_us);
	zlrs_timing_record(&diag.launch_wake_us, zlrs_slot_timer_now_us() - launch_mailbox.irq_us);
	zlrs_timing_record(&diag.launch_cmd_us, launch_mailbox.command_us);
	zlrs_timing_record(&diag.launch_jitter_us, begin_us - tx_at_us);
	zlrs_timing_record(&diag.tx_launch_cmd_us, launch_mailbox.command_us);
	if (launch_mailbox.result != 0) {
		job_state = RADIO_IDLE;
		if (launch_mailbox.result == -ETIME) {
			diag.trigger_late_starts++;
			diag.late_trigger_irq++;
		}
	}
	return launch_mailbox.result;
}

uint32_t zlrs_lr2021_started_us(void)
{
	return begin_us;
}

int zlrs_lr2021_init(struct k_sem *wake)
{
	__ASSERT_NO_MSG(wake != NULL);
	owner_wake = wake;
	/* RAL enables 1024-byte FIFOs. Every complete zlrs frame fits; intermediate
	 * RX threshold events must not obscure the RX_DONE edge or its timestamp. */
	BUILD_ASSERT(ZLRS_FRAME_MAX_LEN <= RAL_LR20XX_MAX_DATA_FIFO_BYTES);
	job_state = RADIO_IDLE;
	if (!device_is_ready(transceiver)) {
		return -ENODEV;
	}
	if (ral_reset(&radio.ral) != RAL_STATUS_OK || ral_init(&radio.ral) != RAL_STATUS_OK ||
	    ral_set_standby(&radio.ral, RAL_STANDBY_CFG_XOSC) != RAL_STATUS_OK ||
	    ral_set_rx_tx_fallback_mode(&radio.ral, RAL_FALLBACK_FS) != RAL_STATUS_OK ||
	    ral_cfg_fifo_irq(&radio.ral, RAL_RADIO_FIFO_FLAG_NONE, RAL_RADIO_FIFO_FLAG_UNDERFLOW,
			     0U, 0U, 0U, 0U) != RAL_STATUS_OK ||
	    ral_clear_irq_status(&radio.ral, RAL_IRQ_ALL) != RAL_STATUS_OK ||
	    ral_set_dio_irq_params(&radio.ral, RAL_IRQ_TX_DONE | RAL_IRQ_RX_DONE |
						       RAL_IRQ_RX_TIMEOUT | RAL_IRQ_RX_HDR_ERROR |
						       RAL_IRQ_RX_CRC_ERROR |
						       RAL_IRQ_TX_FIFO_LEVEL) != RAL_STATUS_OK) {
		return -EIO;
	}
	lora_transceiver_board_attach_interrupt(transceiver, radio_irq);
	lora_transceiver_board_enable_interrupt(transceiver);
	LOG_INF("LR2021 ready: exclusive RAL driver, hardware slot timer");
	return 0;
}

/* The HAL reports SPI transport success, so also check whether the chip accepted
 * the read. Direct status reads do not clear the chip's reset source or IRQs. */
static bool status_read_ok(lr20xx_status_t result)
{
	lr20xx_system_stat1_t stat;

	return result == LR20XX_STATUS_OK &&
	       lr20xx_system_get_status(radio.ral.context, &stat, NULL, NULL) == LR20XX_STATUS_OK &&
	       (stat.command_status == LR20XX_SYSTEM_CMD_STATUS_OK ||
		stat.command_status == LR20XX_SYSTEM_CMD_STATUS_DATA);
}

void zlrs_lr2021_read_status(zlrs_lr2021_status_t *out)
{
	lr20xx_system_stat1_t stat1;
	lr20xx_system_stat2_t stat2;
	lr20xx_system_version_t version;
	lr20xx_system_irq_mask_t irq;
	uint16_t value;

	__ASSERT_NO_MSG(job_state <= RADIO_IDLE_FS);
	*out = (zlrs_lr2021_status_t){0};
	/* Capture the maintenance-boundary state before issuing sensor commands. */
	if (lr20xx_system_get_status(radio.ral.context, &stat1, &stat2, &irq) == LR20XX_STATUS_OK) {
		out->chip_mode = stat2.chip_mode;
		out->command_status = stat1.command_status;
		out->reset_status = stat2.reset_status;
		out->irq = irq;
		out->valid |= ZLRS_LR2021_STATUS_STATE;
	}
	if (status_read_ok(lr20xx_system_get_version(radio.ral.context, &version))) {
		out->version_major = version.major;
		out->version_minor = version.minor;
		out->valid |= ZLRS_LR2021_STATUS_VERSION;
	}
	if (status_read_ok(lr20xx_system_get_temp(
		    radio.ral.context, LR20XX_SYSTEM_VALUE_FORMAT_UNIT,
		    LR20XX_SYSTEM_MEAS_RES_13_BITS, LR20XX_SYSTEM_TEMP_SRC_VBE, &value))) {
		out->temperature_centi_c = zlrs_lr2021_temperature_centi_c(value);
		out->valid |= ZLRS_LR2021_STATUS_TEMP;
	}
	if (status_read_ok(lr20xx_system_get_temp(
		    radio.ral.context, LR20XX_SYSTEM_VALUE_FORMAT_UNIT,
		    LR20XX_SYSTEM_MEAS_RES_13_BITS, LR20XX_SYSTEM_TEMP_SRC_XOSC, &value))) {
		out->xosc_temperature_centi_c = zlrs_lr2021_temperature_centi_c(value);
		out->valid |= ZLRS_LR2021_STATUS_XOSC_TEMP;
	}
	if (status_read_ok(lr20xx_system_get_vbat(radio.ral.context,
						  LR20XX_SYSTEM_VALUE_FORMAT_UNIT,
						  LR20XX_SYSTEM_MEAS_RES_13_BITS, &value))) {
		out->vbat_mv = value;
		out->valid |= ZLRS_LR2021_STATUS_VBAT;
	}
	if (status_read_ok(lr20xx_system_get_errors(radio.ral.context, &value))) {
		out->errors = value;
		out->valid |= ZLRS_LR2021_STATUS_ERRORS;
	}
	out->sampled_ms = k_uptime_get_32();
}

bool zlrs_lr2021_irq_pending(void)
{
	return job_state == RADIO_ACTIVE && irq_pending;
}

bool zlrs_lr2021_process_irq(zlrs_lr2021_result_t *out)
{
	ral_irq_t irq = 0;
	uint16_t len = 0U;
	int16_t rssi = 0;
	int8_t snr = 0;
	int status = -EIO;
	uint32_t airtime_us = 0U;

	if (job_state != RADIO_ACTIVE || !irq_pending) {
		return false;
	}
	const uint32_t event_us = irq_us;

	irq_pending = false;
	if (ral_get_and_clear_irq_status(&radio.ral, &irq) != RAL_STATUS_OK) {
		return false;
	}
	if (irq == 0U) {
		diag.empty_irq++;
		return false;
	}
	ZLRS_TRACE_EVENT("rf_irq_status", event_us, irq);
	diag.irq_last = irq;
	zlrs_timing_record(&diag.irq_service_us, zlrs_slot_timer_now_us() - event_us);
	if (!request->is_tx) {
		diag.rx_header_errors += (irq & RAL_IRQ_RX_HDR_ERROR) != 0U;
		diag.rx_crc_errors += (irq & RAL_IRQ_RX_CRC_ERROR) != 0U;
		diag.rx_length_errors += (irq & RAL_IRQ_RX_LEN_ERROR) != 0U;
	}
	const ral_irq_t frame_errors =
		RAL_IRQ_RX_HDR_ERROR | RAL_IRQ_RX_CRC_ERROR | RAL_IRQ_RX_LEN_ERROR;
	const ral_irq_t chip_errors = RAL_IRQ_CMD_ERROR | RAL_IRQ_ERROR;

	if (request->is_tx && (irq & RAL_IRQ_TX_FIFO_LEVEL) != 0U) {
		ral_radio_fifo_flag_t rx_flags;
		ral_radio_fifo_flag_t tx_flags;

		if (ral_get_fifo_irq(&radio.ral, &rx_flags, &tx_flags) != RAL_STATUS_OK) {
			diag.tx_fifo_errors++;
			irq |= RAL_IRQ_ERROR;
		} else if ((tx_flags & RAL_RADIO_FIFO_FLAG_UNDERFLOW) != 0U) {
			diag.tx_fifo_underflows++;
			irq |= RAL_IRQ_ERROR;
		}
		if ((irq & RAL_IRQ_ERROR) != 0U) {
			(void)ral_set_standby(&radio.ral, RAL_STANDBY_CFG_XOSC);
		}
		(void)ral_clear_fifo_irq(&radio.ral, RAL_RADIO_FIFO_FLAG_NONE,
					 RAL_RADIO_FIFO_FLAG_ALL);
	}

	/* RX single falls back after a damaged packet too. Keep the scheduled window
	 * open for a later valid packet; the RAL clears the RX FIFO before restarting.
	 * No timer boundary or protocol completion is moved by this retry. */
	if (!request->is_tx && (irq & frame_errors) != 0U && (irq & chip_errors) == 0U &&
	    (slot_end_us == 0U || (int32_t)(slot_end_us - zlrs_slot_timer_now_us()) > 0)) {
		if (ral_set_rx(&radio.ral, 0U) == RAL_STATUS_OK) {
			diag.rx_restarted++;
			return false;
		}
	}
	if ((irq & (RAL_IRQ_TX_DONE | RAL_IRQ_RX_DONE | RAL_IRQ_RX_TIMEOUT | frame_errors |
		    chip_errors)) == 0U) {
		return false; /* FIFO/timestamp activity is not a completed transaction. */
	}
	if (request->is_tx && (irq & RAL_IRQ_TX_DONE) != 0U && (irq & chip_errors) == 0U) {
		status = 0;
		diag.tx_done++;
		diag.tx_us_last = event_us - begin_us;
		const uint32_t toa_us = zlrs_phy_airtime_us(request->phy, request->size);

		zlrs_timing_record(&diag.tx_startup_us,
				   diag.tx_us_last > toa_us ? diag.tx_us_last - toa_us : 0U);
	} else if (!request->is_tx && (irq & frame_errors) == 0U && (irq & RAL_IRQ_RX_DONE) != 0U) {
		const uint32_t read_us = zlrs_slot_timer_now_us();

		/* Read once after modem CRC validation, then let the link verify its tag. */
		ZLRS_TRACE_EVENT("rf_fifo_begin", event_us, 0);
		if (ral_get_pkt_size(&radio.ral, &len) == RAL_STATUS_OK &&
		    len <= request->max_rx_size &&
		    (len == 0U ||
		     ral_get_data_rx_buffer(&radio.ral, request->buf, len) == RAL_STATUS_OK)) {
			status = 0;
			diag.rx_packets++;
			airtime_us = zlrs_phy_airtime_us(request->phy, len);
			diag.rx_fifo_bytes += len;
			zlrs_timing_record(&diag.rx_fifo_us, zlrs_slot_timer_now_us() - read_us);
		} else {
			diag.rx_payload_errors++;
		}
		ZLRS_TRACE_EVENT("rf_fifo_end", event_us, len);
		if (zlrs_phys[request->phy].lora_sf != 0U) {
			ral_lora_rx_pkt_status_t pkt;
			if (ral_get_lora_rx_pkt_status(&radio.ral, &pkt) == RAL_STATUS_OK) {
				rssi = pkt.rssi_pkt_in_dbm;
				snr = pkt.snr_pkt_in_db;
			}
		} else {
			ral_flrc_rx_pkt_status_t pkt;
			if (ral_get_flrc_rx_pkt_status(&radio.ral, &pkt) == RAL_STATUS_OK) {
				rssi = pkt.rssi_avg_in_dbm;
			}
		}

		diag.rx_us_last = event_us - begin_us;
	}
	/* The configured hardware fallback already returns a completed packet to FS. */
	job_state = status == 0 && (irq & chip_errors) == 0U ? RADIO_IDLE_FS : RADIO_IDLE;
	*out = (zlrs_lr2021_result_t){
		.status = status,
		.size = len,
		.rssi_dbm = rssi,
		.snr_db = snr,
		.done_us = event_us,
		.airtime_us = airtime_us,
		.completed_ms = zlrs_slot_timer_now_ms(),
	};
	return true;
}

void zlrs_lr2021_abort(void)
{
	zlrs_slot_timer_cancel_launch();
	const bool active =
		(job_state == RADIO_ACTIVE || job_state == RADIO_ARMED) && request != NULL;
	const bool tx = active && request->is_tx;

	/* Read IRQs before requesting standby. A read after SET_STANDBY waits for
	 * its BUSY transition and stalls the next slot's protocol work. Subsequent
	 * HAL commands already wait for BUSY, allowing that work to overlap the
	 * transition. prepare() clears any IRQ arriving after this snapshot. */
	ral_irq_t discarded = 0;
	(void)ral_get_and_clear_irq_status(&radio.ral, &discarded);
	(void)ral_set_standby(&radio.ral, RAL_STANDBY_CFG_XOSC);
	job_state = RADIO_IDLE;
	irq_pending = false;
	if (active) {
		if (tx) {
			diag.tx_expired++;
		} else {
			diag.rx_expired++;
			diag.rx_incomplete +=
				(discarded & (RAL_IRQ_RX_PREAMBLE_DETECTED | RAL_IRQ_RX_HDR_OK)) !=
				0U;
		}
	}
	slot_end_us = 0U;
	diag.overruns++;
}

const zlrs_lr2021_diag_t *zlrs_lr2021_diag(void)
{
	return &diag;
}
