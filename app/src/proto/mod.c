/* SPDX-License-Identifier: Apache-2.0 */
#include "proto/mod.h"

/* One definition per supported PHY: rate, SF, SYNC cadence, receive floor and display name. */
const zlrs_phy_spec_t zlrs_phys[ZLRS_PHY_COUNT] = {
	[ZLRS_PHY_FLRC_2600] = {2600000U, 0, 100, -89, "FLRC 2.6 Mbps"},
	[ZLRS_PHY_FLRC_650] = {650000U, 0, 300, -94, "FLRC 650 kbps"},
	[ZLRS_PHY_FLRC_325] = {325000U, 0, 600, -98, "FLRC 325 kbps"},
	[ZLRS_PHY_LORA_SF6] = {75000U, 6, 300, -103, "LoRa SF6/BW1MHz"},
	[ZLRS_PHY_LORA_SF7] = {43750U, 7, 300, -106, "LoRa SF7/BW1MHz"},
	[ZLRS_PHY_FLRC_1300] = {1300000U, 0, 150, -89, "FLRC 1.3 Mbps"},
};

zlrs_phy_t zlrs_phy_for_rate(uint32_t rate_bps)
{
	for (unsigned int phy = 0; phy < ZLRS_PHY_COUNT; phy++) {
		if (zlrs_phys[phy].lora_sf == 0 && zlrs_phys[phy].rate_bps == rate_bps) {
			return (zlrs_phy_t)phy;
		}
	}
	return ZLRS_PHY_COUNT;
}

const char *zlrs_phy_name(zlrs_phy_t phy)
{
	return (unsigned int)phy < ZLRS_PHY_COUNT ? zlrs_phys[phy].name : "unknown";
}

uint32_t zlrs_phy_airtime_us(zlrs_phy_t phy, uint16_t frame_len)
{
	if ((unsigned int)phy >= ZLRS_PHY_COUNT) {
		return 0U;
	}
	const zlrs_phy_spec_t *mod = &zlrs_phys[phy];
	if (mod->lora_sf != 0U) {
		if (frame_len > ZLRS_LORA_FRAME_MAX_LEN) {
			return 0U;
		}
		/* LR20xx explicit-header, CRC-on, CR4/5 numerator at exactly 1 MHz.
		 * SF6 adds two fine-synchronization symbols; no low-rate optimization. */
		const uint32_t sf = mod->lora_sf;
		const uint32_t fine = sf <= 6U ? 1U : 0U;
		const uint32_t header_bits = 4U * sf + 8U * fine - 28U;
		const uint32_t bits = 8U * (frame_len + 2U);
		const uint32_t payload_bits = bits > header_bits ? bits - header_bits : 0U;
		const uint32_t blocks = (payload_bits + 4U * sf - 1U) / (4U * sf);
		const uint32_t symbols =
			ZLRS_LORA_PREAMBLE_SYMBOLS + 4U + 2U * fine + 8U + 5U * blocks;
		return (4U * symbols + 1U) * (1U << (sf - 2U)) - 1U;
	}
	if (mod->rate_bps == 0U) {
		return 0U;
	}
	const uint64_t bits = ((uint64_t)frame_len + 4U) * 8U;
	return (uint32_t)((bits * 1000000U + mod->rate_bps - 1U) / mod->rate_bps);
}
