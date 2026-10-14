/*
 * Management protocol: runs on the management CDC while in management mode
 * (table: doc/07-protocol.md#usb-management-framing). Pure C frame and fixed status codecs.
 * The application executes device commands.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zlrs/proto/limits.h"
#include "zlrs/region/region.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZLRS_MGMT_SOF0            0xA5U
#define ZLRS_MGMT_SOF1            0x5AU
#define ZLRS_MGMT_VERSION         4U
#define ZLRS_MGMT_FRAME_MAX_LEN   96U
#define ZLRS_MGMT_PAYLOAD_MAX_LEN (ZLRS_MGMT_FRAME_MAX_LEN - 6U)

/* host -> device */
#define ZLRS_MGMT_CMD_ENTER         0x00U /* enter management mode (the formal entry) */
#define ZLRS_MGMT_CMD_STATUS        0x01U /* read status */
#define ZLRS_MGMT_CMD_PAIR          0x02U /* start pairing */
#define ZLRS_MGMT_CMD_ERASE_PAIR    0x03U /* erase the pairing record */
#define ZLRS_MGMT_CMD_SET_KEY_HEX   0x04U /* root key as 64 hex characters */
#define ZLRS_MGMT_CMD_SET_PHRASE    0x05U /* passphrase, derived into a root key on the device */
#define ZLRS_MGMT_CMD_BUILD         0x06U /* read the build string (which version is running) */
#define ZLRS_MGMT_CMD_IMAGE_HASH    0x07U /* MCUboot hash of this image (the update verdict) */
#define ZLRS_MGMT_CMD_SET_BAUD      0x08U /* 4-byte little-endian baud rate */
#define ZLRS_MGMT_CMD_SET_REGION    0x09U /* 1-byte region id (0..3) */
#define ZLRS_MGMT_CMD_SET_POWER     0x0AU /* 1-byte signed dBm */
#define ZLRS_MGMT_CMD_SET_ROLE      0x0BU /* 1-byte role (0 = Leader, 1 = Follower) */
#define ZLRS_MGMT_CMD_SET_PROFILE   0x0CU /* 1-byte profile (0 = P0, 1 = P1, 2 = P2) */
#define ZLRS_MGMT_CMD_SET_BAND      0x0DU /* 0 = 2.4 GHz, 1 = sub-G, 2 = auto; next boot */
#define ZLRS_MGMT_CMD_NEW_DEV_ID    0x0EU /* make a fresh device id (drops the pairing record) */
#define ZLRS_MGMT_CMD_EXIT          0x0FU /* leave management mode */
#define ZLRS_MGMT_CMD_FACTORY_RESET 0x10U /* wipe NVS */
#define ZLRS_MGMT_CMD_RECOVERY      0x11U /* reboot into MCUboot serial recovery */
#define ZLRS_MGMT_CMD_CONFIRM       0x12U /* host confirms this upgrade (no confirm = rollback) */
#define ZLRS_MGMT_CMD_LOG_INFO      0x13U /* what the log store holds */
#define ZLRS_MGMT_CMD_LOG_READ      0x14U /* one chunk: log index (1) + offset (4) + length (1) */
#define ZLRS_MGMT_CMD_DEBUG_REPORT  0x15U /* debug image only: report id (1) + offset (2) */

/* device -> host */
#define ZLRS_MGMT_CMD_ACK               0x80U /* [cmd][status] */
#define ZLRS_MGMT_CMD_STATUS_RESP       0x81U /* see zlrs_mgmt_status_t */
#define ZLRS_MGMT_CMD_BUILD_RESP        0x82U /* build string, at most ZLRS_MGMT_BUILD_MAX_LEN */
#define ZLRS_MGMT_CMD_IMAGE_HASH_RESP   0x83U /* ZLRS_MGMT_IMAGE_HASH_LEN bytes of image hash */
#define ZLRS_MGMT_CMD_LOG_INFO_RESP     0x84U /* count, newest, then file number/size pairs */
#define ZLRS_MGMT_CMD_LOG_DATA_RESP     0x85U /* log index (1) + offset (4) + bytes */
#define ZLRS_MGMT_CMD_DEBUG_REPORT_RESP 0x86U /* report id + offset + total + text chunk */

#define ZLRS_MGMT_DEBUG_REPORT_LINK       0U
#define ZLRS_MGMT_DEBUG_REPORT_RATE       1U
#define ZLRS_MGMT_DEBUG_REPORT_RADIO      2U
#define ZLRS_MGMT_DEBUG_REPORT_POWER      3U
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

#define ZLRS_MGMT_STATUS_LEN       90U
#define ZLRS_MGMT_TEMP_UNAVAILABLE INT16_MIN
#define ZLRS_MGMT_PHRASE_MAX_LEN   32U
#define ZLRS_MGMT_BUILD_MAX_LEN    32U
#define ZLRS_MGMT_IMAGE_HASH_LEN   32U
/* Log store: at most this many files, and this many bytes back per read (one frame's payload) */
#define ZLRS_MGMT_LOG_CHUNK_MAX    80U

typedef struct {
	uint8_t version; /* protocol version */
	uint8_t role;    /* 0 = Leader, 1 = Follower */
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
	int16_t rssi_dbm;
	int16_t peer_rssi_dbm;
	uint8_t peer_lq_percent;
	int8_t active_power_dbm;
	int8_t power_floor_dbm;
	int16_t temperature_centi_c;
	uint32_t tx_frames;
	uint32_t rx_frames;
	uint32_t rx_crc_errors;
	uint32_t rx_bad_tag;
	uint32_t tx_retries;
	uint32_t data_drop_bytes;
	uint32_t missed_slots;
	uint32_t radio_fail;
	uint32_t frequency_hz;
	uint16_t hop_index;
	uint32_t frame_period_us;
	int16_t radio_temperature_centi_c;
	int16_t radio_xosc_temperature_centi_c;
	uint16_t radio_vbat_mv;
	uint8_t radio_version_major;
	uint8_t radio_version_minor;
	uint8_t radio_chip_mode;
	uint8_t radio_command_status;
	uint8_t radio_reset_status;
	/* bit0 junction temp, bit1 XOSC temp, bit2 VBAT, bit3 version,
	 * bit4 mode/command/reset/IRQ, bit5 errors; unset means unavailable. */
	uint8_t radio_valid;
	uint16_t radio_errors;
	uint32_t radio_irq;
} zlrs_mgmt_status_t;

#define ZLRS_MGMT_FLAG_KEY       0x01U
#define ZLRS_MGMT_FLAG_PAIRED    0x02U
#define ZLRS_MGMT_FLAG_SESSION   0x04U
#define ZLRS_MGMT_FLAG_DATA_SUBG 0x08U /* DATA currently uses sub-GHz */
#define ZLRS_MGMT_FLAG_BAND_AUTO 0x10U
#define ZLRS_MGMT_FLAG_BAND_ACQUIRING 0x20U
#define ZLRS_MGMT_FLAG_BAND_PLANNED 0x40U

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

/* status struct <-> fixed-size bytes */
void zlrs_mgmt_status_encode(uint8_t out[ZLRS_MGMT_STATUS_LEN], const zlrs_mgmt_status_t *status);
bool zlrs_mgmt_status_decode(const uint8_t *in, uint8_t len, zlrs_mgmt_status_t *status);

#ifdef __cplusplus
}
#endif
