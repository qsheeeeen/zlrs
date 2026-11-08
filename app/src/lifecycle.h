/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "persist.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Owned by the application loop. Only the current live pairing can be committed. */
typedef struct {
	zlrs_pairing_t candidate;
	uint32_t retry_at_ms;
	uint32_t delay_ms;
} zlrs_pairing_save_t;

/* NULL means absent. Changed/removed credentials supersede any deferred retry. */
bool zlrs_pairing_save_due(zlrs_pairing_save_t *state, const zlrs_pairing_t *live,
			   const zlrs_pairing_t *saved, uint32_t now_ms);
/* Returns the delay until a failed write may be retried; success returns zero. */
uint32_t zlrs_pairing_save_complete(zlrs_pairing_save_t *state, bool success, uint32_t now_ms);

/* Reboot an unconfirmed image on startup fault or confirmation timeout. */
void zlrs_lifecycle_check_rollback(bool fault, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
