/*
 * CRSF framing for handset/module and receiver/FC ports. Frames use the standard DVB-S2
 * polynomial 0xD5; command frames additionally use 0xBA. The radio link encapsulates complete
 * serial frames in its separate CRSF service. Static and dynamic device addresses are accepted;
 * the caller selects the outgoing serial sync byte.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard device addresses; 0xC8 also serves as the common serial sync byte. */
#define ZLRS_CRSF_ADDR_FC        0xC8U
#define ZLRS_CRSF_ADDR_HANDSET   0xEAU
#define ZLRS_CRSF_ADDR_TX_MODULE 0xEEU
#define ZLRS_CRSF_ADDR_RX        0xECU
#define ZLRS_CRSF_ADDR_BROADCAST 0x00U

/* frame types */
#define ZLRS_CRSF_TYPE_LINK_STATISTICS    0x14U
#define ZLRS_CRSF_TYPE_RC_CHANNELS        0x16U
#define ZLRS_CRSF_TYPE_LINK_STATISTICS_RX 0x1CU
#define ZLRS_CRSF_TYPE_LINK_STATISTICS_TX 0x1DU
#define ZLRS_CRSF_TYPE_DEVICE_PING        0x28U
#define ZLRS_CRSF_TYPE_DEVICE_INFO        0x29U
#define ZLRS_CRSF_TYPE_PARAMETER_ENTRY    0x2BU
#define ZLRS_CRSF_TYPE_PARAMETER_READ     0x2CU
#define ZLRS_CRSF_TYPE_PARAMETER_WRITE    0x2DU
#define ZLRS_CRSF_TYPE_COMMAND            0x32U
#define ZLRS_CRSF_TYPE_RADIO              0x3AU
#define ZLRS_CRSF_TYPE_MSP_REQUEST        0x7AU
#define ZLRS_CRSF_TYPE_MSP_RESPONSE       0x7BU

/* Frame length limit comes from CRSF: no more than 64 bytes in total */
#define ZLRS_CRSF_FRAME_MAX_LEN   64U
#define ZLRS_CRSF_PAYLOAD_MAX_LEN (ZLRS_CRSF_FRAME_MAX_LEN - 4U) /* addr + len + type + crc */

/* Channel values: 11 bit, 1000-2000 us maps to 172-1811 */
#define ZLRS_CRSF_CHANNEL_COUNT  16U
#define ZLRS_CRSF_CHANNEL_MIN    172U
#define ZLRS_CRSF_CHANNEL_MID    992U
#define ZLRS_CRSF_CHANNEL_MAX    1811U
#define ZLRS_CRSF_CHANNEL_MASK   0x07FFU
#define ZLRS_CRSF_RC_PAYLOAD_LEN 22U

#define ZLRS_CRSF_LINK_STATS_LEN 10U

/* parse results */
enum zlrs_crsf_result {
	ZLRS_CRSF_NONE = 0,  /* no complete frame yet */
	ZLRS_CRSF_FRAME,     /* a complete frame with a good CRC */
	ZLRS_CRSF_BAD_FRAME, /* bad shape (address, length), dropped */
	ZLRS_CRSF_BAD_CRC,   /* CRC mismatch, dropped */
};

/*
 * Byte-at-a-time parser. Once a frame is complete, type is in p->type, the payload in
 * ZLRS_CRSF_PAYLOAD(p) and its length in p->payload_len.
 */
typedef struct {
	uint8_t buf[ZLRS_CRSF_FRAME_MAX_LEN];
	uint8_t len;         /* bytes received so far */
	uint8_t need;        /* total length of this frame; 0 = still waiting for an address */
	uint8_t type;        /* type of the last frame */
	uint8_t payload_len; /* payload length of the last frame */
	uint32_t frames_ok;
	uint32_t frames_bad;
	uint32_t last_byte_ms;
} zlrs_crsf_parser_t;

#define ZLRS_CRSF_PAYLOAD(p) ((p)->buf + 3U)

void zlrs_crsf_parser_init(zlrs_crsf_parser_t *p);
enum zlrs_crsf_result zlrs_crsf_feed(zlrs_crsf_parser_t *p, uint8_t byte);
enum zlrs_crsf_result zlrs_crsf_feed_at(zlrs_crsf_parser_t *p, uint8_t byte, uint32_t now_ms);
bool zlrs_crsf_frame_valid(const uint8_t *frame, uint8_t len);
bool zlrs_crsf_extended(uint8_t type);
bool zlrs_crsf_command_valid(const uint8_t *payload, uint8_t len);
uint8_t zlrs_crsf_command_crc(const uint8_t *payload, uint8_t len);
uint8_t zlrs_crsf_rssi_percent(int16_t dbm, int16_t sensitivity_dbm);

/* CRC8, polynomial 0xD5; the check value for "123456789" is 0xBC */
uint8_t zlrs_crsf_crc8(const uint8_t *data, size_t len);

/* Build a frame; returns its length, or 0 for invalid arguments */
uint8_t zlrs_crsf_encode(uint8_t *out, uint8_t addr, uint8_t type, const uint8_t *payload,
			 uint8_t payload_len);

/* 16 channels <-> 22 bytes; pack only keeps the low 11 bits */
void zlrs_crsf_channels_pack(uint8_t out[ZLRS_CRSF_RC_PAYLOAD_LEN],
			     const uint16_t channels[ZLRS_CRSF_CHANNEL_COUNT]);
void zlrs_crsf_channels_unpack(const uint8_t in[ZLRS_CRSF_RC_PAYLOAD_LEN],
			       uint16_t channels[ZLRS_CRSF_CHANNEL_COUNT]);

/* RSSI: CRSF reports strength as the negated dBm, -70 dBm -> 70 */
uint8_t zlrs_crsf_rssi_from_dbm(int16_t dbm);

/* TX power index: 0=0mW 1=10 2=25 3=100 4=500 5=1000 6=2000 7=250 8=50 */
uint8_t zlrs_crsf_tx_power_index(int8_t dbm);

/* Closest nominal CRSF 0x14 profile (4/50/150 fps -> 0/1/2).
 * The detailed 0x1D FPS field reports the actual ZLRS rate in 10 fps units.
 */
uint8_t zlrs_crsf_rf_mode(uint32_t frames_per_second);

typedef struct {
	uint8_t uplink_rssi_ant1; /* negated dBm, see zlrs_crsf_rssi_from_dbm() */
	uint8_t uplink_rssi_ant2; /* single antenna here, 0 when absent */
	uint8_t uplink_lq;        /* 0–100 % */
	int8_t uplink_snr_db;
	uint8_t active_antenna;  /* 0 = antenna 1 */
	uint8_t rf_mode;         /* see zlrs_crsf_rf_mode() */
	uint8_t uplink_tx_power; /* see zlrs_crsf_tx_power_index() */
	uint8_t downlink_rssi;
	uint8_t downlink_lq;
	int8_t downlink_snr_db;
} zlrs_crsf_link_stats_t;

void zlrs_crsf_link_stats_encode(uint8_t out[ZLRS_CRSF_LINK_STATS_LEN],
				 const zlrs_crsf_link_stats_t *stats);
void zlrs_crsf_link_stats_decode(const uint8_t in[ZLRS_CRSF_LINK_STATS_LEN],
				 zlrs_crsf_link_stats_t *stats);

#ifdef __cplusplus
}
#endif
