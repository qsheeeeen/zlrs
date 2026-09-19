/*
 * Config protocol: runs on the config CDC while in config mode (table: doc/10-security.md 10).
 * Pure C framing and dispatch: persistence and link actions go to the application through
 * backend callbacks, so the host tests can run every command against a fake one.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zlrs/proto/config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZLRS_CFG_SOF0            0xA5U
#define ZLRS_CFG_SOF1            0x5AU
#define ZLRS_CFG_VERSION         3U
#define ZLRS_CFG_FRAME_MAX_LEN   96U
#define ZLRS_CFG_PAYLOAD_MAX_LEN (ZLRS_CFG_FRAME_MAX_LEN - 6U)

/* host -> device */
#define ZLRS_CFG_CMD_ENTER         0x00U /* enter config mode (the formal entry) */
#define ZLRS_CFG_CMD_STATUS        0x01U /* read status */
#define ZLRS_CFG_CMD_PAIR          0x02U /* start pairing */
#define ZLRS_CFG_CMD_ERASE_PAIR    0x03U /* erase the pairing record */
#define ZLRS_CFG_CMD_SET_KEY_HEX   0x10U /* root key as 64 hex characters */
#define ZLRS_CFG_CMD_SET_PHRASE    0x11U /* passphrase, derived into a root key on the device */
#define ZLRS_CFG_CMD_BUILD         0x12U /* read the build string (which version is running) */
#define ZLRS_CFG_CMD_IMAGE_HASH    0x13U /* MCUboot hash of this image (upgrade/rollback verdict) */
#define ZLRS_CFG_CMD_SET_FAILSAFE  0x20U /* 22-byte CRSF channel payload */
#define ZLRS_CFG_CMD_SET_BAUD      0x21U /* 4-byte little-endian baud rate */
#define ZLRS_CFG_CMD_SET_REGION    0x22U /* 1-byte region id (0..3) */
#define ZLRS_CFG_CMD_SET_POWER     0x23U /* 1-byte signed dBm */
#define ZLRS_CFG_CMD_SET_ROLE      0x24U /* 1-byte role (0 = TX, 1 = RX) */
#define ZLRS_CFG_CMD_SET_PROFILE   0x25U /* 1-byte profile (0 = P0, 1 = P1, 2 = P2) */
#define ZLRS_CFG_CMD_EXIT          0x30U /* leave config mode */
#define ZLRS_CFG_CMD_FACTORY_RESET 0x40U /* wipe NVS */
#define ZLRS_CFG_CMD_RECOVERY      0x41U /* reboot into MCUboot serial recovery */
#define ZLRS_CFG_CMD_CONFIRM       0x42U /* host confirms this upgrade (no confirm = rollback) */

/* device -> host */
#define ZLRS_CFG_CMD_ACK             0x80U /* [cmd][status] */
#define ZLRS_CFG_CMD_STATUS_RESP     0x81U /* see zlrs_cfg_status_t */
#define ZLRS_CFG_CMD_BUILD_RESP      0x82U /* build string, at most ZLRS_CFG_BUILD_MAX_LEN */
#define ZLRS_CFG_CMD_IMAGE_HASH_RESP 0x83U /* ZLRS_CFG_IMAGE_HASH_LEN bytes of image hash */

/* status codes */
#define ZLRS_CFG_OK        0U
#define ZLRS_CFG_E_PARAM   1U
#define ZLRS_CFG_E_STATE   2U
#define ZLRS_CFG_E_STORE   3U
#define ZLRS_CFG_E_UNKNOWN 4U

/* Status response: a fixed 21 bytes; changing the field order means bumping the version */
#define ZLRS_CFG_STATUS_LEN     21U
#define ZLRS_CFG_PHRASE_MAX_LEN 32U
#define ZLRS_CFG_BUILD_MAX_LEN  32U
#define ZLRS_CFG_IMAGE_HASH_LEN 32U

typedef struct {
	uint8_t version; /* protocol version */
	uint8_t role;    /* 0 = TX, 1 = RX */
	uint8_t state;   /* ZLRS_STATE_* */
	uint8_t flags;   /* bit0 root key set, bit1 paired, bit2 session valid */
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	uint8_t lq_percent;
	uint8_t band;        /* local sub-G hardware: 0 = LF (470/433), 1 = HF (868/915) */
	uint8_t region;      /* current region id (0..3) */
	uint8_t profile;     /* active security profile (ZLRS_PROFILE_*) */
	int8_t tx_power_dbm; /* current TX power ceiling */
	uint32_t data_baud;  /* data port baud rate (effective on the next boot) */
} zlrs_cfg_status_t;

#define ZLRS_CFG_FLAG_KEY     0x01U
#define ZLRS_CFG_FLAG_PAIRED  0x02U
#define ZLRS_CFG_FLAG_SESSION 0x04U

/*
 * Device-side actions. Each callback returns ZLRS_CFG_OK or ZLRS_CFG_E_*; ctx comes from
 * the caller and can be a real NVS write or a fake in the host tests.
 */
typedef struct {
	int (*save_root_key)(void *ctx, const uint8_t key[ZLRS_KEY_LEN]);
	int (*read_status)(void *ctx, zlrs_cfg_status_t *out);
	int (*save_failsafe)(void *ctx, const uint8_t channels[22]);
	int (*save_baud)(void *ctx, uint32_t baud);
	int (*save_region)(void *ctx, uint8_t region);
	int (*save_power)(void *ctx, int8_t dbm);
	/* Role and security profile are runtime settings too; both take effect on the next boot */
	int (*save_role)(void *ctx, uint8_t role);
	int (*save_profile)(void *ctx, uint8_t profile);
	int (*pair)(void *ctx);
	int (*erase_pair)(void *ctx);
	int (*factory_reset)(void *ctx);
	int (*enter_recovery)(void *ctx);
	/* Host confirms this image: OK means it is recorded (the upgrade flow depends on it) */
	int (*confirm_image)(void *ctx);
	/* build string; E_STATE when unimplemented */
	int (*read_build)(void *ctx, char *out, size_t out_len);
	/* MCUboot hash of the running image (new build or rolled back one, the tool decides) */
	int (*read_image_hash)(void *ctx, uint8_t out[ZLRS_CFG_IMAGE_HASH_LEN]);
	void *ctx;
} zlrs_cfg_backend_t;

/* Byte-at-a-time config frame parser */
typedef struct {
	uint8_t buf[ZLRS_CFG_FRAME_MAX_LEN];
	uint8_t len;
	uint8_t need;
	uint8_t cmd;
	uint8_t payload_len;
	uint32_t frames_ok;
	uint32_t frames_bad;
} zlrs_cfg_parser_t;

#define ZLRS_CFG_PAYLOAD(p) ((p)->buf + 5U)

void zlrs_cfg_parser_init(zlrs_cfg_parser_t *p);

/* 1 = a full frame is ready (cmd/payload readable), 0 = not yet, -1 = this byte is invalid */
int zlrs_cfg_feed(zlrs_cfg_parser_t *p, uint8_t byte);

/* Build a frame; returns its length, or 0 for invalid arguments */
uint8_t zlrs_cfg_encode(uint8_t *out, size_t out_len, uint8_t cmd, const uint8_t *payload,
			uint8_t payload_len);

/*
 * Handle one command and write the response frame into out (SOF and CRC included).
 * Returns the response length; 0 means this command needs no response.
 */
uint8_t zlrs_cfg_dispatch(const zlrs_cfg_backend_t *backend, uint8_t cmd, const uint8_t *payload,
			  uint8_t payload_len, uint8_t *out, size_t out_len);

/* status struct <-> fixed-size bytes */
void zlrs_cfg_status_encode(uint8_t out[ZLRS_CFG_STATUS_LEN], const zlrs_cfg_status_t *status);
bool zlrs_cfg_status_decode(const uint8_t *in, uint8_t len, zlrs_cfg_status_t *status);

#ifdef __cplusplus
}
#endif
