/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/proto/fhss.h"

#include <string.h>

/*
 * Channel plans follow the available bandwidth and the regulatory channel counts per region:
 *   - FCC 15.247 wants at least 50 channels for a hopping system
 *   - EU868 lives under a duty cap, so few channels with wide spacing
 *   - 2.4G ISM fills the band at 1 MHz spacing
 */
const zlrs_fhss_domain_t zlrs_fhss_domains[ZLRS_DOMAIN_COUNT] = {
	[ZLRS_DOMAIN_FCC915] = {.name = "FCC915",
				.start_hz = 902500000U,
				.step_hz = 500000U,
				.channel_count = 50U},
	[ZLRS_DOMAIN_EU868] = {.name = "EU868",
			       .start_hz = 863250000U,
			       .step_hz = 500000U,
			       .channel_count = 13U},
	/*
	 * 470-510 MHz: the main band of the Core2021 LF variant. A 300 kHz step matches the FLRC
	 * bandwidth (0.325 Mbps DSB is 300 kHz wide); the 200 kHz plan belongs to LoRa and would
	 * squeeze neighbouring channels if used for hopping.
	 */
	[ZLRS_DOMAIN_CN470] = {.name = "CN470",
			       .start_hz = 470300000U,
			       .step_hz = 300000U,
			       .channel_count = 96U},
	/*
	 * 433.05-434.79 MHz is the Chinese 433 ISM band, only 1.74 MHz wide: it fits five channels at
	 * 300 kHz spacing, so this region can only use the narrow-band rate tiers.
	 */
	[ZLRS_DOMAIN_CN433] = {.name = "CN433",
			       .start_hz = 433300000U,
			       .step_hz = 300000U,
			       .channel_count = 5U},
	[ZLRS_DOMAIN_ISM2400] = {.name = "ISM2400",
				 .start_hz = 2402000000U,
				 .step_hz = 1000000U,
				 .channel_count = 79U},
};

static uint32_t rng_state = 0x12345678U;

static void rng_seed(uint32_t seed)
{
	/* Zero is a fixed point of xorshift32, so avoid it */
	rng_state = (seed != 0U) ? seed : 0x12345678U;
}

static uint32_t rng_next(void)
{
	uint32_t x = rng_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;

	return x;
}

static uint8_t rng_n(uint8_t max)
{
	return (max == 0U) ? 0U : (uint8_t)(rng_next() % max);
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
	rng_seed(seed);

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
		pick = (uint8_t)(1U + rng_n((uint8_t)(channel_count - 1U)));
		tmp = sequence[i];
		sequence[i] = sequence[base + pick];
		sequence[base + pick] = tmp;
	}

	return count;
}

void zlrs_fhss_init(zlrs_fhss_t *fhss, uint8_t domain_id, uint32_t seed)
{
	if (domain_id >= ZLRS_DOMAIN_COUNT) {
		domain_id = ZLRS_DOMAIN_ISM2400;
	}

	memset(fhss, 0, sizeof(*fhss));
	fhss->domain = &zlrs_fhss_domains[domain_id];
	fhss->sync_channel = (uint8_t)(fhss->domain->channel_count / 2U);
	fhss->index = 0U;
	fhss->sequence_count = build_sequence(seed, fhss->domain->channel_count, fhss->sync_channel,
					      fhss->sequence);
}

void zlrs_fhss_init_dual(zlrs_fhss_t *fhss, uint8_t main_domain_id, uint8_t sub_domain_id,
			 uint32_t seed)
{
	zlrs_fhss_init(fhss, main_domain_id, seed);

	if (sub_domain_id >= ZLRS_DOMAIN_COUNT) {
		return;
	}

	fhss->sub_domain = &zlrs_fhss_domains[sub_domain_id];
	fhss->sub_sync_channel = (uint8_t)(fhss->sub_domain->channel_count / 2U);
	fhss->sub_index = 0U;
	/* The sub-table seed differs from the main one: one pair must not hop twice on the same table */
	fhss->sub_count = build_sequence(seed ^ 0x9E3779B9U, fhss->sub_domain->channel_count,
					 fhss->sub_sync_channel, fhss->sub_sequence);
}

uint8_t zlrs_fhss_mix_slots(uint8_t mix)
{
	static const uint8_t slots[4] = {0U, 1U, 2U, 4U};

	return slots[mix & 0x03U];
}

bool zlrs_fhss_sub_ready(const zlrs_fhss_t *fhss)
{
	return fhss->sub_domain != NULL && fhss->sub_count > 0U;
}

uint32_t zlrs_fhss_sub_current_hz(const zlrs_fhss_t *fhss)
{
	if (!zlrs_fhss_sub_ready(fhss)) {
		return 0U;
	}

	return fhss->sub_domain->start_hz +
	       (uint32_t)fhss->sub_sequence[fhss->sub_index] * fhss->sub_domain->step_hz;
}

uint32_t zlrs_fhss_sub_next_hz(zlrs_fhss_t *fhss)
{
	if (!zlrs_fhss_sub_ready(fhss)) {
		return 0U;
	}

	fhss->sub_index = (uint16_t)((fhss->sub_index + 1U) % fhss->sub_count);

	return zlrs_fhss_sub_current_hz(fhss);
}

void zlrs_fhss_set_sub_index(zlrs_fhss_t *fhss, uint16_t index)
{
	if (!zlrs_fhss_sub_ready(fhss)) {
		return;
	}

	fhss->sub_index = (uint16_t)(index % fhss->sub_count);
}

static uint32_t freq_hz(const zlrs_fhss_t *fhss, uint8_t channel)
{
	if (fhss->domain == NULL) {
		return 0U; /* not initialised with a key yet */
	}

	if (channel >= fhss->domain->channel_count) {
		channel = fhss->sync_channel;
	}

	return fhss->domain->start_hz + (uint32_t)channel * fhss->domain->step_hz;
}

uint32_t zlrs_fhss_current_hz(const zlrs_fhss_t *fhss)
{
	return freq_hz(fhss, fhss->sequence[fhss->index]);
}

uint32_t zlrs_fhss_next_hz(zlrs_fhss_t *fhss)
{
	if (fhss->sequence_count == 0U) {
		return fhss->domain->start_hz;
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
