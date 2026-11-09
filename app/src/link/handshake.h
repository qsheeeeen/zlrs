/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "link/link.h"
#include "persist.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The exact bytes the pairing KDF is fed with: both ids (sorted) then the salt (sorted) */
extern const uint8_t pairing_sync_word[ZLRS_SYNC_WORD_LEN];

bool link_handle_pair_frame(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms);

bool link_pair_advance(zlrs_link_t *link);

void link_pair_plan(const zlrs_link_t *link, zlrs_link_plan_t *plan);

/* Adopt the saved identity and key tree once on an initialized, unconfigured link.
 * Credential changes rebuild the link before restoring the complete saved view. */
void zlrs_link_restore(zlrs_link_t *link, const zlrs_persist_state_t *saved, uint32_t now_ms);

/* Start pairing; false leaves the current state intact (unconfigured or entropy unavailable).
 * True accepts an attempt, not a completed pairing; the previous saved record remains visible. */
bool zlrs_link_press(zlrs_link_t *link, uint32_t now_ms);

bool zlrs_link_cancel_pair(zlrs_link_t *link, uint32_t now_ms);

uint32_t zlrs_link_pair_fingerprint(const zlrs_link_t *link);

void link_roll_session(zlrs_link_t *link);

uint16_t link_build_handshake(zlrs_link_t *link, uint8_t type);

bool link_handle_schedule(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms);

bool link_handle_sess_resp(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
