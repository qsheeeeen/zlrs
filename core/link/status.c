/*
 * Link observations: keep consumers independent of mutable protocol storage.
 * All reads run on the link owner; cross-thread consumers receive copied application messages.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zlrs/link/link.h"

#include <string.h>

const zlrs_link_cfg_t *zlrs_link_get_config(const zlrs_link_t *link)
{
	return &link->cfg;
}

const zlrs_link_stats_t *zlrs_link_get_stats(const zlrs_link_t *link)
{
	return &link->stats;
}

zlrs_link_state_t zlrs_link_get_state(const zlrs_link_t *link)
{
	return link->state;
}

bool zlrs_link_is_busy(const zlrs_link_t *link)
{
	return link->req_pending;
}

zlrs_link_info_t zlrs_link_get_info(const zlrs_link_t *link)
{
	zlrs_link_info_t info = {
		.state = link->state,
		.configured = link->configured,
		.paired = link->paired,
		.session_valid = link->session_valid,
		.peer_power_dbm = link->peer_power_dbm,
		.last_rc_ms = link->last_rc_ms,
	};

	memcpy(info.dev_id, link->dev_id, sizeof(info.dev_id));
	memcpy(info.peer_id, link->peer_id, sizeof(info.peer_id));
	return info;
}

bool zlrs_link_get_pairing(const zlrs_link_t *link, uint8_t peer_id[ZLRS_DEV_ID_LEN],
			   uint8_t pair_salt[ZLRS_PAIR_SALT_LEN])
{
	if (!link->paired) {
		return false;
	}
	memcpy(peer_id, link->peer_id, ZLRS_DEV_ID_LEN);
	memcpy(pair_salt, link->pair_salt, ZLRS_PAIR_SALT_LEN);
	return true;
}

static uint32_t fingerprint(const uint8_t *data, size_t len)
{
	uint32_t hash = 2166136261U;

	for (size_t i = 0U; i < len; i++) {
		hash = (hash ^ data[i]) * 16777619U;
	}
	return hash;
}

zlrs_link_diagnostics_t zlrs_link_get_diagnostics(const zlrs_link_t *link)
{
	return (zlrs_link_diagnostics_t){
		.frequency_hz = link->fhss.domain != NULL ? zlrs_fhss_current_hz(&link->fhss) : 0U,
		.frame_index = link->frame_index,
		.hop_index = link->fhss.index,
		.queue_count = link->queue_count,
		.arq_used = link->arq_used,
		.arq_seq = link->arq_seq,
		.arq_rx_seq = link->arq_rx_seq,
		.arq_ack_pending = link->arq_ack_pending,
		.pair_wait_cfm = link->pair_wait_cfm,
		.pair_cfm_pending = link->pair_cfm_pending,
		.pair_cfm_sent = link->pair_cfm_sent,
		.pair_nonce = link->pair_nonce,
		.pair_nonce_b = link->pair_nonce_b,
		.root_fingerprint = fingerprint(link->root_key, sizeof(link->root_key)),
		.pair_fingerprint = fingerprint(link->pair_key, sizeof(link->pair_key)),
		.sync_fingerprint = fingerprint(link->sync_word, sizeof(link->sync_word)),
	};
}
