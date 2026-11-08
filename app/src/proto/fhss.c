/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "proto/fhss.h"

#include <string.h>

void zlrs_fhss_init(zlrs_fhss_t *fhss, zlrs_region_id_t region_id, uint32_t seed)
{
	if ((unsigned int)region_id >= ZLRS_REGION_COUNT) {
		region_id = ZLRS_REGION_ISM2400;
	}
	memset(fhss, 0, sizeof(*fhss));
	fhss->region = &zlrs_regions[region_id];
	const uint8_t channels = fhss->region->channel_count;
	if (channels == 0U) {
		return;
	}
	fhss->sync_channel = channels / 2U;
	fhss->sequence_count = (ZLRS_FHSS_SEQUENCE_LEN / channels) * channels;
	uint32_t rng = seed != 0U ? seed : 0x12345678U;
	/* Each block contains every channel once, with its synchronization entry fixed.
	 * Keep the same RNG draws and swaps while building and shuffling each block. */
	for (uint16_t base = 0; base < fhss->sequence_count; base += channels) {
		fhss->sequence[base] = fhss->sync_channel;
		for (uint8_t channel = 1; channel < channels; channel++) {
			fhss->sequence[base + channel] =
				channel == fhss->sync_channel ? 0U : channel;
		}
		for (uint8_t channel = 1; channel < channels; channel++) {
			rng ^= rng << 13;
			rng ^= rng >> 17;
			rng ^= rng << 5;
			const uint8_t pick = 1U + rng % (channels - 1U);
			const uint8_t temp = fhss->sequence[base + channel];
			fhss->sequence[base + channel] = fhss->sequence[base + pick];
			fhss->sequence[base + pick] = temp;
		}
	}
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

void zlrs_fhss_set_index(zlrs_fhss_t *fhss, uint16_t index)
{
	if (fhss->sequence_count > 0U) {
		fhss->index = (uint16_t)(index % fhss->sequence_count);
	}
}
