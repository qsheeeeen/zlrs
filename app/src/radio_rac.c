/*
 * 射频移植层（RAC 后端）：CONFIG_ZLRS_APP_RADIO=y 时用这份。
 * 接口与契约见 radio_port.h，回调时序见 doc/01-system.md 第 7.2 节。
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
} rac_client_t;

static rac_client_t control_client;
static rac_client_t data_client;
static K_SEM_DEFINE(done_sem, 0, 1);

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

/* 速率档：v1.1 起原始速率与带宽合成一个枚举，见 doc/04-lr2021.md */
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

/* 带宽按 Hz 给，RAC/RAL 那边是枚举 */
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
 * LoRa 事务的参数。同步字是 1 字节（链路层从配对钥匙派生，最高位当 private
 * bit，见 lib/link/link.c 的 derive_pair）；RAC 那边虽然声明成枚举，实际是把
 * 这个字节原样交给驱动，驱动的约束只有"高 4 位不能是 0"。
 */
static void fill_lora_params(smtc_rac_context_t *rac, const zlrs_radio_req_t *req)
{
	static const uint8_t lora_default_sync = LORA_PRIVATE_NETWORK_SYNCWORD;

	rac->radio_params.lora.is_tx = req->is_tx;
	rac->radio_params.lora.is_ranging_exchange = false;
	rac->radio_params.lora.frequency_in_hz = req->freq_hz;
	rac->radio_params.lora.sf = (ral_lora_sf_t)req->mod.sf;
	rac->radio_params.lora.bw = lora_bw(req->mod.bw_hz);
	rac->radio_params.lora.cr = (ral_lora_cr_t)((req->mod.cr >= 1U && req->mod.cr <= 4U)
							    ? req->mod.cr
							    : 1U);
	rac->radio_params.lora.preamble_len_in_symb = req->mod.preamble;
	rac->radio_params.lora.header_type = RAL_LORA_PKT_EXPLICIT;
	rac->radio_params.lora.invert_iq_is_on = 0U;
	rac->radio_params.lora.crc_is_on = 1U;
	/*
	 * 配对时还没有配对钥匙，派生不出同步字，只能落在固定值上——配对帧就用它，
	 * 别把"上一对留下的派生值"带进来。
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

static void client_complete(rac_client_t *client, rp_status_t status)
{
	client->status = ZLRS_RADIO_OK;
	client->rx_len = 0U;
	client->rssi = 0;
	client->snr = 0;

	switch (status) {
	case RP_STATUS_TX_DONE:
	case RP_STATUS_RX_TIMEOUT:
		/* 空接收窗口不是错误 */
		break;
	case RP_STATUS_RX_PACKET:
		client->rx_len = client->ctx->smtc_rac_data_result.rx_size;
		client->rssi = client->ctx->smtc_rac_data_result.rssi_result;
		/* FLRC 下 USP 不填 SNR（只有 LoRa 有效），先原样带上去 */
		client->snr = client->ctx->smtc_rac_data_result.snr_result;
		break;
	default:
		client->status = ZLRS_RADIO_FAIL;
		break;
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

	/* 每个事务都一样的 FLRC 参数 */
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

	rac->smtc_rac_data_buffer_setup.tx_payload_buffer = req->buf;
	rac->smtc_rac_data_buffer_setup.size_of_tx_payload_buffer = req->size;
	rac->smtc_rac_data_buffer_setup.rx_payload_buffer = req->buf;
	rac->smtc_rac_data_buffer_setup.size_of_rx_payload_buffer = req->max_rx_size;

	/*
	 * 调制按链路各自选：控制链路可能是 LoRa 远距档，数据链路始终是 FLRC。
	 * 见 doc/02-protocol.md 4.1.1。
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
		/* 三个同步字槽各一个指针，只填这次事务用到的那个 */
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

	/* 结果可能被下一个事务覆盖，取出后立即交付 */
	zlrs_link_complete(client->link, client->status, client->rx_len, client->rssi,
			   client->snr, now_ms);
}

void zlrs_radio_port_drain(void)
{
	const uint32_t now_ms = zlrs_radio_port_now_ms();

	drain_client(&control_client, now_ms);
	drain_client(&data_client, now_ms);
}

void zlrs_radio_port_abort(void)
{
	(void)SMTC_SW_PLATFORM(smtc_rac_abort_radio_submit(control_client.access_id));
	(void)SMTC_SW_PLATFORM(smtc_rac_abort_radio_submit(data_client.access_id));
}

uint32_t zlrs_radio_port_now_ms(void)
{
	return smtc_modem_hal_get_time_in_ms();
}
