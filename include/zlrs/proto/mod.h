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
} zlrs_mod_cfg_t;

zlrs_mod_cfg_t zlrs_mod_flrc(uint32_t rate_bps);
zlrs_mod_cfg_t zlrs_mod_lora(uint8_t sf);
zlrs_mod_cfg_t zlrs_mod_phy(zlrs_phy_t phy);
zlrs_phy_t zlrs_mod_phy_id(const zlrs_mod_cfg_t *mod);
const char *zlrs_mod_phy_name(zlrs_phy_t phy);
/* Portable FLRC estimate; exact airtime for the shipped LoRa profiles. Invalid = zero. */
uint32_t zlrs_mod_airtime_us(const zlrs_mod_cfg_t *mod, uint16_t frame_len);
uint32_t zlrs_mod_airtime_ms(const zlrs_mod_cfg_t *mod, uint16_t frame_len);

#ifdef __cplusplus
}
#endif
