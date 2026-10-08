/*
 * Management protocol: runs on the management CDC while in management mode
 * (table: doc/09-security.md 9). Pure C framing and dispatch: persistence and link actions go to
 * the application through backend callbacks, so the host tests can run every command against a
 * fake one.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zlrs/proto/limits.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZLRS_MGMT_SOF0            0xA5U
#define ZLRS_MGMT_SOF1            0x5AU
#define ZLRS_MGMT_VERSION         3U
#define ZLRS_MGMT_FRAME_MAX_LEN   96U
#define ZLRS_MGMT_PAYLOAD_MAX_LEN (ZLRS_MGMT_FRAME_MAX_LEN - 6U)

/* host -> device */
#define ZLRS_MGMT_CMD_ENTER         0x00U /* enter management mode (the formal entry) */
#define ZLRS_MGMT_CMD_STATUS        0x01U /* read status */
#define ZLRS_MGMT_CMD_PAIR          0x02U /* start pairing */
#define ZLRS_MGMT_CMD_ERASE_PAIR    0x03U /* erase the pairing record */
#define ZLRS_MGMT_CMD_SET_KEY_HEX   0x10U /* root key as 64 hex characters */
#define ZLRS_MGMT_CMD_SET_PHRASE    0x11U /* passphrase, derived into a root key on the device */
#define ZLRS_MGMT_CMD_BUILD         0x12U /* read the build string (which version is running) */
#define ZLRS_MGMT_CMD_IMAGE_HASH    0x13U /* MCUboot hash of this image (the update verdict) */
#define ZLRS_MGMT_CMD_SET_BAUD      0x21U /* 4-byte little-endian baud rate */
#define ZLRS_MGMT_CMD_SET_REGION    0x22U /* 1-byte region id (0..3) */
#define ZLRS_MGMT_CMD_SET_POWER     0x23U /* 1-byte signed dBm */
#define ZLRS_MGMT_CMD_SET_ROLE      0x24U /* 1-byte role (0 = TX, 1 = RX) */
#define ZLRS_MGMT_CMD_SET_PROFILE   0x25U /* 1-byte profile (0 = P0, 1 = P1, 2 = P2) */
#define ZLRS_MGMT_CMD_SET_MODE      0x27U /* 0 = mixed, 1 = data24; effective after reboot */
#define ZLRS_MGMT_CMD_NEW_DEV_ID    0x26U /* make a fresh device id (drops the pairing record) */
#define ZLRS_MGMT_CMD_EXIT          0x30U /* leave management mode */
#define ZLRS_MGMT_CMD_FACTORY_RESET 0x40U /* wipe NVS */
#define ZLRS_MGMT_CMD_RECOVERY      0x41U /* reboot into MCUboot serial recovery */
#define ZLRS_MGMT_CMD_CONFIRM       0x42U /* host confirms this upgrade (no confirm = rollback) */
#define ZLRS_MGMT_CMD_LOG_INFO      0x50U /* what the log store holds */
#define ZLRS_MGMT_CMD_LOG_READ      0x51U /* one chunk: log index (1) + offset (4) + length (1) */
#define ZLRS_MGMT_CMD_DEBUG_REPORT  0x60U /* debug image only: report id (1) + offset (2) */

/* device -> host */
#define ZLRS_MGMT_CMD_ACK               0x80U /* [cmd][status] */
#define ZLRS_MGMT_CMD_STATUS_RESP       0x81U /* see zlrs_mgmt_status_t */
#define ZLRS_MGMT_CMD_BUILD_RESP        0x82U /* build string, at most ZLRS_MGMT_BUILD_MAX_LEN */
#define ZLRS_MGMT_CMD_IMAGE_HASH_RESP   0x83U /* ZLRS_MGMT_IMAGE_HASH_LEN bytes of image hash */
#define ZLRS_MGMT_CMD_LOG_INFO_RESP     0x90U /* see zlrs_mgmt_log_info_t */
#define ZLRS_MGMT_CMD_LOG_DATA_RESP     0x91U /* log index (1) + offset (4) + bytes */
#define ZLRS_MGMT_CMD_DEBUG_REPORT_RESP 0x84U /* report id + offset + total + text chunk */

#define ZLRS_MGMT_DEBUG_REPORT_LINK       1U
#define ZLRS_MGMT_DEBUG_REPORT_RATE       2U
#define ZLRS_MGMT_DEBUG_REPORT_RADIO      3U
#define ZLRS_MGMT_DEBUG_REPORT_POWER      4U
#define ZLRS_MGMT_DEBUG_REPORT_REQ_LEN    3U
#define ZLRS_MGMT_DEBUG_REPORT_HEADER_LEN 5U
#define ZLRS_MGMT_DEBUG_REPORT_CHUNK_MAX                                                           \
	(ZLRS_MGMT_PAYLOAD_MAX_LEN - ZLRS_MGMT_DEBUG_REPORT_HEADER_LEN)

/* status codes */
#define ZLRS_MGMT_OK        0U
#define ZLRS_MGMT_E_PARAM   1U
#define ZLRS_MGMT_E_STATE   2U
#define ZLRS_MGMT_E_STORE   3U
#define ZLRS_MGMT_E_UNKNOWN 4U

/* Status response: a fixed 21 bytes; changing the field order means bumping the version */
#define ZLRS_MGMT_STATUS_LEN     21U
#define ZLRS_MGMT_PHRASE_MAX_LEN 32U
#define ZLRS_MGMT_BUILD_MAX_LEN  32U
#define ZLRS_MGMT_IMAGE_HASH_LEN 32U
/* Log store: at most this many files, and this many bytes back per read (one frame's payload) */
#define ZLRS_MGMT_LOG_FILES_MAX  8U
#define ZLRS_MGMT_LOG_CHUNK_MAX  80U

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
} zlrs_mgmt_status_t;

#define ZLRS_MGMT_FLAG_KEY     0x01U
#define ZLRS_MGMT_FLAG_PAIRED  0x02U
#define ZLRS_MGMT_FLAG_SESSION 0x04U
#define ZLRS_MGMT_FLAG_DATA24  0x08U /* active mode: data only, exclusively 2.4 GHz */

#define ZLRS_MODE_MIXED  0U
#define ZLRS_MODE_DATA24 1U

/*
 * What the log store holds: how many files, which one is being appended to (the export walks them
 * oldest first, so the host needs to know where the ring starts) and the size of each.
 */
typedef struct {
	uint8_t count;  /* files present, 0 when there is no store on this board */
	uint8_t newest; /* index of the file the backend appends to */
	uint16_t num[ZLRS_MGMT_LOG_FILES_MAX]; /* the number in the file's name, oldest first */
	uint32_t size[ZLRS_MGMT_LOG_FILES_MAX];
} zlrs_mgmt_log_info_t;

/*
 * Device-side actions. Each callback returns ZLRS_MGMT_OK or ZLRS_MGMT_E_*; ctx comes from
 * the caller and can be a real NVS write or a fake in the host tests.
 */
typedef struct {
	int (*save_root_key)(void *ctx, const uint8_t key[ZLRS_KEY_LEN]);
	int (*read_status)(void *ctx, zlrs_mgmt_status_t *out);
	/*
	 * Every simple setting arrives here: baud, region, power, role and profile. `value`
	 * is the payload exactly as the host sent it — this layer checks its length and, for an
	 * enumerated value, that it is in range — and `cmd` says what it means. Decoding (the baud is
	 * little-endian), any further validation and everything it does to the device belong to the
	 * backend: that keeps all six in one table on the application side.
	 */
	int (*save_setting)(void *ctx, uint8_t cmd, const uint8_t *value, uint8_t len);
	/* Replace the device id with a fresh one (the pairing record goes with it) */
	int (*new_dev_id)(void *ctx);
	int (*pair)(void *ctx);
	int (*erase_pair)(void *ctx);
	int (*factory_reset)(void *ctx);
	int (*enter_recovery)(void *ctx);
	/* Host confirms this image: OK means it is recorded (the upgrade flow depends on it) */
	int (*confirm_image)(void *ctx);
	/* build string; E_STATE when unimplemented */
	int (*read_build)(void *ctx, char *out, size_t out_len);
	/* MCUboot hash of the running image (new build or rolled back one, the tool decides) */
	int (*read_image_hash)(void *ctx, uint8_t out[ZLRS_MGMT_IMAGE_HASH_LEN]);
	/*
	 * Log store over the management port: the device only serves bytes here, the host decides what to
	 * do with them. E_STATE when the board has no store to read.
	 */
	int (*log_info)(void *ctx, zlrs_mgmt_log_info_t *out);
	int (*log_read)(void *ctx, uint8_t index, uint32_t offset, uint8_t len, uint8_t *out,
			uint8_t *out_len);
	void *ctx;
} zlrs_mgmt_backend_t;

/* Byte-at-a-time management frame parser */
typedef struct {
	uint8_t buf[ZLRS_MGMT_FRAME_MAX_LEN];
	uint8_t len;
	uint8_t need;
	uint8_t cmd;
	uint8_t payload_len;
	uint32_t frames_ok;
	uint32_t frames_bad;
} zlrs_mgmt_parser_t;

#define ZLRS_MGMT_PAYLOAD(p) ((p)->buf + 5U)

void zlrs_mgmt_parser_init(zlrs_mgmt_parser_t *p);

/* 1 = a full frame is ready (cmd/payload readable), 0 = not yet, -1 = this byte is invalid */
int zlrs_mgmt_feed(zlrs_mgmt_parser_t *p, uint8_t byte);

/* Build a frame; returns its length, or 0 for invalid arguments */
uint8_t zlrs_mgmt_encode(uint8_t *out, size_t out_len, uint8_t cmd, const uint8_t *payload,
			 uint8_t payload_len);

/*
 * Handle one command and write the response frame into out (SOF and CRC included).
 * Returns the response length; 0 means this command needs no response.
 */
uint8_t zlrs_mgmt_dispatch(const zlrs_mgmt_backend_t *backend, uint8_t cmd, const uint8_t *payload,
			   uint8_t payload_len, uint8_t *out, size_t out_len);

/* status struct <-> fixed-size bytes */
void zlrs_mgmt_status_encode(uint8_t out[ZLRS_MGMT_STATUS_LEN], const zlrs_mgmt_status_t *status);
bool zlrs_mgmt_status_decode(const uint8_t *in, uint8_t len, zlrs_mgmt_status_t *status);

#ifdef __cplusplus
}
#endif
