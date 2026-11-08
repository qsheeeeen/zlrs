/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "link/link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The exact bytes the pairing KDF is fed with: both ids (sorted) then the salt (sorted) */
#define ZLRS_PAIR_CTX_LEN (2U * ZLRS_DEV_ID_LEN + ZLRS_PAIR_SALT_LEN)

const zlrs_radio_req_t *link_pair_finalize(zlrs_link_t *link, uint8_t type);

bool link_handle_pair_frame(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms);

bool link_pair_advance(zlrs_link_t *link);

void link_pair_plan(const zlrs_link_t *link, zlrs_link_plan_t *plan);

/* The link only joins the air once a root key has been written */
void zlrs_link_set_root_key(zlrs_link_t *link, const uint8_t root_key[ZLRS_KEY_LEN],
			    uint32_t now_ms);

void zlrs_link_set_dev_id(zlrs_link_t *link, const uint8_t dev_id[ZLRS_DEV_ID_LEN]);

void zlrs_link_pair_ctx(const zlrs_link_t *link, uint8_t out[ZLRS_PAIR_CTX_LEN]);

/*
 * Restore pairing from the stored record (no re-pairing after a reboot). pair_salt has to
 * match the peer's copy: the two exchanged nonces. Keys are recomputed, never stored.
 */
void zlrs_link_set_peer(zlrs_link_t *link, const uint8_t peer_id[ZLRS_DEV_ID_LEN],
			const uint8_t pair_salt[ZLRS_PAIR_SALT_LEN], uint32_t now_ms);

/* Start pairing; false leaves the current state intact (unconfigured or entropy unavailable).
 * True accepts an attempt, not a completed pairing; the previous saved record remains visible. */
bool zlrs_link_press(zlrs_link_t *link, uint32_t now_ms);

bool zlrs_link_cancel_pair(zlrs_link_t *link, uint32_t now_ms);

/* Only the last confirmed record is visible, including while re-pairing. */
bool zlrs_link_get_pairing(const zlrs_link_t *link, uint8_t peer_id[ZLRS_DEV_ID_LEN],
			   uint8_t pair_salt[ZLRS_PAIR_SALT_LEN]);

#ifdef __cplusplus
}
#endif
