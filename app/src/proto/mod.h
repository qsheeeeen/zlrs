/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ordered automatic 2.4 GHz profiles; fixed 1.3 Mbps is outside the ladder. */
typedef enum {
	ZLRS_PHY_FLRC_2600,
	ZLRS_PHY_FLRC_650,
	ZLRS_PHY_FLRC_325,
	ZLRS_PHY_LORA_SF6,
	ZLRS_PHY_LORA_SF7,
	ZLRS_PHY_FLRC_1300,
	ZLRS_PHY_COUNT,
} zlrs_phy_t;

#define ZLRS_PHY_AUTO_COUNT        5U
#define ZLRS_LORA_FRAME_MAX_LEN    255U
#define ZLRS_LORA_PREAMBLE_SYMBOLS 12U

/* LoRa uses BW 1 MHz, CR 4/5, explicit header and physical CRC. sf=0 is FLRC. */
typedef struct {
	uint32_t rate_bps;
	uint8_t lora_sf;
	uint16_t sync_period_ms;
	int16_t rssi_floor_dbm;
	const char *name;
} zlrs_phy_spec_t;

extern const zlrs_phy_spec_t zlrs_phys[ZLRS_PHY_COUNT];
zlrs_phy_t zlrs_phy_for_rate(uint32_t rate_bps);
const char *zlrs_phy_name(zlrs_phy_t phy);
/* Portable FLRC estimate; exact airtime for the shipped LoRa profiles. Invalid = zero. */
uint32_t zlrs_phy_airtime_us(zlrs_phy_t phy, uint16_t frame_len);

#ifdef __cplusplus
}
#endif
