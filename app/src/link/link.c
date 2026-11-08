/* SPDX-License-Identifier: Apache-2.0 */
#include "link/link.h"
#include "link/data.h"
#include "link/handshake.h"
#include <string.h>
#include <zephyr/sys/byteorder.h>

/* Link lifecycle, slot scheduling, frame authentication and radio completion. */

bool link_new_nonce(const zlrs_link_t *link, uint32_t previous, bool *have_previous, uint32_t *out)
{
	if (link->random == NULL) {
		return false;
	}
	for (unsigned int i = 0U; i < 4U; i++) {
		uint32_t nonce;
		if (!link->random(link->random_ctx, &nonce)) {
			return false;
		}
		if (!*have_previous || nonce != previous) {
			*have_previous = true;
			*out = nonce;
			return true;
		}
	}
	return false;
}

uint32_t link_sync_freq_hz(const zlrs_link_t *link)
{
	const zlrs_region_t *region = &zlrs_regions[link->status.cfg.region];
	const uint8_t channel = (uint8_t)(region->channel_count / 2U);

	return region->start_hz + (uint32_t)channel * region->step_hz;
}

static bool sync_frame(const zlrs_link_t *link)
{
	return link->status.cfg.resync_every_n > 0U &&
	       (link->status.frame_index % link->status.cfg.resync_every_n) == 0U;
}

uint32_t link_slot_freq_hz(const zlrs_link_t *link)
{
	if (link->status.radio_acquiring || link->band_announced || sync_frame(link) ||
	    link->status.sess_cfm_due || link->sess_ack_due) {
		return link_sync_freq_hz(link);
	}
	/* Both ends count from the confirmed frame, including repeated confirms after lost ACKs.
	 * Keep initial DATA on the same channel where the peer is waiting for confirmation. */
	if (link->status.session_valid &&
	    (uint32_t)(link->status.frame_index - link->sess_sync_frame) <
		    ZLRS_DATA_CONFIRM_FRAMES) {
		return link_sync_freq_hz(link);
	}
	/* Both ends keep fresh-session slot B on the confirmation channel. */
	if ((link->slot == ZLRS_SLOT_DATA_B) &&
	    (uint32_t)(link->slot_ms - link->state_ms) < ZLRS_SESS_CONFIRM_MS) {
		return link_sync_freq_hz(link);
	}

	return zlrs_fhss_current_hz(&link->status.fhss);
}

void link_set_state(zlrs_link_t *link, zlrs_link_state_t state, uint32_t now_ms)
{
	if (link->status.state == state) {
		return;
	}

	link->status.state = state;
	link->state_ms = now_ms;
	if (link->ev.on_state != NULL) {
		link->ev.on_state(link->ev.ctx, state);
	}

	/* Leader is the authority: every return to SEARCHING rolls a new session nonce. */
	if (state == ZLRS_STATE_SEARCHING && link->status.cfg.role == ZLRS_ROLE_LEADER) {
		link_roll_session(link);
		/* Send a SYNC right away instead of waiting a full period, to shorten the reconnect
		 */
		link->last_sync_ms = (uint32_t)(now_ms - link->status.cfg.sync_period_ms);
	}
}

uint16_t link_build_frame(zlrs_link_t *link, zlrs_frame_t *frame, uint8_t *buf, uint16_t data_len)
{
	const uint16_t payload_len = zlrs_packet_layouts[frame->type].payload_len + data_len;
	uint32_t *const counter =
		frame->type >= ZLRS_PKT_PAIR_REQ && frame->type <= ZLRS_PKT_PAIR_CFM
			? &link->pair_tx_seq
			: &link->tx_seq;
	if (*counter == 0U || *counter == UINT32_MAX) {
		return 0U;
	}
	frame->seq = *counter;
	if (link->status.cfg.profile > ZLRS_PROFILE_P1 || data_len > link->status.cfg.data_mtu ||
	    payload_len > ZLRS_PAYLOAD_MAX_LEN - zlrs_profile_tag_len(link->status.cfg.profile)) {
		return 0U;
	}
	const uint8_t tag_len = zlrs_profile_tag_len(link->status.cfg.profile);
	const uint16_t len = zlrs_frame_len(payload_len, tag_len);
	const uint16_t total = zlrs_frame_encode(
		frame, buf, data_len ? ZLRS_FRAME_MAX_LEN : sizeof(link->tx_buf), len, tag_len);
	if (total != 0U && tag_len != 0U) {
		const uint16_t mac_len = total - tag_len;
		zlrs_tag(link_tag_key_for(link, frame->type, true), ZLRS_KEY_LEN, buf, mac_len,
			 buf + mac_len);
	}
	if (total > 0U) {
		(*counter)++;
		link->status.stats.tx_frame_builds++;
		link->status.stats.tx_auth_tags +=
			zlrs_profile_tag_len(link->status.cfg.profile) != 0U;
	}
	return total;
}

const uint8_t *link_tag_key_for(const zlrs_link_t *link, uint8_t type, bool outbound)
{
	const uint8_t *link_auth = outbound ? link->link_auth_out : link->link_auth_in;
	const uint8_t *session = outbound ? link->session_key_out : link->session_key_in;

	switch (zlrs_packet_layouts[type].auth) {
	case ZLRS_AUTH_ROOT:
		return link->auth_key;
	case ZLRS_AUTH_LINK:
		return link_auth;
	case ZLRS_AUTH_SESSION:
		return session;
	case ZLRS_AUTH_DATA:
		return link->status.session_valid ? session : link_auth;
	}
	return NULL;
}

uint16_t link_acq_timeout_ms(const zlrs_link_t *link)
{
	return (link->status.cfg.acq_timeout_ms != 0U) ? link->status.cfg.acq_timeout_ms
						       : link->status.cfg.rx_timeout_ms;
}

bool link_verify_frame(zlrs_link_t *link, const zlrs_frame_t *frame, uint16_t len)
{
	const uint8_t tag_len = zlrs_profile_tag_len(link->status.cfg.profile);
	if (tag_len == 0U) {
		return true;
	}
	uint8_t expected[ZLRS_TAG_LEN];
	zlrs_tag(link_tag_key_for(link, frame->type, false), ZLRS_KEY_LEN, link->rx_buf,
		 len - tag_len, expected);
	const bool valid = zlrs_ct_equal(expected, frame->tag, ZLRS_TAG_LEN);
	link->status.stats.rx_bad_tag += !valid;
	return valid;
}

static bool handle_rx(zlrs_link_t *link, uint16_t len, uint32_t now_ms)
{
	const uint8_t tag_len = zlrs_profile_tag_len(link->status.cfg.profile);
	zlrs_frame_t frame;

	if (!zlrs_frame_decode(&frame, link->rx_buf, len, tag_len)) {
		link->status.stats.rx_bad_frame++;
		return false;
	}

	/* The confirm verifies with the candidate key first, not the state-based path below */
	if (frame.type == ZLRS_PKT_SESS_CFM) {
		return link_handle_sess_cfm(link, &frame, len, now_ms);
	}

	if (!link_verify_frame(link, &frame, len)) {
		return false;
	}

	link->status.stats.rx_frames++;
	link->status.stats.rx_bytes = (uint32_t)(link->status.stats.rx_bytes + len);

	if (frame.type == ZLRS_PKT_PAIR_REQ || frame.type == ZLRS_PKT_PAIR_RESP ||
	    frame.type == ZLRS_PKT_PAIR_CFM) {
		/*
		 * SEARCHING as well: the responder repeats its answer until the confirmation
		 * arrives, and an initiator that already moved on has to be able to answer that
		 * repeat with another confirmation (the handshake has no other acknowledgement).
		 */
		if (link->status.state != ZLRS_STATE_PAIRING &&
		    link->status.state != ZLRS_STATE_SEARCHING) {
			return false;
		}
		return link_handle_pair_frame(link, &frame, now_ms);
	}

	if (frame.type == ZLRS_PKT_SYNC) {
		return link_handle_sync(link, &frame, now_ms);
	}

	if (frame.type == ZLRS_PKT_SESS_RESP) {
		return link_handle_sess_resp(link, &frame, now_ms);
	}

	if (link->status.state != ZLRS_STATE_CONNECTED || !link->status.session_valid ||
	    (link->status.cfg.role == ZLRS_ROLE_FOLLOWER && link->status.radio_acquiring) ||
	    frame.type != (link->status.cfg.role == ZLRS_ROLE_LEADER ? ZLRS_PKT_DATA_F2L
								     : ZLRS_PKT_DATA_L2F)) {
		return false;
	}
	if (frame.seq <= link->last_rx_seq) {
		link->status.stats.rx_dup++;
		return false;
	}
	link->last_rx_seq = frame.seq;
	link->status.last_rx_ms = now_ms;
	link->status.last_rx_frame = link->status.frame_index;
	if (link->status.cfg.role == ZLRS_ROLE_LEADER) {
		link->status.sess_cfm_due = false;
		link->status.radio_acquiring = false;
	}
	link_lq_account(link, frame.seq);
	link_handle_data(link, &frame.body.data, now_ms);
	return true;
}

static void advance_hop(zlrs_link_t *link)
{
	if (link->status.cfg.hopping == 0U) {
		/* Pinned to the sync channel (entry 0 of the hop table) */
		return;
	}

	if (link->status.state == ZLRS_STATE_CONNECTED) {
		link->status.stats.hop_advances++;
		zlrs_fhss_set_index(&link->status.fhss, link->status.fhss.index + 1U);
	}
}

void link_plan_rx(const zlrs_link_t *link, zlrs_link_plan_t *plan, uint32_t freq, uint8_t sync_slot,
		  uint16_t timeout_ms)
{
	plan->op = ZLRS_OP_RX;
	plan->radio = (zlrs_radio_req_t){.freq_hz = freq,
					 .phy = link->status.cfg.phy,
					 .sync_slot = sync_slot,
					 .max_rx_size = ZLRS_FRAME_MAX_LEN,
					 .tx_power_dbm = link->status.cfg.tx_power_dbm,
					 .timeout_ms = timeout_ms};
	memcpy(plan->radio.sync_word,
	       sync_slot == ZLRS_SYNC_SLOT_PAIRING ? pairing_sync_word : link->sync_word,
	       ZLRS_SYNC_WORD_LEN);
}

void link_plan_tx(const zlrs_link_t *link, zlrs_link_plan_t *plan, uint8_t type, uint16_t data_len)
{
	const bool pairing = zlrs_packet_layouts[type].auth == ZLRS_AUTH_ROOT;
	const uint16_t payload_len = zlrs_packet_layouts[type].payload_len + data_len;
	plan->op = ZLRS_OP_TX;
	plan->type = type;
	plan->radio = (zlrs_radio_req_t){
		.freq_hz = type <= ZLRS_PKT_DATA_F2L ? link_slot_freq_hz(link)
						     : link_sync_freq_hz(link),
		.phy = link->status.cfg.phy,
		.sync_slot = pairing ? ZLRS_SYNC_SLOT_PAIRING : ZLRS_SYNC_SLOT_DATA,
		.is_tx = true,
		.tx_power_dbm = pairing ? link->status.cfg.pair_tx_power_dbm
					: link->status.cfg.tx_power_dbm,
		.size = zlrs_frame_len(payload_len,
				       zlrs_profile_tag_len(link->status.cfg.profile))};
	memcpy(plan->radio.sync_word, pairing ? pairing_sync_word : link->sync_word,
	       ZLRS_SYNC_WORD_LEN);
}

void zlrs_link_plan(const zlrs_link_t *link, zlrs_link_plan_t *plan)
{
	*plan = (zlrs_link_plan_t){.frame_index = link->status.frame_index,
				   .now_ms = link->slot_ms,
				   .slot = link->slot};
	if (link->status.busy || link->slot_idle) {
		return;
	}
	const bool searching = link->status.state == ZLRS_STATE_SEARCHING;
	if (link->status.state == ZLRS_STATE_PAIRING ||
	    (searching && link->status.pair_cfm_pending)) {
		link_pair_plan(link, plan);
		return;
	}
	if (!searching && link->status.state != ZLRS_STATE_CONNECTED) {
		return;
	}
	const bool leader = link->status.cfg.role == ZLRS_ROLE_LEADER;
	const uint32_t now_ms = link->slot_ms;
	if (leader != (link->slot == ZLRS_SLOT_DATA_A)) {
		const bool sync_channel =
			searching ||
			(leader && (uint32_t)(now_ms - link->state_ms) < ZLRS_SESS_CONFIRM_MS) ||
			(!leader && link->status.cfg.resync_every_n > 0U &&
			 (uint32_t)(link->status.frame_index - link->status.last_rx_frame) >
				 2U * link->status.cfg.resync_every_n);
		link_plan_rx(link, plan,
			     sync_channel ? link_sync_freq_hz(link) : link_slot_freq_hz(link),
			     ZLRS_SYNC_SLOT_DATA,
			     searching ? link_acq_timeout_ms(link)
				       : link->status.cfg.rx_timeout_ms);
		return;
	}
	const bool sync_due =
		(uint32_t)(now_ms - link->last_sync_ms) >= link->status.cfg.sync_period_ms;
	if (searching) {
		if (leader && (link->session_pending || link->status.session_valid)) {
			if (link->status.sess_cfm_due && !sync_due) {
				link_plan_tx(link, plan, ZLRS_PKT_SESS_CFM, 0U);
			} else if (sync_due || sync_frame(link)) {
				link_plan_tx(link, plan, ZLRS_PKT_SYNC, 0U);
			}
		} else if (!leader && link->session_pending) {
			link_plan_tx(link, plan, ZLRS_PKT_SESS_RESP, 0U);
		}
		return;
	}
	if (link->status.radio_acquiring && (!leader || !link->status.sess_cfm_due)) {
		if (leader && sync_due) {
			link_plan_tx(link, plan, ZLRS_PKT_SYNC, 0U);
		}
		return;
	}
	const uint8_t data_type = leader ? ZLRS_PKT_DATA_L2F : ZLRS_PKT_DATA_F2L;
	if (link->sess_ack_due) {
		link_plan_tx(link, plan, data_type, 0U);
		return;
	}
	if (leader && (link->status.sess_cfm_due || sync_frame(link) ||
		       ((link->status.radio_acquiring || link->band_announced) && sync_due))) {
		link_plan_tx(link, plan,
			     link->status.sess_cfm_due && !sync_due ? ZLRS_PKT_SESS_CFM
								    : ZLRS_PKT_SYNC,
			     0U);
		return;
	}
	const bool retry = link->status.cfg.arq && link_arq_retry_due(link, now_ms);
	const bool waiting = link->status.cfg.arq && link->status.arq_used && !retry;
	const bool queued = !waiting && link->tx_packet_count != 0U;
	const bool heartbeat =
		link->status.cfg.arq &&
		(link->status.arq_ack_pending || (link->status.cfg.feedback_period_ms > 0U &&
						  (uint32_t)(now_ms - link->last_feedback_tx_ms) >=
							  link->status.cfg.feedback_period_ms));
	if (queued || heartbeat) {
		link_plan_tx(link, plan, data_type,
			     queued ? link->tx_packets[link->queue_head].len : 0U);
	}
	plan->arq_wait = waiting;
}

const zlrs_radio_req_t *zlrs_link_finalize(zlrs_link_t *link, const zlrs_link_plan_t *plan)
{
	if (link->status.busy || plan->frame_index != link->status.frame_index ||
	    plan->slot != link->slot || plan->now_ms != link->slot_ms) {
		return NULL;
	}
	link->status.stats.tx_arq_wait_stalls += plan->arq_wait;
	if (plan->op == ZLRS_OP_IDLE) {
		return NULL;
	}
	zlrs_radio_req_t req = plan->radio;
	req.buf = link->rx_buf;
	const bool data = plan->op == ZLRS_OP_TX && plan->type <= ZLRS_PKT_DATA_F2L;
	const uint16_t data_len = data ? req.size - ZLRS_DATA_HEADROOM -
						  zlrs_profile_tag_len(link->status.cfg.profile)
				       : 0U;
	if (plan->op == ZLRS_OP_TX) {
		req.buf = link->tx_buf;
		if (data) {
			req.buf = link_build_data(link, data_len, &req.size);
		} else {
			req.size = link_build_handshake(link, plan->type);
		}
		if (req.buf == NULL || req.size == 0U) {
			return NULL;
		}
	}
	link->req = req;
	link->req_is_data = data;
	link->req_data_len = data_len;
	link->req_data_mtu = link->status.cfg.data_mtu;
	link->req_arq = link->status.cfg.arq != 0U;
	link->req_started = false;
	link->status.busy = true;
	return &link->req;
}

static void limit_data_mtu(zlrs_link_t *link)
{
	const uint16_t max_frame =
		link->status.cfg.automatic_band || zlrs_phys[link->status.cfg.phy].lora_sf != 0U
			? ZLRS_LORA_FRAME_MAX_LEN
			: ZLRS_FRAME_MAX_LEN;
	const uint16_t mtu = max_frame - ZLRS_FRAME_HDR_LEN - ZLRS_DATA_PREFIX_LEN -
			     zlrs_profile_tag_len(link->status.cfg.profile);

	if (link->status.cfg.data_mtu > mtu) {
		link->status.cfg.data_mtu = mtu;
	}
}

void zlrs_link_init(zlrs_link_t *link, const zlrs_link_cfg_t *cfg,
		    bool (*random)(void *ctx, uint32_t *out), void *random_ctx,
		    const zlrs_link_events_t *events)
{
	memset(link, 0, sizeof(*link));
	link->status.cfg = *cfg;
	limit_data_mtu(link);
	link->random = random;
	link->random_ctx = random_ctx;
	if (events != NULL) {
		link->ev = *events;
	}

	link->tx_seq = 1U;
	link->pair_tx_seq = 1U;
	link_quality_reset(link);
	link->status.state = ZLRS_STATE_UNCONFIGURED;
	link->status.radio_acquiring = cfg->automatic_band;
}

void zlrs_link_set_power(zlrs_link_t *link, int8_t tx_power_dbm)
{
	link->status.cfg.tx_power_dbm = tx_power_dbm;
}

bool link_band_plan_valid(const zlrs_link_t *link, const zlrs_band_plan_t *plan,
			  uint32_t frame_index)
{
	const uint8_t phy =
		link->status.cfg.region == ZLRS_REGION_ISM2400 ? link->status.cfg.phy : 0U;
	if (plan->region != link->status.cfg.region ||
	    plan->automatic != link->status.cfg.automatic_band || plan->phy != phy ||
	    plan->target_phy >= ZLRS_PHY_COUNT ||
	    (plan->automatic &&
	     (plan->phy >= ZLRS_PHY_AUTO_COUNT || plan->target_phy >= ZLRS_PHY_AUTO_COUNT)) ||
	    (plan->target_subg && plan->target_phy != 0U)) {
		return false;
	}
	if (plan->switch_frame == 0U) {
		return true;
	}
	return plan->automatic && plan->switch_frame > frame_index &&
	       plan->switch_frame - frame_index <= UINT16_MAX &&
	       (plan->target_subg != (link->status.cfg.region != ZLRS_REGION_ISM2400) ||
		(!plan->target_subg && plan->target_phy != phy));
}

void link_adopt_band_plan(zlrs_link_t *link, const zlrs_band_plan_t *plan)
{
	link->status.band_plan = *plan;
	link->band_announced = plan->switch_frame != 0U;
}

void zlrs_link_acquire_radio(zlrs_link_t *link, uint32_t now_ms)
{
	link->status.band_plan = (zlrs_band_plan_t){
		.region = link->status.cfg.region,
		.automatic = link->status.cfg.automatic_band,
		.phy = link->status.cfg.region == ZLRS_REGION_ISM2400 ? link->status.cfg.phy : 0U,
		.target_subg = link->status.cfg.region != ZLRS_REGION_ISM2400};
	link->band_announced = false;
	link->status.radio_acquiring = true;
	link->status.radio_sync_seen = false;
	link->sess_ack_due = false;
	link->last_sync_ms = now_ms - link->status.cfg.sync_period_ms;
}

bool zlrs_link_radio_captured(zlrs_link_t *link)
{
	if (link->status.cfg.role != ZLRS_ROLE_FOLLOWER || !link->status.radio_acquiring ||
	    !link->status.radio_sync_seen || !link->status.session_valid) {
		return false;
	}
	link->status.radio_acquiring = false;
	link->sess_ack_due = true;
	return true;
}

bool zlrs_link_plan_radio(zlrs_link_t *link, bool target_subg, zlrs_phy_t target_phy,
			  uint32_t switch_frame)
{
	if ((unsigned int)target_phy >= ZLRS_PHY_AUTO_COUNT) {
		return false;
	}
	zlrs_band_plan_t plan = {
		.region = link->status.cfg.region,
		.automatic = 1U,
		.phy = link->status.cfg.region == ZLRS_REGION_ISM2400 ? link->status.cfg.phy : 0U,
		.target_phy = target_phy,
		.target_subg = target_subg,
		.switch_frame = switch_frame};
	if (link->status.busy || link->status.cfg.role != ZLRS_ROLE_LEADER ||
	    !link->status.session_valid ||
	    !link_band_plan_valid(link, &plan, link->status.frame_index) || switch_frame == 0U) {
		return false;
	}
	link->status.band_plan = plan;
	/* First announcement uses a periodic SYNC that the hopping peer expects. */
	link->band_announced = false;
	return true;
}

bool zlrs_link_reconfigure_radio(zlrs_link_t *link, const zlrs_link_cfg_t *cfg)
{
	if (link->status.busy || cfg->region >= ZLRS_REGION_COUNT ||
	    cfg->role != link->status.cfg.role || cfg->profile != link->status.cfg.profile ||
	    cfg->arq != link->status.cfg.arq || cfg->data_mtu != link->status.cfg.data_mtu ||
	    cfg->automatic_band != link->status.cfg.automatic_band) {
		return false;
	}
	uint8_t seed[4];
	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, "zlrs/fhss", NULL, 0U, seed, sizeof(seed));
	link->status.cfg = *cfg;
	zlrs_fhss_init(&link->status.fhss, cfg->region, sys_get_le32(seed));
	zlrs_link_acquire_radio(link, link->status.last_rx_ms);
	/* Quality/power samples belong to the old RF path; replay/ARQ state does not. */
	link_quality_reset(link);
	return true;
}

bool zlrs_link_advance_slot(zlrs_link_t *link, uint8_t slot, uint32_t now_ms)
{
	if (link->status.busy || slot >= ZLRS_SLOT_COUNT) {
		return false;
	}
	link->slot = slot;
	link->slot_ms = now_ms;
	if (link->tx_seq == UINT32_MAX && (link->status.state == ZLRS_STATE_CONNECTED ||
					   link->status.state == ZLRS_STATE_SEARCHING)) {
		/* Rekey before exhausting the counter; keep all queued/unacknowledged bytes. */
		link->status.session_valid = false;
		link->session_pending = false;
		const bool was_searching = link->status.state == ZLRS_STATE_SEARCHING;
		link_set_state(link, ZLRS_STATE_SEARCHING, now_ms);
		if (was_searching && link->status.cfg.role == ZLRS_ROLE_LEADER) {
			link_roll_session(link);
		}
		link->last_sync_ms = now_ms - link->status.cfg.sync_period_ms;
	}

	link->slot_idle = false;
	const bool pairing =
		link->status.state == ZLRS_STATE_PAIRING ||
		(link->status.state == ZLRS_STATE_SEARCHING && link->status.pair_cfm_pending);
	if (slot == ZLRS_SLOT_DATA_A) {
		link->status.frame_index++;
	}
	if (pairing) {
		link->slot_idle = link_pair_advance(link);
	} else if (link->status.state == ZLRS_STATE_CONNECTED) {
		advance_hop(link);
	} else if (link->status.state == ZLRS_STATE_SEARCHING &&
		   link->status.cfg.role == ZLRS_ROLE_LEADER && !link->status.session_valid &&
		   !link->session_pending) {
		link_roll_session(link);
	}
	return true;
}

void zlrs_link_skip_slot(zlrs_link_t *link, uint8_t slot)
{
	link->slot = slot;
	if (slot == ZLRS_SLOT_DATA_A) {
		link->status.frame_index++;
	}
	advance_hop(link);
}

void zlrs_link_tx_started(zlrs_link_t *link, uint32_t now_ms)
{
	if (!link->status.busy || !link->req.is_tx || link->req_started) {
		return;
	}
	link->req_started = true;
	const uint8_t type = link->req.buf[0] & ZLRS_FRAME_TYPE_MASK;
	if (type == ZLRS_PKT_PAIR_CFM) {
		link->status.pair_cfm_sent++;
	} else if (type == ZLRS_PKT_SESS_CFM && link->status.state == ZLRS_STATE_SEARCHING) {
		link_set_state(link, ZLRS_STATE_CONNECTED, now_ms);
	}
	if (link->req_is_data && link->req_arq && link->req_data_len != 0U) {
		if (!link->arq_launched) {
			link->arq_first_sent_ms = now_ms;
		}
		link->arq_launched = true;
		link->arq_sent_ms = now_ms;
	}
}

void zlrs_link_complete(zlrs_link_t *link, int status, uint16_t rx_len, int16_t rssi, int8_t snr,
			uint32_t now_ms)
{
	if (!link->status.busy) {
		return;
	}
	if (status == ZLRS_RADIO_OK) {
		zlrs_link_tx_started(link, now_ms);
	}
	const bool was_tx = link->req.is_tx;
	const uint8_t slot = (link->slot < ZLRS_SLOT_COUNT) ? link->slot : 0U;

	link->status.busy = false;

	if (status != ZLRS_RADIO_OK) {
		link->status.stats.radio_fail++;
		link->status.stats.slot_fail[slot]++;
		if (was_tx) {
			link->status.stats.tx_cancelled_frames += !link->req_started;
			link->status.stats.tx_failed_frames += link->req_started;
			link_complete_data(link, false, now_ms);
		}
		return;
	}

	if (was_tx) {
		link->status.stats.slot_tx[slot]++;
		link->status.stats.tx_frames++;
		link->status.stats.tx_bytes =
			(uint32_t)(link->status.stats.tx_bytes + link->req.size);
		link_complete_data(link, true, now_ms);
		return;
	}

	link->status.stats.slot_rx[slot]++;
	if (rx_len > 0U && rssi != 0) {
		if (link->status.stats.rssi_min == 0 || rssi < link->status.stats.rssi_min) {
			link->status.stats.rssi_min = rssi;
		}
		if (link->status.stats.rssi_max == 0 || rssi > link->status.stats.rssi_max) {
			link->status.stats.rssi_max = rssi;
		}
	}
	if (rx_len > 0U) {
		link->status.stats.slot_hit[slot]++;
		if (handle_rx(link, rx_len, now_ms)) {
			link->status.stats.last_rssi = rssi;
			link->status.stats.last_snr = snr;
			link->status.accepted_rx_events++;
		}
	}
}

void zlrs_link_tick(zlrs_link_t *link, uint32_t now_ms)
{

	switch (link->status.state) {
	case ZLRS_STATE_UNCONFIGURED:
		if (link->status.configured) {
			link_set_state(link, ZLRS_STATE_UNPAIRED, now_ms);
		}
		break;

	case ZLRS_STATE_PAIRING:
		if ((uint32_t)(now_ms - link->state_ms) > link->status.cfg.pair_window_ms) {
			(void)zlrs_link_cancel_pair(link, now_ms);
		}
		break;

	case ZLRS_STATE_CONNECTED:
		/*
		 * Grace after entering CONNECTED: the peer may still be leaving SEARCHING (it needs
		 * to hear one of our confirms), and its first data frame cannot arrive until it
		 * does. Running the peer-loss check inside that window would roll a new session and
		 * restart the handshake before it ever had a chance to finish.
		 */
		if ((uint32_t)(now_ms - link->state_ms) <= ZLRS_SESS_CONFIRM_MS) {
			break;
		}
		if (link->status.cfg.peer_timeout_ms > 0U &&
		    (uint32_t)(now_ms - link->status.last_rx_ms) >
			    link->status.cfg.peer_timeout_ms) {
			link->status.session_valid = false;
			link_set_state(link, ZLRS_STATE_SEARCHING, now_ms);
		}
		break;

	default:
		break;
	}
}

const zlrs_link_status_t *zlrs_link_status(const zlrs_link_t *link)
{
	return &link->status;
}
