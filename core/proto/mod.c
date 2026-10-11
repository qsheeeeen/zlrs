/* SPDX-License-Identifier: Apache-2.0 */
#include "zlrs/proto/mod.h"

zlrs_mod_cfg_t zlrs_mod_flrc(uint32_t rate_bps)
{
	return (zlrs_mod_cfg_t){.rate_bps = rate_bps};
}

uint32_t zlrs_mod_airtime_us(const zlrs_mod_cfg_t *mod, uint16_t frame_len)
{
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
