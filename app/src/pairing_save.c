/* SPDX-License-Identifier: Apache-2.0 */
#include "pairing_save.h"

#include <string.h>

#define RETRY_MIN_MS 1000U
#define RETRY_MAX_MS 30000U

static bool same_pairing(const zlrs_persist_pairing_t *a, const zlrs_persist_pairing_t *b)
{
	return b != NULL && memcmp(a->peer_id, b->peer_id, sizeof(a->peer_id)) == 0 &&
	       memcmp(a->pair_salt, b->pair_salt, sizeof(a->pair_salt)) == 0;
}

bool zlrs_pairing_save_due(zlrs_pairing_save_t *state, const zlrs_persist_pairing_t *live,
			   const zlrs_persist_pairing_t *saved, uint32_t now_ms)
{
	if (live == NULL || same_pairing(live, saved)) {
		*state = (zlrs_pairing_save_t){0};
		return false;
	}
	if (!state->pending || !same_pairing(live, &state->candidate)) {
		*state = (zlrs_pairing_save_t){
			.candidate = *live,
			.retry_at_ms = now_ms,
			.delay_ms = RETRY_MIN_MS,
			.pending = true,
		};
	}
	return (int32_t)(now_ms - state->retry_at_ms) >= 0;
}

uint32_t zlrs_pairing_save_complete(zlrs_pairing_save_t *state, bool success, uint32_t now_ms)
{
	if (success || !state->pending) {
		*state = (zlrs_pairing_save_t){0};
		return 0U;
	}
	const uint32_t delay = state->delay_ms;
	state->retry_at_ms = now_ms + delay;
	state->delay_ms = delay * 2U < RETRY_MAX_MS ? delay * 2U : RETRY_MAX_MS;
	return delay;
}
