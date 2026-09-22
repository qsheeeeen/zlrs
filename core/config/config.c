/*
 * Config protocol, see include/zlrs/config/config.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/config/config.h"

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

void zlrs_cfg_parser_init(zlrs_cfg_parser_t *p)
{
	memset(p, 0, sizeof(*p));
}

int zlrs_cfg_feed(zlrs_cfg_parser_t *p, uint8_t byte)
{
	if (p->len == 0U) {
		if (byte != ZLRS_CFG_SOF0) {
			p->frames_bad++;
			return -1;
		}
		p->buf[0] = byte;
		p->len = 1U;
		return 0;
	}

	if (p->len == 1U) {
		if (byte != ZLRS_CFG_SOF1) {
			p->len = 0U;
			p->frames_bad++;
			return -1;
		}
		p->buf[1] = byte;
		p->len = 2U;
		return 0;
	}

	if (p->len == 2U) {
		if (byte != ZLRS_CFG_VERSION) {
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
		if (byte > ZLRS_CFG_PAYLOAD_MAX_LEN) {
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
		return zlrs_cfg_feed(p, byte);
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

uint8_t zlrs_cfg_encode(uint8_t *out, size_t out_len, uint8_t cmd, const uint8_t *payload,
			uint8_t payload_len)
{
	const uint8_t total = (uint8_t)(payload_len + 6U);

	if (out == NULL || out_len < total || payload_len > ZLRS_CFG_PAYLOAD_MAX_LEN) {
		return 0U;
	}

	out[0] = ZLRS_CFG_SOF0;
	out[1] = ZLRS_CFG_SOF1;
	out[2] = ZLRS_CFG_VERSION;
	out[3] = cmd;
	out[4] = payload_len;
	if (payload_len > 0U) {
		memcpy(&out[5], payload, payload_len);
	}
	out[total - 1U] = zlrs_crc8(&out[2], (size_t)payload_len + 3U);

	return total;
}

void zlrs_cfg_status_encode(uint8_t out[ZLRS_CFG_STATUS_LEN], const zlrs_cfg_status_t *status)
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

bool zlrs_cfg_status_decode(const uint8_t *in, uint8_t len, zlrs_cfg_status_t *status)
{
	if (in == NULL || status == NULL || len < ZLRS_CFG_STATUS_LEN) {
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

	return zlrs_cfg_encode(out, out_len, ZLRS_CFG_CMD_ACK, payload, sizeof(payload));
}

static uint8_t handle_set_key(const zlrs_cfg_backend_t *backend, const uint8_t *payload,
			      uint8_t len, uint8_t *out, size_t out_len, uint8_t cmd)
{
	uint8_t key[ZLRS_KEY_LEN];
	bool ok = true;

	if (len != 2U * ZLRS_KEY_LEN || backend->save_root_key == NULL) {
		return ack_frame(out, out_len, cmd, ZLRS_CFG_E_PARAM);
	}

	for (uint8_t i = 0U; i < ZLRS_KEY_LEN; i++) {
		const uint8_t hi = hex_nibble((char)payload[i * 2U], &ok);
		const uint8_t lo = hex_nibble((char)payload[i * 2U + 1U], &ok);

		key[i] = (uint8_t)((hi << 4) | lo);
	}

	if (!ok) {
		memset(key, 0, sizeof(key));
		return ack_frame(out, out_len, cmd, ZLRS_CFG_E_PARAM);
	}

	const int rc = backend->save_root_key(backend->ctx, key);

	memset(key, 0, sizeof(key));

	return ack_frame(out, out_len, cmd, (uint8_t)rc);
}

static uint8_t handle_set_phrase(const zlrs_cfg_backend_t *backend, const uint8_t *payload,
				 uint8_t len, uint8_t *out, size_t out_len, uint8_t cmd)
{
	uint8_t key[ZLRS_KEY_LEN];
	char phrase[ZLRS_CFG_PHRASE_MAX_LEN + 1U];
	int rc;

	if (len == 0U || len > ZLRS_CFG_PHRASE_MAX_LEN || backend->save_root_key == NULL) {
		return ack_frame(out, out_len, cmd, ZLRS_CFG_E_PARAM);
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

uint8_t zlrs_cfg_dispatch(const zlrs_cfg_backend_t *backend, uint8_t cmd, const uint8_t *payload,
			  uint8_t payload_len, uint8_t *out, size_t out_len)
{
	if (backend == NULL || out == NULL) {
		return 0U;
	}

	switch (cmd) {
	case ZLRS_CFG_CMD_ENTER:
		/* Entering config mode is the application's job outside a session; here we are already in one */
		return ack_frame(out, out_len, cmd, ZLRS_CFG_OK);
	case ZLRS_CFG_CMD_STATUS: {
		zlrs_cfg_status_t status;
		uint8_t body[ZLRS_CFG_STATUS_LEN];

		if (backend->read_status == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_CFG_E_STATE);
		}

		memset(&status, 0, sizeof(status));
		const int rc = backend->read_status(backend->ctx, &status);

		if (rc != ZLRS_CFG_OK) {
			return ack_frame(out, out_len, cmd, (uint8_t)rc);
		}

		zlrs_cfg_status_encode(body, &status);

		return zlrs_cfg_encode(out, out_len, ZLRS_CFG_CMD_STATUS_RESP, body, sizeof(body));
	}
	case ZLRS_CFG_CMD_BUILD: {
		char build[ZLRS_CFG_BUILD_MAX_LEN] = {0};

		if (backend->read_build == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_CFG_E_STATE);
		}

		const int rc = backend->read_build(backend->ctx, build, sizeof(build));

		if (rc != ZLRS_CFG_OK) {
			return ack_frame(out, out_len, cmd, (uint8_t)rc);
		}

		/* Send the build string byte by byte, without the trailing NUL padding */
		uint8_t n = 0U;

		while (n < sizeof(build) && build[n] != '\0') {
			n++;
		}

		return zlrs_cfg_encode(out, out_len, ZLRS_CFG_CMD_BUILD_RESP,
				       (const uint8_t *)build, n);
	}
	case ZLRS_CFG_CMD_CONFIRM:
		if (backend->confirm_image == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_CFG_E_STATE);
		}
		return ack_frame(out, out_len, cmd, (uint8_t)backend->confirm_image(backend->ctx));
	case ZLRS_CFG_CMD_IMAGE_HASH: {
		uint8_t hash[ZLRS_CFG_IMAGE_HASH_LEN];

		if (backend->read_image_hash == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_CFG_E_STATE);
		}

		memset(hash, 0, sizeof(hash));

		const int rc = backend->read_image_hash(backend->ctx, hash);

		if (rc != ZLRS_CFG_OK) {
			return ack_frame(out, out_len, cmd, (uint8_t)rc);
		}

		return zlrs_cfg_encode(out, out_len, ZLRS_CFG_CMD_IMAGE_HASH_RESP, hash,
				       sizeof(hash));
	}
	case ZLRS_CFG_CMD_PAIR:
		if (backend->pair == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_CFG_E_STATE);
		}
		return ack_frame(out, out_len, cmd, (uint8_t)backend->pair(backend->ctx));
	case ZLRS_CFG_CMD_ERASE_PAIR:
		if (backend->erase_pair == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_CFG_E_STATE);
		}
		return ack_frame(out, out_len, cmd, (uint8_t)backend->erase_pair(backend->ctx));
	case ZLRS_CFG_CMD_SET_KEY_HEX:
		return handle_set_key(backend, payload, payload_len, out, out_len, cmd);
	case ZLRS_CFG_CMD_SET_PHRASE:
		return handle_set_phrase(backend, payload, payload_len, out, out_len, cmd);
	case ZLRS_CFG_CMD_SET_FAILSAFE:
		if (payload_len != 22U || backend->save_failsafe == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_CFG_E_PARAM);
		}
		return ack_frame(out, out_len, cmd,
				 (uint8_t)backend->save_failsafe(backend->ctx, payload));
	case ZLRS_CFG_CMD_SET_BAUD: {
		uint32_t baud;

		if (payload_len != 4U || backend->save_baud == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_CFG_E_PARAM);
		}

		baud = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) |
		       ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);

		return ack_frame(out, out_len, cmd,
				 (uint8_t)backend->save_baud(backend->ctx, baud));
	}
	case ZLRS_CFG_CMD_SET_REGION:
		if (payload_len != 1U || backend->save_region == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_CFG_E_PARAM);
		}
		return ack_frame(out, out_len, cmd,
				 (uint8_t)backend->save_region(backend->ctx, payload[0]));
	case ZLRS_CFG_CMD_SET_POWER:
		if (payload_len != 1U || backend->save_power == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_CFG_E_PARAM);
		}
		return ack_frame(out, out_len, cmd,
				 (uint8_t)backend->save_power(backend->ctx, (int8_t)payload[0]));
	case ZLRS_CFG_CMD_SET_ROLE:
		/* 0 = TX, 1 = RX; the application decides whether the value is usable */
		if (payload_len != 1U || payload[0] > ZLRS_ROLE_RX || backend->save_role == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_CFG_E_PARAM);
		}
		return ack_frame(out, out_len, cmd, (uint8_t)backend->save_role(backend->ctx, payload[0]));
	case ZLRS_CFG_CMD_SET_PROFILE:
		/* 0 = P0, 1 = P1, 2 = P2; whether a profile may be selected is the application's call */
		if (payload_len != 1U || payload[0] > ZLRS_PROFILE_P2 ||
		    backend->save_profile == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_CFG_E_PARAM);
		}
		return ack_frame(out, out_len, cmd,
				 (uint8_t)backend->save_profile(backend->ctx, payload[0]));
	case ZLRS_CFG_CMD_NEW_DEV_ID:
		if (payload_len != 0U || backend->new_dev_id == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_CFG_E_PARAM);
		}
		return ack_frame(out, out_len, cmd, (uint8_t)backend->new_dev_id(backend->ctx));
	case ZLRS_CFG_CMD_EXIT:
		/* The application handles the exit (usually a reboot), here we only acknowledge it */
		return ack_frame(out, out_len, cmd, ZLRS_CFG_OK);
	case ZLRS_CFG_CMD_FACTORY_RESET:
		if (backend->factory_reset == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_CFG_E_STATE);
		}
		return ack_frame(out, out_len, cmd, (uint8_t)backend->factory_reset(backend->ctx));
	case ZLRS_CFG_CMD_RECOVERY:
		if (backend->enter_recovery == NULL) {
			return ack_frame(out, out_len, cmd, ZLRS_CFG_E_STATE);
		}
		return ack_frame(out, out_len, cmd, (uint8_t)backend->enter_recovery(backend->ctx));
	default:
		return ack_frame(out, out_len, cmd, ZLRS_CFG_E_UNKNOWN);
	}
}
