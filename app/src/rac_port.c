/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rac_port.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include <smtc_modem_hal.h>
#include <smtc_rac_api.h>
#include <smtc_sw_platform_helper.h>

LOG_MODULE_REGISTER(zlrs_rac, CONFIG_APP_LOG_LEVEL);

/* FLRC 速率档：原始速率与 DSB 带宽成对出现，见 docs/05-lr2021.md */
static const struct {
	uint32_t br_bps;
	uint32_t bw_hz;
} rate_table[] = {
	{ 2600000U, 2666000U },
	{ 1300000U, 1200000U },
	{ 650000U, 600000U },
	{ 325000U, 300000U },
};

typedef struct {
	uint8_t access_id;
	smtc_rac_context_t *ctx;
	zlrs_link_t *link;
	bool is_tx;
	volatile bool done;
	int status;
	uint16_t rx_len;
	int16_t rssi;
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

const zlrs_radio_ops_t *zlrs_rac_control_ops(void)
{
	return &control_ops;
}

const zlrs_radio_ops_t *zlrs_rac_data_ops(void)
{
	return &data_ops;
}

static const uint32_t *rate_params(uint32_t rate_bps, uint32_t *bw_hz)
{
	for (size_t i = 0; i < ARRAY_SIZE(rate_table); i++) {
		if (rate_table[i].br_bps == rate_bps) {
			*bw_hz = rate_table[i].bw_hz;
			return &rate_table[i].br_bps;
		}
	}

	/* 不认识的档位回落到均衡档 */
	*bw_hz = rate_table[1].bw_hz;

	return &rate_table[1].br_bps;
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

	switch (status) {
	case RP_STATUS_TX_DONE:
	case RP_STATUS_RX_TIMEOUT:
		/* 空接收窗口不是错误 */
		break;
	case RP_STATUS_RX_PACKET:
		client->rx_len = client->ctx->smtc_rac_data_result.rx_size;
		client->rssi = client->ctx->smtc_rac_data_result.rssi_result;
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
		LOG_ERR("RAC 客户端打开失败（priority %u）", priority);
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
	client->ctx->radio_params.flrc.preamble_len_in_bits =
		CONFIG_ZLRS_APP_FLRC_PREAMBLE_BITS;
	client->ctx->radio_params.flrc.sync_word_len = RAL_FLRC_SYNCWORD_LENGTH_4_BYTES;

	return 0;
}

int zlrs_rac_port_init(zlrs_link_t *control, zlrs_link_t *data)
{
	if (client_open(&control_client, RAC_HIGH_PRIORITY, control_post, control) != 0) {
		return -1;
	}
	if (client_open(&data_client, RAC_MEDIUM_PRIORITY, data_post, data) != 0) {
		return -1;
	}

	LOG_INF("RAC 客户端就绪：控制 id=%u，数据 id=%u", control_client.access_id,
		data_client.access_id);

	return 0;
}

static int port_submit(void *ctx, const zlrs_radio_req_t *req)
{
	rac_client_t *client = ctx;
	smtc_rac_context_t *rac = client->ctx;
	uint32_t bw_hz = 0U;
	const uint32_t *br = rate_params(req->rate_bps, &bw_hz);

	rac->smtc_rac_data_buffer_setup.tx_payload_buffer = req->buf;
	rac->smtc_rac_data_buffer_setup.size_of_tx_payload_buffer = req->size;
	rac->smtc_rac_data_buffer_setup.rx_payload_buffer = req->buf;
	rac->smtc_rac_data_buffer_setup.size_of_rx_payload_buffer = req->max_rx_size;

	rac->radio_params.flrc.is_tx = req->is_tx;
	rac->radio_params.flrc.tx_size = req->size;
	rac->radio_params.flrc.max_rx_size = req->max_rx_size;
	rac->radio_params.flrc.frequency_in_hz = req->freq_hz;
	rac->radio_params.flrc.tx_power_in_dbm = req->tx_power_dbm;
	rac->radio_params.flrc.br_in_bps = *br;
	rac->radio_params.flrc.bw_dsb_in_hz = bw_hz;
	rac->radio_params.flrc.cr = RAL_FLRC_CR_3_4;
	rac->radio_params.flrc.tx_syncword = tx_syncword(req->sync_slot);
	rac->radio_params.flrc.match_sync_word = rx_match_syncword(req->sync_slot);
	rac->radio_params.flrc.sync_word = req->sync_word;
	rac->radio_params.flrc.rx_timeout_ms = req->timeout_ms;

	rac->scheduler_config.start_time_ms = smtc_modem_hal_get_time_in_ms();

	client->is_tx = req->is_tx;
	client->done = false;

	if (SMTC_SW_PLATFORM(smtc_rac_submit_radio_transaction(client->access_id)) !=
	    SMTC_RAC_SUCCESS) {
		return -1;
	}

	return 0;
}

int zlrs_rac_port_wait(uint32_t timeout_ms)
{
	return k_sem_take(&done_sem, K_MSEC(timeout_ms));
}

static void drain_client(rac_client_t *client, uint32_t now_ms)
{
	if (!client->done || client->link == NULL) {
		return;
	}

	client->done = false;

	/* 结果可能被下一个事务覆盖，取出后立即交付 */
	zlrs_link_complete(client->link, client->status, client->rx_len, client->rssi,
			   now_ms);
}

void zlrs_rac_port_drain(void)
{
	const uint32_t now_ms = zlrs_rac_port_now_ms();

	drain_client(&control_client, now_ms);
	drain_client(&data_client, now_ms);
}

void zlrs_rac_port_abort(void)
{
	(void)SMTC_SW_PLATFORM(smtc_rac_abort_radio_submit(control_client.access_id));
	(void)SMTC_SW_PLATFORM(smtc_rac_abort_radio_submit(data_client.access_id));
}

uint32_t zlrs_rac_port_now_ms(void)
{
	return smtc_modem_hal_get_time_in_ms();
}
