/* Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * One owner for both links. RAL and the LR20xx HAL are reused; RAC and the
 * radio planner do not participate in initialization, IRQs or transactions.
 */
#include "platform/radio.h"
#include "platform/slot_timer.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usp/lora_lbm_transceiver.h>

#include "ralf_lr20xx.h"
#include "ral_lr20xx_bsp.h"
#include "lr20xx_system.h"
#include "lr20xx_radio_fifo.h"
#include "lr20xx_radio_common.h"

LOG_MODULE_REGISTER(zlrs_radio, CONFIG_APP_LOG_LEVEL);

static const struct device *const transceiver =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_lorawan_transceiver));
static ralf_t radio = RALF_LR20XX_INSTANTIATE(DEVICE_DT_GET(DT_CHOSEN(zephyr_lorawan_transceiver)));
static K_SEM_DEFINE(done_sem, 0, 1);
static zlrs_link_t *owner;
static const zlrs_radio_req_t *request;
static volatile bool pending;
static bool prepared;
static volatile bool irq_pending;
static volatile uint32_t irq_us;
static uint32_t begin_us;
static uint32_t slot_end_us;
static uint32_t tx_at_us;
static bool timed_slot;
static ral_pkt_type_t configured_type;
static bool cfg_valid;
static ralf_params_flrc_t flrc_cfg;
static bool sync_valid[3];
static uint8_t sync_words[3][ZLRS_SYNC_WORD_LEN];
static bool tx_cfg_valid;
static uint32_t tx_cfg_freq;
static int8_t tx_cfg_power;
static bool rx_timing_valid;
static uint32_t rx_done_us;
static uint32_t rx_airtime_us;
static zlrs_radio_diag_t diag;

/* Rate tiers: since v1.1 the raw rate and bandwidth share one enum, see doc/06-radio.md */
static ral_flrc_raw_bit_rate_t raw_bit_rate(uint32_t rate_bps)
{
	switch (rate_bps) {
	case 2600000U:
		return RAL_FLRC_RAW_BIT_RATE_2_600_MBPS;
	case 650000U:
		return RAL_FLRC_RAW_BIT_RATE_0_650_MBPS;
	case 325000U:
		return RAL_FLRC_RAW_BIT_RATE_0_325_MBPS;
	case 1300000U:
	default:
		return RAL_FLRC_RAW_BIT_RATE_1_300_MBPS;
	}
}

static ral_flrc_preamble_length_t preamble_len(uint16_t bits)
{
	switch (bits) {
	case 4U:
		return RAL_FLRC_PREAMBLE_LENGTH_4_BITS;
	case 8U:
		return RAL_FLRC_PREAMBLE_LENGTH_8_BITS;
	case 12U:
		return RAL_FLRC_PREAMBLE_LENGTH_12_BITS;
	case 16U:
		return RAL_FLRC_PREAMBLE_LENGTH_16_BITS;
	case 20U:
		return RAL_FLRC_PREAMBLE_LENGTH_20_BITS;
	case 24U:
		return RAL_FLRC_PREAMBLE_LENGTH_24_BITS;
	case 28U:
		return RAL_FLRC_PREAMBLE_LENGTH_28_BITS;
	case 32U:
	default:
		return RAL_FLRC_PREAMBLE_LENGTH_32_BITS;
	}
}

static ral_flrc_tx_syncword_t tx_syncword(uint8_t slot)
{
	switch (slot) {
	case ZLRS_SYNC_SLOT_DATA:
		return RAL_FLRC_TX_SYNCWORD_2;
	case ZLRS_SYNC_SLOT_PAIRING:
		return RAL_FLRC_TX_SYNCWORD_3;
	default:
		return RAL_FLRC_TX_SYNCWORD_1;
	}
}

/* Bandwidth is given in Hz, RAC/RAL takes an enum */
static ral_lora_bw_t lora_bw(uint32_t bw_hz)
{
	switch (bw_hz) {
	case 250000U:
		return RAL_LORA_BW_250_KHZ;
	case 500000U:
		return RAL_LORA_BW_500_KHZ;
	case 125000U:
	default:
		return RAL_LORA_BW_125_KHZ;
	}
}

static ral_flrc_rx_match_sync_word_t rx_match_syncword(uint8_t slot)
{
	switch (slot) {
	case ZLRS_SYNC_SLOT_DATA:
		return RAL_FLRC_RX_MATCH_SYNCWORD_2;
	case ZLRS_SYNC_SLOT_PAIRING:
		return RAL_FLRC_RX_MATCH_SYNCWORD_3;
	default:
		return RAL_FLRC_RX_MATCH_SYNCWORD_1;
	}
}

static void radio_irq(const struct device *dev)
{
	ARG_UNUSED(dev);
	if (pending && !irq_pending) {
		irq_us = zlrs_slot_timer_now_us();
		irq_pending = true;
	}
	k_sem_give(&done_sem);
}

void zlrs_radio_wake(void)
{
	k_sem_give(&done_sem);
}

void zlrs_radio_slot(uint32_t tx_us, uint32_t end_us)
{
	tx_at_us = tx_us;
	slot_end_us = end_us;
	timed_slot = end_us != 0U;
	if (timed_slot) {
		zlrs_slot_timer_deadline(end_us);
	}
}

static void make_flrc(const zlrs_mod_cfg_t *mod, uint16_t len, ralf_params_flrc_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->mod_params.raw_bit_rate = raw_bit_rate(mod->rate_bps);
	cfg->mod_params.cr = RAL_FLRC_CR_3_4;
	cfg->mod_params.pulse_shape = RAL_FLRC_PULSE_SHAPE_BT_05;
	cfg->pkt_params.preamble_len = preamble_len(CONFIG_ZLRS_APP_FLRC_PREAMBLE_BITS);
	/* 16-bit sync word saves 16 uncoded bits; keep the per-slot syncword match enabled. */
	cfg->pkt_params.sync_word_len = RAL_FLRC_SYNCWORD_LENGTH_2_BYTES;
	cfg->pkt_params.pld_is_fix = false;
	cfg->pkt_params.pld_len_in_bytes = len;
	cfg->pkt_params.crc_type = RAL_FLRC_CRC_2_BYTES;
}

uint32_t zlrs_radio_airtime_us(const zlrs_mod_cfg_t *mod, uint16_t len)
{
	if (mod->modulation == ZLRS_MOD_FLRC) {
		ralf_params_flrc_t cfg;

		make_flrc(mod, len, &cfg);
		return ral_get_flrc_time_on_air_in_us(&radio.ral, &cfg.pkt_params, &cfg.mod_params);
	}
	return zlrs_mod_airtime_us(mod, len);
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

static int setup_flrc(const zlrs_radio_req_t *req)
{
	ralf_params_flrc_t cfg;
	const bool full = !cfg_valid || configured_type != RAL_PKT_TYPE_FLRC;

	make_flrc(&req->mod, req->is_tx ? req->size : req->max_rx_size, &cfg);
	cfg.rf_freq_in_hz = req->freq_hz;
	cfg.pkt_params.tx_syncword = tx_syncword(req->sync_slot);
	cfg.pkt_params.match_sync_word = rx_match_syncword(req->sync_slot);

	/* Packet type, modem, frequency, packet format and sync words have separate lifetimes. */
	cfg_valid = false;
	if (full && ral_set_pkt_type(&radio.ral, RAL_PKT_TYPE_FLRC) != RAL_STATUS_OK) {
		return -EIO;
	}
	if ((full || cfg.rf_freq_in_hz != flrc_cfg.rf_freq_in_hz) &&
	    setup_frequency(cfg.rf_freq_in_hz) != 0) {
		return -EIO;
	}
	if ((full || memcmp(&cfg.mod_params, &flrc_cfg.mod_params, sizeof(cfg.mod_params)) != 0) &&
	    ral_set_flrc_mod_params(&radio.ral, &cfg.mod_params) != RAL_STATUS_OK) {
		return -EIO;
	}
	if ((full || memcmp(&cfg.pkt_params, &flrc_cfg.pkt_params, sizeof(cfg.pkt_params)) != 0) &&
	    ral_set_flrc_pkt_params(&radio.ral, &cfg.pkt_params) != RAL_STATUS_OK) {
		return -EIO;
	}
	if (full && ral_set_flrc_crc_params(&radio.ral, 0U, 0U) != RAL_STATUS_OK) {
		return -EIO;
	}
	const uint8_t id = (uint8_t)req->sync_slot - 1U;

	if (id >= 3U) {
		return -EINVAL;
	}
	if ((full || !sync_valid[id] ||
	     memcmp(sync_words[id], req->sync_word, ZLRS_SYNC_WORD_LEN) != 0) &&
	    ral_set_flrc_sync_word(&radio.ral, id + 1U, req->sync_word,
				   RAL_FLRC_SYNCWORD_LENGTH_2_BYTES) != RAL_STATUS_OK) {
		return -EIO;
	}
	if (full) {
		/* Other slots must be rewritten when they next become active. */
		memset(sync_valid, 0, sizeof(sync_valid));
	}
	memcpy(sync_words[id], req->sync_word, ZLRS_SYNC_WORD_LEN);
	sync_valid[id] = true;
	flrc_cfg = cfg;
	configured_type = RAL_PKT_TYPE_FLRC;
	cfg_valid = true;
	return setup_tx_power(req);
}

static int setup_lora(const zlrs_radio_req_t *req)
{
	ralf_params_lora_t cfg = {0};

	cfg.mod_params.sf = (ral_lora_sf_t)req->mod.sf;
	cfg.mod_params.bw = lora_bw(req->mod.bw_hz);
	cfg.mod_params.cr = (ral_lora_cr_t)req->mod.cr;
	cfg.pkt_params.preamble_len_in_symb = req->mod.preamble;
	cfg.pkt_params.header_type = RAL_LORA_PKT_EXPLICIT;
	cfg.pkt_params.pld_len_in_bytes = req->is_tx ? req->size : req->max_rx_size;
	cfg.pkt_params.crc_is_on = true;
	cfg.rf_freq_in_hz = req->freq_hz;
	cfg.output_pwr_in_dbm = req->tx_power_dbm;
	cfg.sync_word = req->mod.sync_byte != 0U && req->sync_slot != ZLRS_SYNC_SLOT_PAIRING
				? req->mod.sync_byte
				: 0x12U;
	cfg_valid = false;
	tx_cfg_valid = false;
	return ralf_setup_lora(&radio, &cfg) == RAL_STATUS_OK ? 0 : -EIO;
}

static int prepare_request(zlrs_link_t *link, const zlrs_radio_req_t *req)
{
	const uint32_t start = zlrs_slot_timer_now_us();
	ral_irq_t stale = 0;
	int rc;

	if (pending || prepared || req == NULL || link == NULL) {
		return -EBUSY;
	}
	rx_timing_valid = false;
	owner = link;
	request = req;
	k_sem_reset(&done_sem);
	if (ral_set_standby(&radio.ral, RAL_STANDBY_CFG_XOSC) != RAL_STATUS_OK ||
	    ral_get_and_clear_irq_status(&radio.ral, &stale) != RAL_STATUS_OK) {
		return -EIO;
	}
	if ((stale & (RAL_IRQ_CMD_ERROR | RAL_IRQ_ERROR)) != 0U) {
		(void)lr20xx_system_get_errors(radio.ral.context, &diag.chip_errors);
		diag.chip_error_freq_hz = flrc_cfg.rf_freq_in_hz;
		(void)lr20xx_system_clear_errors(radio.ral.context);
	}
	if (stale != 0U) {
		diag.stale_irq++;
		diag.stale_irq_bits = stale;
	}
	rc = req->mod.modulation == ZLRS_MOD_FLRC ? setup_flrc(req) : setup_lora(req);
	if (rc != 0) {
		return rc;
	}
	if (ral_set_dio_irq_params(&radio.ral, RAL_IRQ_TX_DONE | RAL_IRQ_RX_DONE |
						       RAL_IRQ_RX_TIMEOUT | RAL_IRQ_RX_HDR_ERROR |
						       RAL_IRQ_RX_CRC_ERROR) != RAL_STATUS_OK) {
		return -EIO;
	}
	/* Aborted TX leaves bytes in the FIFO. Each request starts with its own packet. */
	if (req->is_tx && (lr20xx_radio_fifo_clear_tx(radio.ral.context) != LR20XX_STATUS_OK ||
			   ral_set_pkt_payload(&radio.ral, req->buf, req->size) != RAL_STATUS_OK)) {
		return -EIO;
	}
	/* Finish synthesizer startup here. The following read waits for BUSY before launch. */
	if (ral_set_fs(&radio.ral) != RAL_STATUS_OK ||
	    ral_get_and_clear_irq_status(&radio.ral, &stale) != RAL_STATUS_OK) {
		return -EIO;
	}
	diag.submit_us_last = zlrs_slot_timer_now_us() - start;
	diag.submit_us_max = MAX(diag.submit_us_max, diag.submit_us_last);
	if (timed_slot) {
		if (req->is_tx) {
			/* TX has a fixed launch point; receivers configure before it. */
			if ((int32_t)(zlrs_slot_timer_now_us() - tx_at_us) > 100) {
				diag.late_starts++;
				diag.launch_late_us_last = zlrs_slot_timer_now_us() - tx_at_us;
				diag.launch_late_us_max =
					MAX(diag.launch_late_us_max, diag.launch_late_us_last);
				return -ETIME;
			}
			if ((int32_t)(tx_at_us + ZLRS_RADIO_STARTUP_US +
				      zlrs_radio_airtime_us(&req->mod, req->size) - slot_end_us) >=
			    0) {
				return -EMSGSIZE;
			}
		}
		if ((int32_t)(zlrs_slot_timer_now_us() - slot_end_us) >= 0) {
			return -ETIME;
		}
	}
	prepared = true;
	if (req->is_tx) {
		const uint32_t ready_us = zlrs_slot_timer_now_us();

		diag.preload_lead_us_last =
			timed_slot && (int32_t)(tx_at_us - ready_us) > 0 ? tx_at_us - ready_us : 0U;
	}
	return 0;
}

int zlrs_radio_prepare(zlrs_link_t *link, const zlrs_radio_req_t *req)
{
	return prepare_request(link, req);
}

int zlrs_radio_launch(void)
{
	int rc;

	if (!prepared || pending) {
		return -EINVAL;
	}
	prepared = false;
	if (timed_slot) {
		if (request->is_tx && (int32_t)(zlrs_slot_timer_now_us() - tx_at_us) < 0) {
			return -EAGAIN;
		}
		if ((int32_t)(zlrs_slot_timer_now_us() - slot_end_us) >= 0) {
			return -ETIME;
		}
		if (request->is_tx && (int32_t)(zlrs_slot_timer_now_us() - tx_at_us) > 100) {
			diag.late_starts++;
			diag.launch_late_us_last = zlrs_slot_timer_now_us() - tx_at_us;
			diag.launch_late_us_max =
				MAX(diag.launch_late_us_max, diag.launch_late_us_last);
			return -ETIME;
		}
	}
	begin_us = zlrs_slot_timer_now_us();
	if (timed_slot && request->is_tx) {
		diag.launch_jitter_us_last = begin_us - tx_at_us;
		diag.launch_jitter_us_max =
			MAX(diag.launch_jitter_us_max, diag.launch_jitter_us_last);
	}
	irq_pending = false;
	pending = true;
	diag.transactions++;
	rc = request->is_tx ? ral_set_tx(&radio.ral) : ral_set_rx(&radio.ral, 0U);
	diag.launch_cmd_us_last = zlrs_slot_timer_now_us() - begin_us;
	diag.launch_cmd_us_max = MAX(diag.launch_cmd_us_max, diag.launch_cmd_us_last);
	if (rc != RAL_STATUS_OK) {
		pending = false;
		return -EIO;
	}
	return 0;
}

static K_SEM_DEFINE(launch_request_sem, 0, 1);
static K_SEM_DEFINE(launch_result_sem, 0, 1);
static uint32_t launch_target_us;
static int launch_result;

static void launch_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		(void)k_sem_take(&launch_request_sem, K_FOREVER);
		zlrs_slot_timer_wait_until(launch_target_us);
		launch_result = zlrs_radio_launch();
		k_sem_give(&launch_result_sem);
	}
}

/* Timer bottom half: only the prepared launch command may preempt a scheduler lock.
 * The main thread waits for its result and cannot access the RAL until it completes.
 * This thread never touches USB, link state, packet buffers or configuration. */
K_THREAD_DEFINE(zlrs_radio_launch_tid, 1536, launch_thread, NULL, NULL, NULL, K_PRIO_COOP(0), 0, 0);

int zlrs_radio_launch_at(uint32_t target_us)
{
	if (!prepared || request == NULL || !request->is_tx) {
		return -EINVAL;
	}
	launch_target_us = target_us;
	k_sem_give(&launch_request_sem);
	(void)k_sem_take(&launch_result_sem, K_FOREVER);
	return launch_result;
}

int zlrs_radio_init(void)
{
	if (!device_is_ready(transceiver)) {
		return -ENODEV;
	}
	if (ral_reset(&radio.ral) != RAL_STATUS_OK || ral_init(&radio.ral) != RAL_STATUS_OK ||
	    ral_set_standby(&radio.ral, RAL_STANDBY_CFG_XOSC) != RAL_STATUS_OK ||
	    ral_set_rx_tx_fallback_mode(&radio.ral, RAL_FALLBACK_FS) != RAL_STATUS_OK ||
	    ral_clear_irq_status(&radio.ral, RAL_IRQ_ALL) != RAL_STATUS_OK ||
	    ral_set_dio_irq_params(&radio.ral, RAL_IRQ_TX_DONE | RAL_IRQ_RX_DONE |
						       RAL_IRQ_RX_TIMEOUT | RAL_IRQ_RX_HDR_ERROR |
						       RAL_IRQ_RX_CRC_ERROR) != RAL_STATUS_OK) {
		return -EIO;
	}
	lora_transceiver_board_attach_interrupt(transceiver, radio_irq);
	lora_transceiver_board_enable_interrupt(transceiver);
	LOG_INF("LR2021 ready: exclusive RAL driver, hardware slot timer");
	return 0;
}

int zlrs_radio_wait_us(uint32_t timeout_us)
{
	return k_sem_take(&done_sem, K_USEC(timeout_us));
}

bool zlrs_radio_irq_pending(void)
{
	return pending && irq_pending;
}

bool zlrs_radio_rx_timing(uint32_t *done_us, uint32_t *airtime_us)
{
	*done_us = rx_done_us;
	*airtime_us = rx_airtime_us;
	return rx_timing_valid;
}

void zlrs_radio_drain(void)
{
	ral_irq_t irq = 0;
	uint16_t len = 0U;
	int16_t rssi = 0;
	int8_t snr = 0;
	int status = ZLRS_RADIO_FAIL;

	if (!pending || !irq_pending) {
		return;
	}
	const uint32_t event_us = irq_us;

	irq_pending = false;
	if (ral_get_and_clear_irq_status(&radio.ral, &irq) != RAL_STATUS_OK) {
		return;
	}
	if (irq == 0U) {
		diag.empty_irq++;
		return;
	}
	diag.irq_last = irq;
	diag.irq_service_us_last = zlrs_slot_timer_now_us() - event_us;
	diag.irq_service_us_max = MAX(diag.irq_service_us_max, diag.irq_service_us_last);
	if (!request->is_tx) {
		diag.rx_header_errors += (irq & RAL_IRQ_RX_HDR_ERROR) != 0U;
		diag.rx_crc_errors += (irq & RAL_IRQ_RX_CRC_ERROR) != 0U;
		diag.rx_length_errors += (irq & RAL_IRQ_RX_LEN_ERROR) != 0U;
	}
	if (request->is_tx && (irq & RAL_IRQ_TX_DONE) != 0U) {
		status = ZLRS_RADIO_OK;
		diag.tx_done++;
		diag.tx_us_last = event_us - begin_us;
		const uint32_t toa_us = zlrs_radio_airtime_us(&request->mod, request->size);

		diag.tx_startup_us_last = diag.tx_us_last > toa_us ? diag.tx_us_last - toa_us : 0U;
		diag.tx_startup_us_max = MAX(diag.tx_startup_us_max, diag.tx_startup_us_last);
	} else if (!request->is_tx && (irq & (RAL_IRQ_RX_HDR_ERROR | RAL_IRQ_RX_CRC_ERROR)) == 0U &&
		   (irq & RAL_IRQ_RX_DONE) != 0U) {
		if (ral_get_pkt_payload(&radio.ral, request->max_rx_size, request->buf, &len) ==
		    RAL_STATUS_OK) {
			status = ZLRS_RADIO_OK;
			diag.rx_packets++;
			rx_done_us = event_us;
			rx_airtime_us = zlrs_radio_airtime_us(&request->mod, len);
			rx_timing_valid = true;
		} else {
			diag.rx_payload_errors++;
		}
		if (request->mod.modulation == ZLRS_MOD_FLRC) {
			ral_flrc_rx_pkt_status_t pkt;

			if (ral_get_flrc_rx_pkt_status(&radio.ral, &pkt) == RAL_STATUS_OK) {
				rssi = pkt.rssi_sync_in_dbm;
			}
		} else {
			ral_lora_rx_pkt_status_t pkt;

			if (ral_get_lora_rx_pkt_status(&radio.ral, &pkt) == RAL_STATUS_OK) {
				rssi = pkt.rssi_pkt_in_dbm;
				snr = pkt.snr_pkt_in_db;
			}
		}
		diag.rx_us_last = event_us - begin_us;
		diag.rx_windows++;
	}
	(void)ral_set_standby(&radio.ral, RAL_STANDBY_CFG_XOSC);
	pending = false;
	zlrs_link_complete(owner, status, len, rssi, snr, k_uptime_get_32());
}

void zlrs_radio_abort(void)
{
	const bool active = pending && request != NULL;
	const bool tx = active && request->is_tx;

	(void)ral_set_standby(&radio.ral, RAL_STANDBY_CFG_XOSC);
	pending = false;
	irq_pending = false;
	prepared = false;
	ral_irq_t discarded = 0;
	(void)ral_get_and_clear_irq_status(&radio.ral, &discarded);
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
	k_sem_reset(&done_sem);
	timed_slot = false;
	diag.overruns++;
}

void zlrs_radio_diag(zlrs_radio_diag_t *out)
{
	*out = diag;
}
