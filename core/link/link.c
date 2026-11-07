/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* Link lifecycle, slot scheduling, frame authentication and radio completion. */

bool link_new_nonce(const zlrs_link_t *link, uint32_t previous, bool have_previous, uint32_t *out)
{
	if (link->random == NULL) {
		return false;
	}
	for (unsigned int i = 0U; i < 4U; i++) {
		uint32_t nonce;
		if (!link->random(link->random_ctx, &nonce)) {
			return false;
		}
		if (!have_previous || nonce != previous) {
			*out = nonce;
			return true;
		}
	}
	return false;
}

uint32_t link_sync_freq_hz(const zlrs_link_t *link)
{
	const zlrs_fhss_domain_t *domain = &zlrs_fhss_domains[link->cfg.domain];
	const uint8_t channel = (uint8_t)(domain->channel_count / 2U);

	return domain->start_hz + (uint32_t)channel * domain->step_hz;
}

static bool sync_frame(const zlrs_link_t *link)
{
	return link->cfg.resync_every_n > 0U &&
	       (link->frame_index % link->cfg.resync_every_n) == 0U;
}

uint32_t link_slot_freq_hz(const zlrs_link_t *link)
{
	if (link->radio_acquiring || link->band_announced || sync_frame(link) ||
	    link->sess_cfm_due || link->sess_ack_due) {
		return link_sync_freq_hz(link);
	}
	/* Both ends count from the confirmed frame, including repeated confirms after lost ACKs.
	 * Keep initial DATA on the same channel where the peer is waiting for confirmation. */
	if (link->session_valid &&
	    (uint32_t)(link->frame_index - link->sess_sync_frame) < ZLRS_DATA_CONFIRM_FRAMES) {
		return link_sync_freq_hz(link);
	}
	/* Both ends keep fresh-session slot B on the confirmation channel. */
	if ((link->slot == ZLRS_SLOT_DATA_B) &&
	    (uint32_t)(link->slot_ms - link->state_ms) < ZLRS_SESS_CONFIRM_MS) {
		return link_sync_freq_hz(link);
	}

	return zlrs_fhss_current_hz(&link->fhss);
}

void link_set_state(zlrs_link_t *link, zlrs_link_state_t state, uint32_t now_ms)
{
	if (link->state == state) {
		return;
	}

	link->state = state;
	link->state_ms = now_ms;
	if (link->ev.on_state != NULL) {
		link->ev.on_state(link->ev.ctx, state);
	}

	/* Leader is the authority: every return to SEARCHING rolls a new session nonce. */
	if (state == ZLRS_STATE_SEARCHING && link->cfg.role == ZLRS_ROLE_LEADER) {
		link_roll_session(link, now_ms);
		/* Send a SYNC right away instead of waiting a full period, to shorten the reconnect */
		link->last_sync_ms = (uint32_t)(now_ms - link->cfg.sync_period_ms);
	}
}

static uint16_t link_encode_frame(const zlrs_link_t *link, uint8_t *buf, const zlrs_frame_t *frame,
				  uint16_t payload_len, const uint8_t *tag_key)
{
	if (link->cfg.profile > ZLRS_PROFILE_P1 ||
	    payload_len > ZLRS_PAYLOAD_MAX_LEN - zlrs_profile_tag_len(link->cfg.profile)) {
		return 0U;
	}
	const uint8_t tag_len = zlrs_profile_tag_len(link->cfg.profile);
	const uint16_t len = zlrs_frame_len(payload_len, tag_len);
	const uint16_t total = zlrs_frame_encode(frame, buf, sizeof(link->tx_buf), len, tag_len);
	if (total != 0U && tag_len != 0U) {
		const uint16_t mac_len = total - tag_len;
		zlrs_tag(tag_key, ZLRS_KEY_LEN, buf, mac_len, buf + mac_len);
	}
	return total;
}

uint16_t link_build_frame(zlrs_link_t *link, zlrs_frame_t *frame, uint16_t payload_len,
			  const uint8_t *tag_key)
{
	uint32_t *const counter =
		frame->type >= ZLRS_PKT_PAIR_REQ && frame->type <= ZLRS_PKT_PAIR_CFM
			? &link->pair_tx_seq
			: &link->tx_seq;
	if (*counter == 0U || *counter == UINT32_MAX) {
		return 0U;
	}
	frame->seq = *counter;
	const uint16_t total = link_encode_frame(link, link->tx_buf, frame, payload_len, tag_key);
	if (total > 0U) {
		(*counter)++;
		link->stats.tx_frame_builds++;
		link->stats.tx_auth_tags += zlrs_profile_tag_len(link->cfg.profile) != 0U;
	}
	return total;
}

const uint8_t *link_tag_key_for(const zlrs_link_t *link, uint8_t type, bool outbound)
{
	const uint8_t *link_auth = outbound ? link->link_auth_out : link->link_auth_in;
	const uint8_t *session = outbound ? link->session_key_out : link->session_key_in;

	switch (type) {
	case ZLRS_PKT_PAIR_REQ:
	case ZLRS_PKT_PAIR_RESP:
	case ZLRS_PKT_PAIR_CFM:
		return link->auth_key;
	case ZLRS_PKT_SYNC:
	/* SESS_RESP must verify before the session key exists, so it uses the pairing-level key */
	case ZLRS_PKT_SESS_RESP:
		return link_auth;
	/* The confirm frame must be tagged with the very session key it confirms */
	case ZLRS_PKT_SESS_CFM:
		return session;
	default:
		return link->session_valid ? session : link_auth;
	}
}

static const zlrs_radio_req_t *make_req(zlrs_link_t *link, bool is_tx, uint8_t *buf, uint16_t size,
					uint32_t freq_hz, uint8_t sync_slot,
					const uint8_t sync_word[ZLRS_SYNC_WORD_LEN],
					uint16_t timeout_ms)
{
	if (link->req_pending) {
		return NULL;
	}

	link->req.freq_hz = freq_hz;
	link->req.mod = link->cfg.mod;
	link->req.sync_slot = sync_slot;
	memcpy(link->req.sync_word, sync_word, ZLRS_SYNC_WORD_LEN);
	link->req.is_tx = is_tx;
	link->req.tx_power_dbm = link->cfg.tx_power_dbm;
	link->req.size = size;
	link->req.max_rx_size = is_tx ? 0U : ZLRS_FRAME_MAX_LEN;
	link->req.timeout_ms = timeout_ms;
	link->req.buf = buf;
	link->req_is_tx = is_tx;
	/* The encoder has already validated TX shape. Snapshot the actual request buffer,
	 * which may be the staged packet rather than tx_buf, without decoding/copying it. */
	link->req_is_data = is_tx && (buf[0] == ZLRS_PKT_DATA_L2F || buf[0] == ZLRS_PKT_DATA_F2L);
	link->req_data_len = link->req_is_data ? size - ZLRS_FRAME_HDR_LEN - ZLRS_DATA_PREFIX_LEN -
							 zlrs_profile_tag_len(link->cfg.profile)
					       : 0U;
	link->req_data_mtu = link->cfg.data_mtu;
	link->req_arq = link->cfg.arq != 0U;
	link->req_started = false;
	link->req_pending = true;

	return &link->req;
}

const zlrs_radio_req_t *link_submit_tx(zlrs_link_t *link, uint8_t *buf, uint16_t len,
				       uint32_t freq_hz, uint8_t sync_slot,
				       const uint8_t sync_word[ZLRS_SYNC_WORD_LEN])
{
	if (len == 0U) {
		return NULL;
	}

	return make_req(link, true, buf, len, freq_hz, sync_slot, sync_word, 0U);
}

const zlrs_radio_req_t *link_submit_rx(zlrs_link_t *link, uint32_t freq_hz, uint8_t sync_slot,
				       const uint8_t sync_word[ZLRS_SYNC_WORD_LEN],
				       uint16_t timeout_ms)
{
	return make_req(link, false, link->rx_buf, 0U, freq_hz, sync_slot, sync_word, timeout_ms);
}

uint16_t link_acq_timeout_ms(const zlrs_link_t *link)
{
	return (link->cfg.acq_timeout_ms != 0U) ? link->cfg.acq_timeout_ms
						: link->cfg.rx_timeout_ms;
}

static int handle_control_rx(zlrs_link_t *link, uint16_t len, uint32_t now_ms)
{
	const uint8_t tag_len = zlrs_profile_tag_len(link->cfg.profile);
	zlrs_frame_t frame;
	const uint8_t *key;

	if (!zlrs_frame_decode(&frame, link->rx_buf, len, tag_len)) {
		link->stats.rx_bad_frame++;
		return -1;
	}

	/* The confirm verifies with the candidate key first, not the state-based path below */
	if (frame.type == ZLRS_PKT_SESS_CFM) {
		return link_handle_sess_cfm(link, &frame, len, now_ms);
	}

	key = link_tag_key_for(link, frame.type, false);
	if (tag_len > 0U) {
		uint8_t expect[ZLRS_TAG_LEN];

		zlrs_tag(key, ZLRS_KEY_LEN, link->rx_buf, len - tag_len, expect);
		if (!zlrs_ct_equal(expect, frame.tag, ZLRS_TAG_LEN)) {
			link->stats.rx_bad_tag++;
			return -1;
		}
	}

	link->stats.rx_frames++;
	link->stats.rx_bytes = (uint32_t)(link->stats.rx_bytes + len);

	if (frame.type == ZLRS_PKT_PAIR_REQ || frame.type == ZLRS_PKT_PAIR_RESP ||
	    frame.type == ZLRS_PKT_PAIR_CFM) {
		/*
		 * SEARCHING as well: the responder repeats its answer until the confirmation arrives, and
		 * an initiator that already moved on has to be able to answer that repeat with another
		 * confirmation (the handshake has no other acknowledgement).
		 */
		if (link->state != ZLRS_STATE_PAIRING && link->state != ZLRS_STATE_SEARCHING) {
			return -1;
		}
		return link_handle_pair_frame(link, &frame, now_ms) ? 0 : -1;
	}

	if (frame.type == ZLRS_PKT_SYNC) {
		return link_handle_sync(link, &frame, now_ms) ? 0 : -1;
	}

	if (frame.type == ZLRS_PKT_SESS_RESP) {
		return link_handle_sess_resp(link, &frame, now_ms) ? 0 : -1;
	}

	return -1;
}

static int handle_rx(zlrs_link_t *link, uint16_t len, uint32_t now_ms)
{
	if (len == 0U || (link->rx_buf[0] & ZLRS_FRAME_TYPE_MASK) > ZLRS_PKT_DATA_F2L) {
		return handle_control_rx(link, len, now_ms);
	}
	const uint8_t tag_len = zlrs_profile_tag_len(link->cfg.profile);
	zlrs_data_view_t view;
	if (!zlrs_frame_decode_data(&view, link->rx_buf, len, tag_len)) {
		link->stats.rx_bad_frame++;
		return -1;
	}
	if (tag_len != 0U) {
		uint8_t expect[ZLRS_TAG_LEN];
		zlrs_tag(link_tag_key_for(link, view.type, false), ZLRS_KEY_LEN, link->rx_buf,
			 len - tag_len, expect);
		if (!zlrs_ct_equal(expect, view.tag, tag_len)) {
			link->stats.rx_bad_tag++;
			return -1;
		}
	}
	link->stats.rx_frames++;
	link->stats.rx_bytes += len;
	if (link->state != ZLRS_STATE_CONNECTED || !link->session_valid ||
	    (link->cfg.role == ZLRS_ROLE_FOLLOWER && link->radio_acquiring) ||
	    view.type !=
		    (link->cfg.role == ZLRS_ROLE_LEADER ? ZLRS_PKT_DATA_F2L : ZLRS_PKT_DATA_L2F)) {
		return -1;
	}
	if (view.seq <= link->last_rx_seq) {
		link->stats.rx_dup++;
		return -1;
	}
	link->last_rx_seq = view.seq;
	link->last_rx_ms = now_ms;
	link->last_rx_frame = link->frame_index;
	if (link->cfg.role == ZLRS_ROLE_LEADER) {
		link->sess_cfm_due = false;
		link->radio_acquiring = false;
	}
	link_lq_account(link, view.seq);
	link_handle_data(link, &view, now_ms);
	return 0;
}

static void advance_hop(zlrs_link_t *link)
{
	if (link->cfg.hopping == 0U) {
		/* Pinned to the sync channel (entry 0 of the hop table) */
		return;
	}

	if (link->state == ZLRS_STATE_CONNECTED) {
		link->stats.hop_advances++;
		(void)zlrs_fhss_next_hz(&link->fhss);
	}
}

void link_plan_rx(const zlrs_link_t *link, zlrs_link_plan_t *plan, uint32_t freq, uint8_t sync_slot,
		  const uint8_t *sync_word, uint16_t timeout_ms)
{
	plan->op = ZLRS_OP_RX;
	plan->radio = (zlrs_radio_req_t){.freq_hz = freq,
					 .mod = link->cfg.mod,
					 .sync_slot = sync_slot,
					 .max_rx_size = ZLRS_FRAME_MAX_LEN,
					 .timeout_ms = timeout_ms};
	memcpy(plan->radio.sync_word, sync_word, ZLRS_SYNC_WORD_LEN);
}

void link_plan_tx(const zlrs_link_t *link, zlrs_link_plan_t *plan, uint8_t type,
		  uint16_t payload_len, uint32_t freq)
{
	plan->op = ZLRS_OP_TX;
	plan->type = type;
	plan->radio = (zlrs_radio_req_t){
		.freq_hz = freq,
		.mod = link->cfg.mod,
		.sync_slot = ZLRS_SYNC_SLOT_DATA,
		.is_tx = true,
		.tx_power_dbm = link->cfg.tx_power_dbm,
		.size = zlrs_frame_len(payload_len, zlrs_profile_tag_len(link->cfg.profile))};
	memcpy(plan->radio.sync_word, link->sync_word, ZLRS_SYNC_WORD_LEN);
}

zlrs_link_plan_t zlrs_link_plan(const zlrs_link_t *link)
{
	zlrs_link_plan_t plan = {
		.frame_index = link->frame_index, .now_ms = link->slot_ms, .slot = link->slot};
	if (link->req_pending || link->slot_idle) {
		return plan;
	}
	const bool a = link->slot == ZLRS_SLOT_DATA_A;
	const bool leader = link->cfg.role == ZLRS_ROLE_LEADER;
	const uint32_t now_ms = link->slot_ms;
	const uint32_t sync_freq = link_sync_freq_hz(link);
	if (link->state == ZLRS_STATE_PAIRING ||
	    (link->state == ZLRS_STATE_SEARCHING && link->pair_cfm_pending)) {
		link_pair_plan(link, &plan);
		return plan;
	}
	if (link->state == ZLRS_STATE_SEARCHING) {
		if (leader && a) {
			if (!link->session_pending && !link->session_valid) {
				return plan;
			}
			if (link->sess_cfm_due &&
			    (uint32_t)(now_ms - link->last_sync_ms) < link->cfg.sync_period_ms) {
				link_plan_tx(link, &plan, ZLRS_PKT_SESS_CFM,
					     ZLRS_SESS_CFM_PAYLOAD_LEN, sync_freq);
			} else if ((uint32_t)(now_ms - link->last_sync_ms) >=
					   link->cfg.sync_period_ms ||
				   sync_frame(link)) {
				link_plan_tx(link, &plan, ZLRS_PKT_SYNC, ZLRS_SYNC_PAYLOAD_LEN,
					     sync_freq);
			}
		} else if (a || leader) {
			link_plan_rx(link, &plan, sync_freq, ZLRS_SYNC_SLOT_DATA, link->sync_word,
				     link_acq_timeout_ms(link));
		} else if (link->sess_resp_due || link->session_pending) {
			link_plan_tx(link, &plan, ZLRS_PKT_SESS_RESP, ZLRS_SESS_RESP_PAYLOAD_LEN,
				     sync_freq);
		}
		return plan;
	}
	if (link->state != ZLRS_STATE_CONNECTED) {
		return plan;
	}
	if (link->radio_acquiring && leader == a) {
		if (!leader) {
			return plan;
		}
		if (!link->sess_cfm_due) {
			if ((uint32_t)(now_ms - link->last_sync_ms) >= link->cfg.sync_period_ms) {
				link_plan_tx(link, &plan, ZLRS_PKT_SYNC, ZLRS_SYNC_PAYLOAD_LEN,
					     sync_freq);
			}
			return plan;
		}
	}
	if (link->sess_ack_due && leader == a) {
		link_plan_tx(link, &plan, leader ? ZLRS_PKT_DATA_L2F : ZLRS_PKT_DATA_F2L,
			     ZLRS_DATA_PREFIX_LEN, link_slot_freq_hz(link));
		return plan;
	}
	if (!leader && a && link->cfg.resync_every_n > 0U &&
	    (uint32_t)(link->frame_index - link->last_rx_frame) > 2U * link->cfg.resync_every_n) {
		link_plan_rx(link, &plan, sync_freq, ZLRS_SYNC_SLOT_DATA, link->sync_word,
			     link->cfg.rx_timeout_ms);
		return plan;
	}
	if (leader && a && link->sess_cfm_due) {
		if ((uint32_t)(now_ms - link->last_sync_ms) >= link->cfg.sync_period_ms) {
			link_plan_tx(link, &plan, ZLRS_PKT_SYNC, ZLRS_SYNC_PAYLOAD_LEN, sync_freq);
		} else {
			link_plan_tx(link, &plan, ZLRS_PKT_SESS_CFM, ZLRS_SESS_CFM_PAYLOAD_LEN,
				     sync_freq);
		}
		return plan;
	}
	if (leader && a &&
	    (sync_frame(link) ||
	     ((link->radio_acquiring || link->band_announced) &&
	      (uint32_t)(now_ms - link->last_sync_ms) >= link->cfg.sync_period_ms))) {
		link_plan_tx(link, &plan, ZLRS_PKT_SYNC, ZLRS_SYNC_PAYLOAD_LEN, sync_freq);
		return plan;
	}
	if (leader == a) {
		const bool retry = link->cfg.arq && link_arq_retry_due(link, now_ms);
		const bool waiting = link->cfg.arq && link->arq_used && !retry;
		if (!waiting && link->tx_packet_count != 0U) {
			link_plan_tx(link, &plan, leader ? ZLRS_PKT_DATA_L2F : ZLRS_PKT_DATA_F2L,
				     ZLRS_DATA_PREFIX_LEN + link->tx_packets[link->queue_head].len,
				     link_slot_freq_hz(link));
		} else if (link->cfg.arq && (link->arq_ack_pending ||
					     (link->cfg.feedback_period_ms > 0U &&
					      (uint32_t)(now_ms - link->last_feedback_tx_ms) >=
						      link->cfg.feedback_period_ms))) {
			link_plan_tx(link, &plan, leader ? ZLRS_PKT_DATA_L2F : ZLRS_PKT_DATA_F2L,
				     ZLRS_DATA_PREFIX_LEN, link_slot_freq_hz(link));
		}
		plan.arq_wait = waiting;
		return plan;
	}
	const uint32_t freq = leader && (uint32_t)(now_ms - link->state_ms) < ZLRS_SESS_CONFIRM_MS
				      ? sync_freq
				      : link_slot_freq_hz(link);
	link_plan_rx(link, &plan, freq, ZLRS_SYNC_SLOT_DATA, link->sync_word,
		     link->cfg.rx_timeout_ms);
	return plan;
}

const zlrs_radio_req_t *zlrs_link_finalize(zlrs_link_t *link, const zlrs_link_plan_t *plan)
{
	if (link->req_pending || plan->frame_index != link->frame_index ||
	    plan->slot != link->slot || plan->now_ms != link->slot_ms) {
		return NULL;
	}
	link->stats.tx_arq_wait_stalls += plan->arq_wait;
	if (plan->op == ZLRS_OP_IDLE) {
		return NULL;
	}
	if (plan->op == ZLRS_OP_RX) {
		return link_submit_rx(link, plan->radio.freq_hz, plan->radio.sync_slot,
				      plan->radio.sync_word, plan->radio.timeout_ms);
	}
	switch (plan->type) {
	case ZLRS_PKT_PAIR_REQ:
	case ZLRS_PKT_PAIR_RESP:
	case ZLRS_PKT_PAIR_CFM:
		return link_pair_finalize(link, plan->type);
	case ZLRS_PKT_SYNC:
		return link_send_sync(link, plan->now_ms);
	case ZLRS_PKT_SESS_RESP:
		return link_send_sess_resp(link);
	case ZLRS_PKT_SESS_CFM:
		return link_send_sess_cfm(link);
	case ZLRS_PKT_DATA_L2F:
	case ZLRS_PKT_DATA_F2L:
		return plan->radio.size ==
				       ZLRS_DATA_HEADROOM + zlrs_profile_tag_len(link->cfg.profile)
			       ? link_send_empty_data(link)
			       : link_send_data(link, plan->now_ms);
	default:
		return NULL;
	}
}

static void limit_data_mtu(zlrs_link_t *link)
{
	const uint16_t max_frame = link->cfg.automatic_band || link->cfg.mod.lora_sf != 0U
					   ? ZLRS_LORA_FRAME_MAX_LEN
					   : ZLRS_FRAME_MAX_LEN;
	const uint16_t mtu = max_frame - ZLRS_FRAME_HDR_LEN - ZLRS_DATA_PREFIX_LEN -
			     zlrs_profile_tag_len(link->cfg.profile);

	if (link->cfg.data_mtu > mtu) {
		link->cfg.data_mtu = mtu;
	}
}

void zlrs_link_init(zlrs_link_t *link, const zlrs_link_cfg_t *cfg,
		    bool (*random)(void *ctx, uint32_t *out), void *random_ctx,
		    const zlrs_link_events_t *events)
{
	memset(link, 0, sizeof(*link));
	link->cfg = *cfg;
	limit_data_mtu(link);
	link->random = random;
	link->random_ctx = random_ctx;
	if (events != NULL) {
		link->ev = *events;
	}

	link->tx_seq = 1U;
	link->pair_tx_seq = 1U;
	link_quality_reset(link);
	link->state = ZLRS_STATE_UNCONFIGURED;
	link->radio_acquiring = cfg->automatic_band;
}

void zlrs_link_set_power(zlrs_link_t *link, int8_t tx_power_dbm)
{
	link->cfg.tx_power_dbm = tx_power_dbm;
}

bool link_band_plan_valid(const zlrs_link_t *link, const zlrs_band_plan_t *plan,
			  uint32_t frame_index)
{
	const uint8_t phy =
		link->cfg.domain == ZLRS_DOMAIN_ISM2400 ? zlrs_mod_phy_id(&link->cfg.mod) : 0U;
	if (plan->domain != link->cfg.domain || plan->automatic != link->cfg.automatic_band ||
	    plan->phy != phy || plan->target_phy >= ZLRS_PHY_COUNT ||
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
	       (plan->target_subg != (link->cfg.domain != ZLRS_DOMAIN_ISM2400) ||
		(!plan->target_subg && plan->target_phy != phy));
}

void link_adopt_band_plan(zlrs_link_t *link, const zlrs_band_plan_t *plan)
{
	link->band_plan = *plan;
	link->band_announced = plan->switch_frame != 0U;
}

void zlrs_link_acquire_radio(zlrs_link_t *link, uint32_t now_ms)
{
	link->band_plan = (zlrs_band_plan_t){
		.domain = link->cfg.domain,
		.automatic = link->cfg.automatic_band,
		.phy = link->cfg.domain == ZLRS_DOMAIN_ISM2400 ? zlrs_mod_phy_id(&link->cfg.mod)
							       : 0U,
		.target_subg = link->cfg.domain != ZLRS_DOMAIN_ISM2400};
	link->band_announced = false;
	link->radio_acquiring = true;
	link->radio_sync_seen = false;
	link->sess_ack_due = false;
	link->last_sync_ms = now_ms - link->cfg.sync_period_ms;
}

bool zlrs_link_radio_captured(zlrs_link_t *link)
{
	if (link->cfg.role != ZLRS_ROLE_FOLLOWER || !link->radio_acquiring ||
	    !link->radio_sync_seen || !link->session_valid) {
		return false;
	}
	link->radio_acquiring = false;
	link->sess_ack_due = true;
	return true;
}

bool zlrs_link_plan_band(zlrs_link_t *link, bool target_subg, uint32_t switch_frame)
{
	return zlrs_link_plan_radio(link, target_subg, ZLRS_PHY_FLRC_2600, switch_frame);
}

bool zlrs_link_plan_radio(zlrs_link_t *link, bool target_subg, zlrs_phy_t target_phy,
			  uint32_t switch_frame)
{
	if ((unsigned int)target_phy >= ZLRS_PHY_AUTO_COUNT) {
		return false;
	}
	zlrs_band_plan_t plan = {.domain = link->cfg.domain,
				 .automatic = 1U,
				 .phy = link->cfg.domain == ZLRS_DOMAIN_ISM2400
						? zlrs_mod_phy_id(&link->cfg.mod)
						: 0U,
				 .target_phy = target_phy,
				 .target_subg = target_subg,
				 .switch_frame = switch_frame};
	if (link->req_pending || link->cfg.role != ZLRS_ROLE_LEADER || !link->session_valid ||
	    !link_band_plan_valid(link, &plan, link->frame_index) || switch_frame == 0U) {
		return false;
	}
	link->band_plan = plan;
	/* First announcement uses a periodic SYNC that the hopping peer expects. */
	link->band_announced = false;
	return true;
}

bool zlrs_link_reconfigure_radio(zlrs_link_t *link, const zlrs_link_cfg_t *cfg)
{
	if (link->req_pending || cfg->domain >= ZLRS_DOMAIN_COUNT || cfg->role != link->cfg.role ||
	    cfg->profile != link->cfg.profile || cfg->arq != link->cfg.arq ||
	    cfg->data_mtu != link->cfg.data_mtu ||
	    cfg->automatic_band != link->cfg.automatic_band) {
		return false;
	}
	uint8_t seed[4];
	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, "zlrs/fhss", NULL, 0U, seed, sizeof(seed));
	link->cfg = *cfg;
	zlrs_fhss_init(&link->fhss, cfg->domain, zlrs_get_le32(seed));
	zlrs_link_acquire_radio(link, link->last_rx_ms);
	/* Quality/power samples belong to the old RF path; replay/ARQ state does not. */
	link_quality_reset(link);
	return true;
}

bool zlrs_link_advance_slot(zlrs_link_t *link, uint8_t slot, uint32_t now_ms)
{
	if (link->req_pending || slot >= ZLRS_SLOT_COUNT) {
		return false;
	}
	link->slot = slot;
	link->slot_ms = now_ms;
	if (link->tx_seq == UINT32_MAX &&
	    (link->state == ZLRS_STATE_CONNECTED || link->state == ZLRS_STATE_SEARCHING)) {
		/* Rekey before exhausting the counter; keep all queued/unacknowledged bytes. */
		link->session_valid = false;
		link->session_pending = false;
		const bool was_searching = link->state == ZLRS_STATE_SEARCHING;
		link_set_state(link, ZLRS_STATE_SEARCHING, now_ms);
		if (was_searching && link->cfg.role == ZLRS_ROLE_LEADER) {
			link_roll_session(link, now_ms);
		}
		link->last_sync_ms = now_ms - link->cfg.sync_period_ms;
	}

	link->slot_idle = false;
	const bool pairing = link->state == ZLRS_STATE_PAIRING ||
			     (link->state == ZLRS_STATE_SEARCHING && link->pair_cfm_pending);
	if (slot == ZLRS_SLOT_DATA_A) {
		link->frame_index++;
	}
	if (pairing) {
		link->slot_idle = link_pair_advance(link);
	} else if (link->state == ZLRS_STATE_CONNECTED) {
		advance_hop(link);
	} else if (link->state == ZLRS_STATE_SEARCHING && link->cfg.role == ZLRS_ROLE_LEADER &&
		   !link->session_valid && !link->session_pending) {
		link_roll_session(link, now_ms);
	}
	return true;
}

const zlrs_radio_req_t *zlrs_link_slot(zlrs_link_t *link, uint8_t slot, uint32_t now_ms)
{
	if (!zlrs_link_advance_slot(link, slot, now_ms)) {
		return NULL;
	}
	const zlrs_link_plan_t plan = zlrs_link_plan(link);
	return zlrs_link_finalize(link, &plan);
}

void zlrs_link_skip_slot(zlrs_link_t *link, uint8_t slot)
{
	link->slot = slot;
	if (slot == ZLRS_SLOT_DATA_A) {
		link->frame_index++;
	}
	advance_hop(link);
}

void zlrs_link_tx_started(zlrs_link_t *link, uint32_t now_ms)
{
	if (!link->req_pending || !link->req_is_tx || link->req_started) {
		return;
	}
	link->req_started = true;
	const uint8_t type = link->req.buf[0] & ZLRS_FRAME_TYPE_MASK;
	if (type == ZLRS_PKT_PAIR_CFM) {
		link->pair_cfm_sent++;
	} else if (type == ZLRS_PKT_SESS_CFM && link->state == ZLRS_STATE_SEARCHING) {
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
	if (!link->req_pending) {
		return;
	}
	if (status == ZLRS_RADIO_OK) {
		zlrs_link_tx_started(link, now_ms);
	}
	const bool was_tx = link->req_is_tx;
	const uint8_t slot = (link->slot < ZLRS_SLOT_COUNT) ? link->slot : 0U;

	link->req_pending = false;

	if (status != ZLRS_RADIO_OK) {
		link->stats.radio_fail++;
		link->stats.slot_fail[slot]++;
		if (was_tx) {
			link->stats.tx_cancelled_frames += !link->req_started;
			link->stats.tx_failed_frames += link->req_started;
			link->pending_data_len = 0U;
			if (link->req_is_data && link->req_arq && link->req_data_len != 0U) {
				link->arq_local_failed = true;
			}
		}
		return;
	}

	if (was_tx) {
		link->stats.slot_tx[slot]++;
		link->stats.tx_frames++;
		link->stats.tx_bytes = (uint32_t)(link->stats.tx_bytes + link->req.size);
		if (link->req_is_data) {
			link->arq_ack_pending = false;
			link->sess_ack_due = false;
			link->last_feedback_tx_ms = now_ms;
			const uint16_t data_len = link->req_data_len;
			if (data_len == 0U) {
				link->stats.tx_ack_only_frames++;
			} else {
				link->stats.tx_data_frames++;
				link->stats.tx_data_ack_frames += link->req_arq;
				link->stats.tx_data_bytes += data_len;
				link->stats.tx_full_data_frames += data_len == link->req_data_mtu;
			}
		}
		if (link->pending_data_len != 0U) {
			link_release_data(link);
		}
		link->pending_data_len = 0U;
		return;
	}

	link->stats.slot_rx[slot]++;
	if (rx_len > 0U && rssi != 0) {
		if (link->stats.rssi_min == 0 || rssi < link->stats.rssi_min) {
			link->stats.rssi_min = rssi;
		}
		if (link->stats.rssi_max == 0 || rssi > link->stats.rssi_max) {
			link->stats.rssi_max = rssi;
		}
	}
	if (rx_len > 0U) {
		link->stats.slot_hit[slot]++;
		if (handle_rx(link, rx_len, now_ms) == 0) {
			link->stats.last_rssi = rssi;
			link->stats.last_snr = snr;
			link->accepted_rx_events++;
		}
	}
}

void zlrs_link_rx(zlrs_link_t *link, const uint8_t *buf, uint16_t len, int16_t rssi, int8_t snr,
		  uint32_t now_ms)
{
	if (buf == NULL || len == 0U || (link->req_pending && link->req_is_tx)) {
		return;
	}
	if (len > sizeof(link->rx_buf)) {
		link->stats.rx_bad_frame++;
		return;
	}

	memcpy(link->rx_buf, buf, len);
	link->req_pending = false;
	if (handle_rx(link, len, now_ms) == 0) {
		link->stats.last_rssi = rssi;
		link->stats.last_snr = snr;
		link->accepted_rx_events++;
	}
}

void zlrs_link_tick(zlrs_link_t *link, uint32_t now_ms)
{

	switch (link->state) {
	case ZLRS_STATE_UNCONFIGURED:
		if (link->configured) {
			link_set_state(link, ZLRS_STATE_UNPAIRED, now_ms);
		}
		break;

	case ZLRS_STATE_PAIRING:
		if ((uint32_t)(now_ms - link->state_ms) > link->cfg.pair_window_ms) {
			(void)zlrs_link_cancel_pair(link, now_ms);
		}
		break;

	case ZLRS_STATE_CONNECTED:
		/*
		 * Grace after entering CONNECTED: the peer may still be leaving SEARCHING (it needs to
		 * hear one of our confirms), and its first data frame cannot arrive until it does. Running
		 * the peer-loss check inside that window would roll a new session and restart the
		 * handshake before it ever had a chance to finish.
		 */
		if ((uint32_t)(now_ms - link->state_ms) <= ZLRS_SESS_CONFIRM_MS) {
			break;
		}
		if (link->cfg.peer_timeout_ms > 0U &&
		    (uint32_t)(now_ms - link->last_rx_ms) > link->cfg.peer_timeout_ms) {
			link->session_valid = false;
			link_set_state(link, ZLRS_STATE_SEARCHING, now_ms);
		}
		break;

	default:
		break;
	}
}

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
		.peer_rssi_dbm = link->peer_rssi_dbm,
		.peer_lq_percent = link->peer_lq_percent,
		.peer_lq_generation = link->peer_lq_generation,
		.last_peer_feedback_ms = link->last_peer_feedback_ms,
		.last_rx_ms = link->last_rx_ms,
		.accepted_rx_events = link->accepted_rx_events,
		.frame_index = link->frame_index,
		.frequency_hz = link->fhss.domain != NULL ? zlrs_fhss_current_hz(&link->fhss) : 0U,
		.hop_index = link->fhss.index,
		.band_plan = link->band_plan,
		.radio_acquiring = link->radio_acquiring,
		.radio_sync_seen = link->radio_sync_seen,
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
		.last_rx_frame = link->last_rx_frame,
		.sess_nonce_leader = link->sess_nonce_leader,
		.sess_nonce_follower = link->sess_nonce_follower,
		.sess_cfm_due = link->sess_cfm_due,
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
