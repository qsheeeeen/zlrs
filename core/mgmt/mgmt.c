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

static void put_u16(uint8_t *out, uint16_t value)
{
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *out, uint32_t value)
{
	put_u16(out, (uint16_t)value);
	put_u16(out + 2, (uint16_t)(value >> 16));
}

static uint16_t get_u16(const uint8_t *in)
{
	return (uint16_t)in[0] | ((uint16_t)in[1] << 8);
}

static uint32_t get_u32(const uint8_t *in)
{
	return (uint32_t)get_u16(in) | ((uint32_t)get_u16(in + 2) << 16);
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
	put_u32(out + 16, status->data_baud);
	out[20] = status->profile;
	put_u16(out + 21, (uint16_t)status->rssi_dbm);
	put_u16(out + 23, (uint16_t)status->peer_rssi_dbm);
	out[25] = status->peer_lq_percent;
	out[26] = (uint8_t)status->active_power_dbm;
	out[27] = (uint8_t)status->power_floor_dbm;
	put_u16(out + 28, (uint16_t)status->temperature_centi_c);
	put_u32(out + 30, status->tx_frames);
	put_u32(out + 34, status->rx_frames);
	put_u32(out + 38, status->rx_crc_errors);
	put_u32(out + 42, status->rx_bad_tag);
	put_u32(out + 46, status->tx_retries);
	put_u32(out + 50, status->data_drop_bytes);
	put_u32(out + 54, status->missed_slots);
	put_u32(out + 58, status->radio_fail);
	put_u32(out + 62, status->frequency_hz);
	put_u16(out + 66, status->hop_index);
	put_u32(out + 68, status->frame_period_us);
	put_u16(out + 72, (uint16_t)status->radio_temperature_centi_c);
	put_u16(out + 74, (uint16_t)status->radio_xosc_temperature_centi_c);
	put_u16(out + 76, status->radio_vbat_mv);
	out[78] = status->radio_version_major;
	out[79] = status->radio_version_minor;
	out[80] = status->radio_chip_mode;
	out[81] = status->radio_command_status;
	out[82] = status->radio_reset_status;
	out[83] = status->radio_valid;
	put_u16(out + 84, status->radio_errors);
	put_u32(out + 86, status->radio_irq);
}

bool zlrs_mgmt_status_decode(const uint8_t *in, uint8_t len, zlrs_mgmt_status_t *status)
{
	if (in == NULL || status == NULL || len != ZLRS_MGMT_STATUS_LEN) {
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
	status->data_baud = get_u32(in + 16);
	status->profile = in[20];
	status->rssi_dbm = (int16_t)get_u16(in + 21);
	status->peer_rssi_dbm = (int16_t)get_u16(in + 23);
	status->peer_lq_percent = in[25];
	status->active_power_dbm = (int8_t)in[26];
	status->power_floor_dbm = (int8_t)in[27];
	status->temperature_centi_c = (int16_t)get_u16(in + 28);
	status->tx_frames = get_u32(in + 30);
	status->rx_frames = get_u32(in + 34);
	status->rx_crc_errors = get_u32(in + 38);
	status->rx_bad_tag = get_u32(in + 42);
	status->tx_retries = get_u32(in + 46);
	status->data_drop_bytes = get_u32(in + 50);
	status->missed_slots = get_u32(in + 54);
	status->radio_fail = get_u32(in + 58);
	status->frequency_hz = get_u32(in + 62);
	status->hop_index = get_u16(in + 66);
	status->frame_period_us = get_u32(in + 68);
	status->radio_temperature_centi_c = (int16_t)get_u16(in + 72);
	status->radio_xosc_temperature_centi_c = (int16_t)get_u16(in + 74);
	status->radio_vbat_mv = get_u16(in + 76);
	status->radio_version_major = in[78];
	status->radio_version_minor = in[79];
	status->radio_chip_mode = in[80];
	status->radio_command_status = in[81];
	status->radio_reset_status = in[82];
	status->radio_valid = in[83];
	status->radio_errors = get_u16(in + 84);
	status->radio_irq = get_u32(in + 86);
	return true;
}
