/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "proto/fhss.h"

#include <string.h>

static uint32_t rng_next(uint32_t *state)
{
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static uint16_t build_sequence(uint32_t seed, uint8_t channel_count, uint8_t sync_channel,
			       uint8_t *sequence)
{
	uint16_t count;

	if (channel_count == 0U) {
		return 0U;
	}
	if (sync_channel >= channel_count) {
		sync_channel = (uint8_t)(channel_count / 2U);
	}

	count = (uint16_t)((ZLRS_FHSS_SEQUENCE_LEN / channel_count) * channel_count);
	uint32_t rng = seed != 0U ? seed : 0x12345678U;

	/*
	 * Slot 0 of every block holds the sync channel and the channel that would have been there gets
	 * channel 0 instead, so every frequency appears exactly as often in the table.
	 */
	for (uint16_t i = 0; i < count; i++) {
		const uint8_t channel = (uint8_t)(i % channel_count);

		if (channel == 0U) {
			sequence[i] = sync_channel;
		} else if (channel == sync_channel) {
			sequence[i] = 0U;
		} else {
			sequence[i] = channel;
		}
	}

	/* Shuffle inside the block, leaving position 0 alone */
	for (uint16_t i = 0; i < count; i++) {
		uint16_t base;
		uint8_t pick;
		uint8_t tmp;

		if ((i % channel_count) == 0U) {
			continue;
		}

		base = (uint16_t)((i / channel_count) * channel_count);
		pick = (uint8_t)(1U + (rng_next(&rng) % (channel_count - 1U)));
		tmp = sequence[i];
		sequence[i] = sequence[base + pick];
		sequence[base + pick] = tmp;
	}

	return count;
}

void zlrs_fhss_init(zlrs_fhss_t *fhss, zlrs_region_id_t region_id, uint32_t seed)
{
	if ((unsigned int)region_id >= ZLRS_REGION_COUNT) {
		region_id = ZLRS_REGION_ISM2400;
	}

	memset(fhss, 0, sizeof(*fhss));
	fhss->region = &zlrs_regions[region_id];
	fhss->sync_channel = (uint8_t)(fhss->region->channel_count / 2U);
	fhss->index = 0U;
	fhss->sequence_count = build_sequence(seed, fhss->region->channel_count, fhss->sync_channel,
					      fhss->sequence);
}

uint32_t zlrs_fhss_current_hz(const zlrs_fhss_t *fhss)
{
	uint8_t channel = fhss->sequence[fhss->index];
	if (fhss->region == NULL) {
		return 0U; /* not initialised with a key yet */
	}

	if (channel >= fhss->region->channel_count) {
		channel = fhss->sync_channel;
	}

	return fhss->region->start_hz + (uint32_t)channel * fhss->region->step_hz;
}

uint32_t zlrs_fhss_next_hz(zlrs_fhss_t *fhss)
{
	if (fhss->sequence_count == 0U) {
		return fhss->region->start_hz;
	}

	fhss->index = (uint16_t)((fhss->index + 1U) % fhss->sequence_count);

	return zlrs_fhss_current_hz(fhss);
}

void zlrs_fhss_set_index(zlrs_fhss_t *fhss, uint16_t index)
{
	if (fhss->sequence_count > 0U) {
		fhss->index = (uint16_t)(index % fhss->sequence_count);
	}
}
