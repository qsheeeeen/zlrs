/* SPDX-License-Identifier: Apache-2.0 */
#include "zlrs/proto/mod.h"

zlrs_mod_cfg_t zlrs_mod_flrc(uint32_t rate_bps)
{
	return (zlrs_mod_cfg_t){.rate_bps = rate_bps};
}

zlrs_mod_cfg_t zlrs_mod_lora(uint8_t sf)
{
	if (sf != 6U && sf != 7U) {
		return (zlrs_mod_cfg_t){0};
	}
	return (zlrs_mod_cfg_t){.rate_bps = sf == 6U ? 75000U : 43750U, .lora_sf = sf};
}

zlrs_mod_cfg_t zlrs_mod_phy(zlrs_phy_t phy)
{
	switch (phy) {
	case ZLRS_PHY_FLRC_2600:
		return zlrs_mod_flrc(2600000U);
	case ZLRS_PHY_FLRC_650:
		return zlrs_mod_flrc(650000U);
	case ZLRS_PHY_FLRC_325:
		return zlrs_mod_flrc(325000U);
	case ZLRS_PHY_LORA_SF6:
		return zlrs_mod_lora(6U);
	case ZLRS_PHY_LORA_SF7:
		return zlrs_mod_lora(7U);
	case ZLRS_PHY_FLRC_1300:
		return zlrs_mod_flrc(1300000U);
	default:
		return (zlrs_mod_cfg_t){0};
	}
}

zlrs_phy_t zlrs_mod_phy_id(const zlrs_mod_cfg_t *mod)
{
	for (unsigned int i = 0U; i < ZLRS_PHY_COUNT; i++) {
		const zlrs_mod_cfg_t candidate = zlrs_mod_phy((zlrs_phy_t)i);
		if (candidate.rate_bps == mod->rate_bps && candidate.lora_sf == mod->lora_sf) {
			return (zlrs_phy_t)i;
		}
	}
	return ZLRS_PHY_COUNT;
}

const char *zlrs_mod_phy_name(zlrs_phy_t phy)
{
	static const char *const names[] = {"FLRC 2.6 Mbps",   "FLRC 650 kbps",   "FLRC 325 kbps",
					    "LoRa SF6/BW1MHz", "LoRa SF7/BW1MHz", "FLRC 1.3 Mbps"};
	return (unsigned int)phy < ZLRS_PHY_COUNT ? names[phy] : "unknown";
}

uint32_t zlrs_mod_airtime_us(const zlrs_mod_cfg_t *mod, uint16_t frame_len)
{
	if (mod->lora_sf != 0U) {
		if ((mod->lora_sf != 6U && mod->lora_sf != 7U) ||
		    frame_len > ZLRS_LORA_FRAME_MAX_LEN) {
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

uint32_t zlrs_mod_airtime_ms(const zlrs_mod_cfg_t *mod, uint16_t frame_len)
{
	return (zlrs_mod_airtime_us(mod, frame_len) + 999U) / 1000U;
}
