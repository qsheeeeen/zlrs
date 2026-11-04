/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include "zlrs/proto/limits.h"
/* host -> device */
#define ZLRS_MGMT_CMD_ENTER         0x00U /* enter management mode (the formal entry) */
#define ZLRS_MGMT_CMD_STATUS        0x01U /* read status */
#define ZLRS_MGMT_CMD_PAIR          0x02U /* start pairing */
#define ZLRS_MGMT_CMD_ERASE_PAIR    0x03U /* erase the pairing record */
#define ZLRS_MGMT_CMD_SET_KEY_HEX   0x04U /* root key as 64 hex characters */
#define ZLRS_MGMT_CMD_SET_PHRASE    0x05U /* passphrase, derived into a root key on the device */
#define ZLRS_MGMT_CMD_SET_BAUD      0x08U /* 4-byte little-endian baud rate */
#define ZLRS_MGMT_CMD_SET_REGION    0x09U /* 1-byte region id (0..3) */
#define ZLRS_MGMT_CMD_SET_ROLE      0x0AU /* 1-byte role (0 = Leader, 1 = Follower) */
#define ZLRS_MGMT_CMD_SET_PROFILE   0x0BU /* 1-byte profile (0 = P0, 1 = P1, 2 = P2) */
#define ZLRS_MGMT_CMD_SET_BAND      0x0CU /* 0 = 2.4 GHz, 1 = sub-G, 2 = auto; next boot */
#define ZLRS_MGMT_CMD_NEW_DEV_ID    0x0DU /* make a fresh device id (drops the pairing record) */
#define ZLRS_MGMT_CMD_EXIT          0x0EU /* leave management mode */
#define ZLRS_MGMT_CMD_FACTORY_RESET 0x0FU /* wipe NVS */
#define ZLRS_MGMT_CMD_LOG_INFO      0x12U /* what the log store holds */
#define ZLRS_MGMT_CMD_DEBUG_REPORT  0x14U /* debug image only: one report id byte */
#define ZLRS_MGMT_CMD_OBSERVE       0x15U /* timestamped status without pausing RF */

typedef struct {
	uint8_t role;  /* 0 = Leader, 1 = Follower */
	uint8_t state; /* ZLRS_STATE_* */
	uint8_t flags; /* bit0 root key set, bit1 paired, bit2 session valid */
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	uint8_t lq_percent;
	uint8_t band;        /* local sub-G hardware: 0 = LF (470/433), 1 = HF (868/915) */
	uint8_t region;      /* current region id (0..3) */
	uint8_t profile;     /* active security profile (ZLRS_PROFILE_*) */
	int8_t tx_power_dbm; /* active-band maximum allowed TX power */
	uint32_t data_baud;  /* data port baud rate (effective on the next boot) */
	int16_t rssi_dbm;
	int16_t peer_rssi_dbm;
	uint8_t peer_lq_percent;
	uint32_t lq_generation;
	uint32_t peer_lq_generation;
	uint32_t power_raises;
	uint32_t power_lowers;
	uint32_t power_fallbacks;
	uint32_t power_changed_ms;
	uint32_t peer_feedback_ms;
	int16_t power_rssi_dbm;
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
	uint8_t data_phy;
} zlrs_mgmt_status_t;

#define ZLRS_MGMT_FLAG_POWER_FIXED    0x80U /* volatile debug override */
#define ZLRS_MGMT_FLAG_KEY            0x01U
#define ZLRS_MGMT_FLAG_PAIRED         0x02U
#define ZLRS_MGMT_FLAG_SESSION        0x04U
#define ZLRS_MGMT_FLAG_DATA_SUBG      0x08U /* DATA currently uses sub-GHz */
#define ZLRS_MGMT_FLAG_BAND_AUTO      0x10U
#define ZLRS_MGMT_FLAG_BAND_ACQUIRING 0x20U
#define ZLRS_MGMT_FLAG_BAND_PLANNED   0x40U
