/*
 * Management protocol, see include/zlrs/mgmt/mgmt.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/mgmt/mgmt.h"

#include <string.h>

#include "zlrs/proto/crc.h"
#include "zlrs/proto/crypto.h"
#include "zlrs/proto/frame.h"

static uint8_t hex_nibble(char c, bool *ok)
{
	if (c >= '0' && c <= '9') {
		return (uint8_t)(c - '0');
	}
	if (c >= 'a' && c <= 'f') {
		return (uint8_t)(c - 'a' + 10);
	}
	if (c >= 'A' && c <= 'F') {
		return (uint8_t)(c - 'A' + 10);
	}

	*ok = false;
	return 0U;
}

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

static uint8_t ack_frame(uint8_t *out, size_t out_len, uint8_t cmd, uint8_t status)
{
	uint8_t payload[2] = {cmd, status};

	return zlrs_mgmt_encode(out, out_len, ZLRS_MGMT_CMD_ACK, payload, sizeof(payload));
}

/*
 * The simple settings all take the same shape: the format fixes the payload length, and what the
 * bytes mean (and whether the value is usable) is the backend's business.
 */
static uint8_t handle_setting(const zlrs_mgmt_backend_t *backend, uint8_t cmd,
			      const uint8_t *payload, uint8_t payload_len, uint8_t want_len,
			      uint8_t *out, size_t out_len)
{
	if (payload_len != want_len || backend->save_setting == NULL) {
		return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_PARAM);
	}

	return ack_frame(out, out_len, cmd,
			 (uint8_t)backend->save_setting(backend->ctx, cmd, payload, payload_len));
}

static uint8_t handle_set_key(const zlrs_mgmt_backend_t *backend, const uint8_t *payload,
			      uint8_t len, uint8_t *out, size_t out_len, uint8_t cmd)
{
	uint8_t key[ZLRS_KEY_LEN];
	bool ok = true;

	if (len != 2U * ZLRS_KEY_LEN || backend->save_root_key == NULL) {
		return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_PARAM);
	}

	for (uint8_t i = 0U; i < ZLRS_KEY_LEN; i++) {
		const uint8_t hi = hex_nibble((char)payload[i * 2U], &ok);
		const uint8_t lo = hex_nibble((char)payload[i * 2U + 1U], &ok);

		key[i] = (uint8_t)((hi << 4) | lo);
	}

	if (!ok) {
		memset(key, 0, sizeof(key));
		return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_PARAM);
	}

	const int rc = backend->save_root_key(backend->ctx, key);

	memset(key, 0, sizeof(key));

	return ack_frame(out, out_len, cmd, (uint8_t)rc);
}

static uint8_t handle_set_phrase(const zlrs_mgmt_backend_t *backend, const uint8_t *payload,
				 uint8_t len, uint8_t *out, size_t out_len, uint8_t cmd)
{
	uint8_t key[ZLRS_KEY_LEN];
	char phrase[ZLRS_MGMT_PHRASE_MAX_LEN + 1U];
	int rc;

	if (len == 0U || len > ZLRS_MGMT_PHRASE_MAX_LEN || backend->save_root_key == NULL) {
		return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_PARAM);
	}

	memcpy(phrase, payload, len);
	phrase[len] = '\0';

	/* The derivation is fixed on the device: the salt carries a version, the tool cannot pick rounds */
	zlrs_root_key_from_phrase(phrase, key);
	rc = backend->save_root_key(backend->ctx, key);

	memset(key, 0, sizeof(key));
	memset(phrase, 0, sizeof(phrase));

	return ack_frame(out, out_len, cmd, (uint8_t)rc);
}

uint8_t zlrs_mgmt_dispatch(const zlrs_mgmt_backend_t *backend, uint8_t cmd, const uint8_t *payload,
			   uint8_t payload_len, uint8_t *out, size_t out_len)
{
	if (backend == NULL || out == NULL) {
		return 0U;
	}

	switch (cmd) {
	case ZLRS_MGMT_CMD_ENTER:
		/* Entering the mode is the application's job outside a session; here we are already in one */
		return ack_frame(out, out_len, cmd, ZLRS_MGMT_OK);
	case ZLRS_MGMT_CMD_STATUS: {
		zlrs_mgmt_status_t status;
		uint8_t body[ZLRS_MGMT_STATUS_LEN];

		if (backend->read_status == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_STATE);
		}

		memset(&status, 0, sizeof(status));
		const int rc = backend->read_status(backend->ctx, &status);

		if (rc != ZLRS_MGMT_OK) {
			return ack_frame(out, out_len, cmd, (uint8_t)rc);
		}

		zlrs_mgmt_status_encode(body, &status);

		return zlrs_mgmt_encode(out, out_len, ZLRS_MGMT_CMD_STATUS_RESP, body,
					sizeof(body));
	}
	case ZLRS_MGMT_CMD_BUILD: {
		char build[ZLRS_MGMT_BUILD_MAX_LEN] = {0};

		if (backend->read_build == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_STATE);
		}

		const int rc = backend->read_build(backend->ctx, build, sizeof(build));

		if (rc != ZLRS_MGMT_OK) {
			return ack_frame(out, out_len, cmd, (uint8_t)rc);
		}

		/* Send the build string byte by byte, without the trailing NUL padding */
		uint8_t n = 0U;

		while (n < sizeof(build) && build[n] != '\0') {
			n++;
		}

		return zlrs_mgmt_encode(out, out_len, ZLRS_MGMT_CMD_BUILD_RESP,
					(const uint8_t *)build, n);
	}
	case ZLRS_MGMT_CMD_CONFIRM:
		if (backend->confirm_image == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_STATE);
		}
		return ack_frame(out, out_len, cmd, (uint8_t)backend->confirm_image(backend->ctx));
	case ZLRS_MGMT_CMD_IMAGE_HASH: {
		uint8_t hash[ZLRS_MGMT_IMAGE_HASH_LEN];

		if (backend->read_image_hash == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_STATE);
		}

		memset(hash, 0, sizeof(hash));

		const int rc = backend->read_image_hash(backend->ctx, hash);

		if (rc != ZLRS_MGMT_OK) {
			return ack_frame(out, out_len, cmd, (uint8_t)rc);
		}

		return zlrs_mgmt_encode(out, out_len, ZLRS_MGMT_CMD_IMAGE_HASH_RESP, hash,
					sizeof(hash));
	}
	case ZLRS_MGMT_CMD_PAIR:
		if (backend->pair == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_STATE);
		}
		return ack_frame(out, out_len, cmd, (uint8_t)backend->pair(backend->ctx));
	case ZLRS_MGMT_CMD_ERASE_PAIR:
		if (backend->erase_pair == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_STATE);
		}
		return ack_frame(out, out_len, cmd, (uint8_t)backend->erase_pair(backend->ctx));
	case ZLRS_MGMT_CMD_SET_KEY_HEX:
		return handle_set_key(backend, payload, payload_len, out, out_len, cmd);
	case ZLRS_MGMT_CMD_SET_PHRASE:
		return handle_set_phrase(backend, payload, payload_len, out, out_len, cmd);
	case ZLRS_MGMT_CMD_SET_BAUD:
		/* 4-byte little-endian baud rate; the value itself is checked by the backend */
		return handle_setting(backend, cmd, payload, payload_len, 4U, out, out_len);
	case ZLRS_MGMT_CMD_SET_REGION:
	case ZLRS_MGMT_CMD_SET_POWER:
		/* 1 byte: a region id, or a signed dBm value */
		return handle_setting(backend, cmd, payload, payload_len, 1U, out, out_len);
	case ZLRS_MGMT_CMD_SET_ROLE:
		/* 0 = TX, 1 = RX; out of range is a protocol error, unusable is the backend's call */
		if (payload_len == 1U && payload[0] > ZLRS_ROLE_RX) {
			return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_PARAM);
		}
		return handle_setting(backend, cmd, payload, payload_len, 1U, out, out_len);
	case ZLRS_MGMT_CMD_SET_BAND:
		if (payload_len == 1U &&
		    (payload[0] < ZLRS_DATA_BAND_24 || payload[0] > ZLRS_DATA_BAND_SUBG)) {
			return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_PARAM);
		}
		return handle_setting(backend, cmd, payload, payload_len, 1U, out, out_len);
	case ZLRS_MGMT_CMD_SET_PROFILE:
		/* 0 = P0, 1 = P1, 2 = P2; whether a profile may be selected is the backend's call */
		if (payload_len == 1U && payload[0] > ZLRS_PROFILE_P2) {
			return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_PARAM);
		}
		return handle_setting(backend, cmd, payload, payload_len, 1U, out, out_len);
	case ZLRS_MGMT_CMD_NEW_DEV_ID:
		if (payload_len != 0U || backend->new_dev_id == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_PARAM);
		}
		return ack_frame(out, out_len, cmd, (uint8_t)backend->new_dev_id(backend->ctx));
	case ZLRS_MGMT_CMD_EXIT:
		/* The application handles the exit (usually a reboot), here we only acknowledge it */
		return ack_frame(out, out_len, cmd, ZLRS_MGMT_OK);
	case ZLRS_MGMT_CMD_FACTORY_RESET:
		if (backend->factory_reset == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_STATE);
		}
		return ack_frame(out, out_len, cmd, (uint8_t)backend->factory_reset(backend->ctx));
	case ZLRS_MGMT_CMD_RECOVERY:
		if (backend->enter_recovery == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_STATE);
		}
		return ack_frame(out, out_len, cmd, (uint8_t)backend->enter_recovery(backend->ctx));
	case ZLRS_MGMT_CMD_LOG_INFO: {
		zlrs_mgmt_log_info_t info;
		uint8_t body[2U + (6U * ZLRS_MGMT_LOG_FILES_MAX)];
		uint8_t n = 0U;

		if (backend->log_info == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_STATE);
		}

		memset(&info, 0, sizeof(info));

		const int rc = backend->log_info(backend->ctx, &info);

		if (rc != ZLRS_MGMT_OK) {
			return ack_frame(out, out_len, cmd, (uint8_t)rc);
		}

		body[n++] = info.count;
		body[n++] = info.newest;

		for (uint8_t i = 0U; i < info.count; i++) {
			body[n++] = (uint8_t)(info.num[i] & 0xFFU);
			body[n++] = (uint8_t)((info.num[i] >> 8) & 0xFFU);
			body[n++] = (uint8_t)(info.size[i] & 0xFFU);
			body[n++] = (uint8_t)((info.size[i] >> 8) & 0xFFU);
			body[n++] = (uint8_t)((info.size[i] >> 16) & 0xFFU);
			body[n++] = (uint8_t)((info.size[i] >> 24) & 0xFFU);
		}

		return zlrs_mgmt_encode(out, out_len, ZLRS_MGMT_CMD_LOG_INFO_RESP, body, n);
	}
	case ZLRS_MGMT_CMD_LOG_READ: {
		const uint8_t chunk = 1U + 4U + ZLRS_MGMT_LOG_CHUNK_MAX;
		uint8_t body[chunk];
		uint8_t got = 0U;

		if (backend->log_read == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_STATE);
		}
		if (payload_len != 6U || payload[5] > ZLRS_MGMT_LOG_CHUNK_MAX) {
			return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_PARAM);
		}

		const uint32_t offset = (uint32_t)payload[1] | ((uint32_t)payload[2] << 8) |
					((uint32_t)payload[3] << 16) | ((uint32_t)payload[4] << 24);
		const int rc = backend->log_read(backend->ctx, payload[0], offset, payload[5],
						 &body[5], &got);

		if (rc != ZLRS_MGMT_OK) {
			return ack_frame(out, out_len, cmd, (uint8_t)rc);
		}

		body[0] = payload[0];
		body[1] = (uint8_t)(offset & 0xFFU);
		body[2] = (uint8_t)((offset >> 8) & 0xFFU);
		body[3] = (uint8_t)((offset >> 16) & 0xFFU);
		body[4] = (uint8_t)((offset >> 24) & 0xFFU);

		return zlrs_mgmt_encode(out, out_len, ZLRS_MGMT_CMD_LOG_DATA_RESP, body,
					(uint8_t)(5U + got));
	}
	default:
		return ack_frame(out, out_len, cmd, ZLRS_MGMT_E_UNKNOWN);
	}
}
