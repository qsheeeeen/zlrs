/*
 * Management protocol, see include/zlrs/mgmt/mgmt.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/mgmt/mgmt.h"

#include <string.h>

#include "zlrs/proto/crc.h"

void zlrs_mgmt_parser_init(zlrs_mgmt_parser_t *p)
{
	memset(p, 0, sizeof(*p));
}

int zlrs_mgmt_feed(zlrs_mgmt_parser_t *p, uint8_t byte)
{
	if (p->len == 0U) {
		if (byte != ZLRS_MGMT_SOF0) {
			p->frames_bad++;
			return -1;
		}
		p->buf[0] = byte;
		p->len = 1U;
		return 0;
	}

	if (p->len == 1U) {
		if (byte != ZLRS_MGMT_SOF1) {
			p->len = 0U;
			p->frames_bad++;
			return -1;
		}
		p->buf[1] = byte;
		p->len = 2U;
		return 0;
	}

	if (p->len == 2U) {
		if (byte != ZLRS_MGMT_VERSION) {
			p->len = 0U;
			p->frames_bad++;
			return -1;
		}
		p->buf[2] = byte;
		p->len = 3U;
		return 0;
	}

	if (p->len == 3U) {
		p->buf[3] = byte;
		p->len = 4U;
		return 0;
	}

	if (p->len == 4U) {
		if (byte > ZLRS_MGMT_PAYLOAD_MAX_LEN) {
			p->len = 0U;
			p->frames_bad++;
			return -1;
		}
		p->buf[4] = byte;
		p->len = 5U;
		p->need = (uint8_t)(byte + 6U); /* SOF(2) + ver + cmd + len + payload + crc */
		return 0;
	}

	if (p->len >= p->need) {
		p->len = 0U;
		return zlrs_mgmt_feed(p, byte);
	}

	p->buf[p->len] = byte;
	p->len++;

	if (p->len < p->need) {
		return 0;
	}

	const uint8_t total = p->need;
	const uint8_t crc = zlrs_crc8(&p->buf[2], (size_t)total - 3U);

	p->cmd = p->buf[3];
	p->payload_len = p->buf[4];
	p->len = 0U;
	p->need = 0U;

	if (crc != p->buf[total - 1U]) {
		p->frames_bad++;
		return -1;
	}

	p->frames_ok++;
	return 1;
}

uint8_t zlrs_mgmt_encode(uint8_t *out, size_t out_len, uint8_t cmd, const uint8_t *payload,
			 uint8_t payload_len)
{
	const uint8_t total = (uint8_t)(payload_len + 6U);

	if (out == NULL || out_len < total || payload_len > ZLRS_MGMT_PAYLOAD_MAX_LEN) {
		return 0U;
	}

	out[0] = ZLRS_MGMT_SOF0;
	out[1] = ZLRS_MGMT_SOF1;
	out[2] = ZLRS_MGMT_VERSION;
	out[3] = cmd;
	out[4] = payload_len;
	if (payload_len > 0U) {
		memcpy(&out[5], payload, payload_len);
	}
	out[total - 1U] = zlrs_crc8(&out[2], (size_t)payload_len + 3U);

	return total;
}

void zlrs_mgmt_status_encode(uint8_t out[ZLRS_MGMT_STATUS_LEN], const zlrs_mgmt_status_t *status)
{
	out[0] = status->version;
	out[1] = status->role;
	out[2] = status->state;
	out[3] = status->flags;
	memcpy(&out[4], status->dev_id, ZLRS_DEV_ID_LEN);
	memcpy(&out[8], status->peer_id, ZLRS_DEV_ID_LEN);
	out[12] = status->lq_percent;
	out[13] = status->band;
	out[14] = status->region;
	out[15] = (uint8_t)status->tx_power_dbm;
	out[16] = (uint8_t)(status->data_baud & 0xFFU);
	out[17] = (uint8_t)((status->data_baud >> 8) & 0xFFU);
	out[18] = (uint8_t)((status->data_baud >> 16) & 0xFFU);
	out[19] = (uint8_t)((status->data_baud >> 24) & 0xFFU);
	out[20] = status->profile;
}

bool zlrs_mgmt_status_decode(const uint8_t *in, uint8_t len, zlrs_mgmt_status_t *status)
{
	if (in == NULL || status == NULL || len < ZLRS_MGMT_STATUS_LEN) {
		return false;
	}

	status->version = in[0];
	status->role = in[1];
	status->state = in[2];
	status->flags = in[3];
	memcpy(status->dev_id, &in[4], ZLRS_DEV_ID_LEN);
	memcpy(status->peer_id, &in[8], ZLRS_DEV_ID_LEN);
	status->lq_percent = in[12];
	status->band = in[13];
	status->region = in[14];
	status->tx_power_dbm = (int8_t)in[15];
	status->data_baud = (uint32_t)in[16] | ((uint32_t)in[17] << 8) | ((uint32_t)in[18] << 16) |
			    ((uint32_t)in[19] << 24);
	status->profile = in[20];

	return true;
}
