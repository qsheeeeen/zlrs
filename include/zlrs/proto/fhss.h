/*
 * Hopping sequence, see doc/07-protocol.md: both ends derive the same table from fhss_seed,
 * slot 0 of every block stays on the sync channel and the rest are shuffled within the block.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/proto/limits.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Region band: the region layer picks the channel plan and the power ceiling */
typedef struct {
	const char *name;
	uint32_t start_hz;     /* centre frequency of the first channel */
	uint32_t step_hz;      /* channel spacing */
	uint8_t channel_count; /* number of channels */
} zlrs_fhss_domain_t;

typedef enum {
	ZLRS_DOMAIN_FCC915 = 0,
	ZLRS_DOMAIN_EU868,
	ZLRS_DOMAIN_CN470,
	ZLRS_DOMAIN_CN433,
	ZLRS_DOMAIN_ISM2400,
	ZLRS_DOMAIN_COUNT
} zlrs_fhss_domain_id_t;

extern const zlrs_fhss_domain_t zlrs_fhss_domains[ZLRS_DOMAIN_COUNT];

typedef struct {
	const zlrs_fhss_domain_t *domain;
	uint8_t sequence[ZLRS_FHSS_SEQUENCE_LEN];
	uint16_t sequence_count;
	uint16_t index;
	uint8_t sync_channel;

} zlrs_fhss_t;

void zlrs_fhss_init(zlrs_fhss_t *fhss, zlrs_fhss_domain_id_t domain_id, uint32_t seed);

uint32_t zlrs_fhss_current_hz(const zlrs_fhss_t *fhss);
uint32_t zlrs_fhss_next_hz(zlrs_fhss_t *fhss);

void zlrs_fhss_set_index(zlrs_fhss_t *fhss, uint16_t index);

#ifdef __cplusplus
}
#endif
