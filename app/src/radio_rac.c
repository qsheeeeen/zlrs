/*
 * Radio port layer (RAC backend), used when CONFIG_ZLRS_APP_RADIO=y.
 * Interface and contract: radio_port.h; callback timing: doc/07-architecture.md 6.2.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "radio_port.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include <smtc_modem_hal.h>
#include <smtc_rac_api.h>
#include <smtc_sw_platform_helper.h>

#include "ral.h"

LOG_MODULE_REGISTER(zlrs_radio_rac, CONFIG_APP_LOG_LEVEL);

typedef struct {
	uint8_t access_id;
	smtc_rac_context_t *ctx;
	zlrs_link_t *link;
	volatile bool done;
	int status;
	uint16_t rx_len;
	int16_t rssi;
	int8_t snr;
	uint32_t submit_us; /* when the transaction went in, for the window duration counters */
	uint16_t timeout_ms;
	bool is_tx;
} rac_client_t;

static rac_client_t control_client;
static rac_client_t data_client;
static K_SEM_DEFINE(done_sem, 0, 1);

/* Port-level bench counters, see radio_port.h */
static zlrs_radio_diag_t diag;

void zlrs_radio_port_diag(zlrs_radio_diag_t *out)
{
	if (out != NULL) {
		*out = diag;
	}
}

/*
 * Leave no IRQ behind for the next transaction. The radio holds its IRQ latch until the status is
 * read: a bit left over from an earlier slot would complete the next transaction early (or wake
 * the planner with no task, which is what its "no more task activated" trace is), so read and
 * count them here. The vendor sample never hits this because its engine runs in its own thread and
 * drains every event as it comes.
 */
static void drain_radio_irq(void)
{
	ralf_t *radio = smtc_rac_get_radio();
	ral_irq_t irq = 0;

	if (radio == NULL) {
		return;
	}
	if (ral_get_and_clear_irq_status(&radio->ral, &irq) == RAL_STATUS_OK &&
	    (uint32_t)irq != 0U) {
		diag.stale_irq++;
		diag.stale_irq_bits = (uint32_t)irq;
	}
}

static uint32_t port_random(void *ctx)
{
	(void)ctx;

	return sys_rand32_get();
}

static int port_submit(void *ctx, const zlrs_radio_req_t *req);

static const zlrs_radio_ops_t control_ops = {
	.submit = port_submit,
	.random = port_random,
	.ctx = &control_client,
};

static const zlrs_radio_ops_t data_ops = {
	.submit = port_submit,
	.random = port_random,
	.ctx = &data_client,
};

const zlrs_radio_ops_t *zlrs_radio_control_ops(void)
{
	return &control_ops;
}

const zlrs_radio_ops_t *zlrs_radio_data_ops(void)
{
	return &data_ops;
}

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

/*
 * Parameters of a LoRa transaction. The sync word is one byte (derived by the link layer from
 * the pairing key, top bit as the private bit, see derive_pair in core/link/link.c). RAC declares
 * it as an enum but passes the byte through; the driver only requires the top 4 bits to be non-zero.
 */
static void fill_lora_params(smtc_rac_context_t *rac, const zlrs_radio_req_t *req)
{
	static const uint8_t lora_default_sync = LORA_PRIVATE_NETWORK_SYNCWORD;

	rac->radio_params.lora.is_tx = req->is_tx;
	rac->radio_params.lora.is_ranging_exchange = false;
	rac->radio_params.lora.frequency_in_hz = req->freq_hz;
	rac->radio_params.lora.sf = (ral_lora_sf_t)req->mod.sf;
	rac->radio_params.lora.bw = lora_bw(req->mod.bw_hz);
	rac->radio_params.lora.cr =
		(ral_lora_cr_t)((req->mod.cr >= 1U && req->mod.cr <= 4U) ? req->mod.cr : 1U);
	rac->radio_params.lora.preamble_len_in_symb = req->mod.preamble;
	rac->radio_params.lora.header_type = RAL_LORA_PKT_EXPLICIT;
	rac->radio_params.lora.invert_iq_is_on = 0U;
	rac->radio_params.lora.crc_is_on = 1U;
	/*
	 * While pairing there is no pairing key yet, so no sync word can be derived and a fixed value
	 * has to do: pairing frames use it, and no leftover derivation from the previous pair leaks in.
	 */
	rac->radio_params.lora.sync_word =
		(req->mod.sync_byte != 0U && req->sync_slot != ZLRS_SYNC_SLOT_PAIRING)
			? (smtc_rac_lora_syncword_t)req->mod.sync_byte
			: (smtc_rac_lora_syncword_t)lora_default_sync;
	rac->radio_params.lora.tx_power_in_dbm = (uint8_t)req->tx_power_dbm;
	rac->radio_params.lora.tx_size = req->size;
	rac->radio_params.lora.rx_timeout_ms = req->timeout_ms;
	rac->radio_params.lora.symb_nb_timeout = 0U;
	rac->radio_params.lora.max_rx_size = req->max_rx_size;
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

/* Report a failing transaction once: it fails every frame, more log lines help nobody */
static bool radio_fail_logged;

static void client_complete(rac_client_t *client, rp_status_t status)
{
	const uint32_t now_us = (uint32_t)k_cyc_to_us_floor64(k_cycle_get_64());
	const uint32_t took_us = now_us - client->submit_us;

	client->status = ZLRS_RADIO_OK;
	client->rx_len = 0U;
	client->rssi = 0;
	client->snr = 0;

	switch (status) {
	case RP_STATUS_TX_DONE:
	case RP_STATUS_RX_TIMEOUT:
		/* An empty receive window is not an error */
		break;
	case RP_STATUS_RX_PACKET:
		client->rx_len = client->ctx->smtc_rac_data_result.rx_size;
		client->rssi = client->ctx->smtc_rac_data_result.rssi_result;
		/* USP leaves SNR unset under FLRC (only LoRa provides it), pass it on as-is */
		client->snr = client->ctx->smtc_rac_data_result.snr_result;
		break;
	default:
		client->status = ZLRS_RADIO_FAIL;
		/* First time only: a failing transaction repeats every frame and would flood the log */
		if (!radio_fail_logged) {
			radio_fail_logged = true;
			LOG_ERR("radio transaction failed (rp_status %d)", (int)status);
		}
		break;
	}

	if (client->is_tx) {
		diag.tx_us_last = took_us;
	} else {
		diag.rx_us_last = took_us;
		diag.rx_windows++;
		/* A packet ends the window early; anything else this far in is an early close */
		if (client->rx_len == 0U &&
		    took_us + 500U < (uint32_t)client->timeout_ms * 1000U) {
			diag.rx_early++;
		}
	}

	client->done = true;
	k_sem_give(&done_sem);
}

static void control_post(rp_status_t status)
{
	client_complete(&control_client, status);
}

static void data_post(rp_status_t status)
{
	client_complete(&data_client, status);
}

static int client_open(rac_client_t *client, uint8_t priority, void (*post)(rp_status_t),
		       zlrs_link_t *link)
{
	client->access_id = SMTC_SW_PLATFORM(smtc_rac_open_radio(priority));
	client->ctx = smtc_rac_get_context(client->access_id);
	client->link = link;

	if (client->ctx == NULL) {
		LOG_ERR("RAC client open failed (priority %u)", priority);
		return -1;
	}

	client->ctx->modulation_type = SMTC_RAC_MODULATION_FLRC;
	client->ctx->scheduler_config.scheduling = SMTC_RAC_ASAP_TRANSACTION;
	client->ctx->scheduler_config.callback_pre_radio_transaction = NULL;
	client->ctx->scheduler_config.callback_post_radio_transaction = post;

	/* FLRC parameters shared by every transaction */
	client->ctx->radio_params.flrc.pld_is_fix = false;
	client->ctx->radio_params.flrc.crc_type = RAL_FLRC_CRC_2_BYTES;
	client->ctx->radio_params.flrc.crc_seed = 0U;
	client->ctx->radio_params.flrc.crc_polynomial = 0U;
	client->ctx->radio_params.flrc.pulse_shape = RAL_FLRC_PULSE_SHAPE_BT_05;
	client->ctx->radio_params.flrc.preamble_len =
		preamble_len(CONFIG_ZLRS_APP_FLRC_PREAMBLE_BITS);
	client->ctx->radio_params.flrc.sync_word_len = RAL_FLRC_SYNCWORD_LENGTH_4_BYTES;

	return 0;
}

int zlrs_radio_port_init(zlrs_link_t *control, zlrs_link_t *data)
{
	if (client_open(&control_client, RAC_HIGH_PRIORITY, control_post, control) != 0) {
		return -1;
	}
	if (client_open(&data_client, RAC_MEDIUM_PRIORITY, data_post, data) != 0) {
		return -1;
	}

	LOG_INF("RAC clients ready: control id=%u, data id=%u", control_client.access_id,
		data_client.access_id);

	return 0;
}

static int port_submit(void *ctx, const zlrs_radio_req_t *req)
{
	rac_client_t *client = ctx;
	smtc_rac_context_t *rac = client->ctx;

	drain_radio_irq();
	diag.transactions++;
	client->submit_us = (uint32_t)k_cyc_to_us_floor64(k_cycle_get_64());
	client->timeout_ms = req->timeout_ms;
	client->is_tx = req->is_tx;

	rac->smtc_rac_data_buffer_setup.tx_payload_buffer = req->buf;
	rac->smtc_rac_data_buffer_setup.size_of_tx_payload_buffer = req->size;
	rac->smtc_rac_data_buffer_setup.rx_payload_buffer = req->buf;
	rac->smtc_rac_data_buffer_setup.size_of_rx_payload_buffer = req->max_rx_size;

	/*
	 * Modulation is per link: control may run the LoRa long-range tier, data is always FLRC.
	 * See doc/07-architecture.md 3.
	 */
	if (req->mod.modulation == (uint8_t)ZLRS_MOD_LORA) {
		rac->modulation_type = SMTC_RAC_MODULATION_LORA;
		fill_lora_params(rac, req);
	} else {
		rac->modulation_type = SMTC_RAC_MODULATION_FLRC;
		rac->radio_params.flrc.is_tx = req->is_tx;
		rac->radio_params.flrc.tx_size = req->size;
		rac->radio_params.flrc.max_rx_size = req->max_rx_size;
		rac->radio_params.flrc.frequency_in_hz = req->freq_hz;
		rac->radio_params.flrc.tx_power_in_dbm = req->tx_power_dbm;
		rac->radio_params.flrc.raw_bit_rate = raw_bit_rate(req->mod.rate_bps);
		rac->radio_params.flrc.cr = RAL_FLRC_CR_3_4;
		rac->radio_params.flrc.tx_syncword_index = tx_syncword(req->sync_slot);
		rac->radio_params.flrc.match_sync_word = rx_match_syncword(req->sync_slot);
		/* One pointer per sync word slot; only the one this transaction uses is filled in */
		for (uint8_t i = 0U; i < 3U; i++) {
			rac->radio_params.flrc.sync_word[i] = NULL;
		}
		if (req->sync_slot >= ZLRS_SYNC_SLOT_CONTROL &&
		    req->sync_slot <= ZLRS_SYNC_SLOT_PAIRING) {
			rac->radio_params.flrc.sync_word[req->sync_slot - 1U] = req->sync_word;
		}
		rac->radio_params.flrc.rx_timeout_ms = req->timeout_ms;
	}

	rac->scheduler_config.start_time_ms = smtc_modem_hal_get_time_in_ms();

	client->done = false;

	if (SMTC_SW_PLATFORM(smtc_rac_submit_radio_transaction(client->access_id)) !=
	    SMTC_RAC_SUCCESS) {
		return -1;
	}

	return 0;
}

int zlrs_radio_port_wait_us(uint32_t timeout_us)
{
	return k_sem_take(&done_sem, K_USEC(timeout_us));
}

static void drain_client(rac_client_t *client, uint32_t now_ms)
{
	if (!client->done || client->link == NULL) {
		return;
	}

	client->done = false;

	/* The next transaction can overwrite the result, so hand it over as soon as it is read */
	zlrs_link_complete(client->link, client->status, client->rx_len, client->rssi, client->snr,
			   now_ms);
}

void zlrs_radio_port_drain(void)
{
	const uint32_t now_ms = zlrs_radio_port_now_ms();

	/*
	 * A transaction that just closed has had its IRQ consumed by the planner; anything latched
	 * here arrived outside a transaction, which is exactly the condition the engine reports with
	 * "no more task activated". Counting it shows whether that trace has a real source.
	 */
	if (smtc_rac_is_irq_flag_pending()) {
		diag.late_irq++;
	}

	drain_client(&control_client, now_ms);
	drain_client(&data_client, now_ms);
}

void zlrs_radio_port_abort(void)
{
	diag.overruns++;
	(void)SMTC_SW_PLATFORM(smtc_rac_abort_radio_submit(control_client.access_id));
	(void)SMTC_SW_PLATFORM(smtc_rac_abort_radio_submit(data_client.access_id));

	/*
	 * Drop any completion the aborted transaction still owes: it would otherwise wake the next
	 * wait, which would take "the semaphore was given" for "my transaction finished" and skip the
	 * finish path on a request that never completed.
	 */
	control_client.done = false;
	data_client.done = false;
	while (k_sem_take(&done_sem, K_NO_WAIT) == 0) {
	}
}

uint32_t zlrs_radio_port_now_ms(void)
{
	return smtc_modem_hal_get_time_in_ms();
}
