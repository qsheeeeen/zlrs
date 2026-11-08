/*
 * Hopping sequence, see doc/07-protocol.md: both ends derive the same table from fhss_seed,
 * slot 0 of every block stays on the sync channel and the rest are shuffled within the block.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "proto/limits.h"
#include "region/region.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	const zlrs_region_t *region;
	uint8_t sequence[ZLRS_FHSS_SEQUENCE_LEN];
	uint16_t sequence_count;
	uint16_t index;
	uint8_t sync_channel;

} zlrs_fhss_t;

void zlrs_fhss_init(zlrs_fhss_t *fhss, zlrs_region_id_t region_id, uint32_t seed);

uint32_t zlrs_fhss_current_hz(const zlrs_fhss_t *fhss);
uint32_t zlrs_fhss_next_hz(zlrs_fhss_t *fhss);

void zlrs_fhss_set_index(zlrs_fhss_t *fhss, uint16_t index);

#ifdef __cplusplus
}
#endif
