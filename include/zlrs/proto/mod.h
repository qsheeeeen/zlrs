/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FLRC raw rate. Hardware packet overhead is accounted by the LR2021 adapter. */
typedef struct {
	uint32_t rate_bps;
} zlrs_mod_cfg_t;

zlrs_mod_cfg_t zlrs_mod_flrc(uint32_t rate_bps);
/* Payload plus four bytes of overhead, microseconds rounded up; zero rate returns zero. */
uint32_t zlrs_mod_airtime_us(const zlrs_mod_cfg_t *mod, uint16_t frame_len);
uint32_t zlrs_mod_airtime_ms(const zlrs_mod_cfg_t *mod, uint16_t frame_len);

#ifdef __cplusplus
}
#endif
