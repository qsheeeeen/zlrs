/* SPDX-License-Identifier: Apache-2.0 */
#include "../support/peer.h"
/* Two-node DATA-only simulation: pairing, sessions, duplex ARQ and backpressure. */
#include "link/link.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string.h>
#include <tuple>
#include <vector>
#include <zephyr/ztest.h>
extern "C" {
#include "band.h"
#include "power.h"
}

typedef struct {
	zlrs_link_t link;
	uint32_t rng;
	uint8_t received[16384];
	uint16_t received_len;
	bool blocked;
	bool random_failed;
	bool force_random;
	uint32_t forced_random;
} node_t;

typedef struct {
	node_t leader;
	node_t follower;
	uint32_t now_ms;
	uint32_t frames;
	uint16_t drop_leader;
	uint16_t drop_follower;
	uint16_t fail_leader, fail_follower;
	bool duplicate_air, corrupt_air;
	bool drop_confirms_only;
	bool block_24_leader, block_24_follower, lose_band_plans;
	int16_t rssi_24;
	bool withhold_phase;
	uint64_t wire_hash;
	uint32_t wire_packets;
	uint8_t captured[ZLRS_FRAME_MAX_LEN];
	uint16_t captured_len;
} sim_t;

static bool random_word(void *ctx, uint32_t *out)
{
	node_t *const node = static_cast<node_t *>(ctx);
	if (node->random_failed) {
		return false;
	}
	if (node->force_random) {
		*out = node->forced_random;
		return true;
	}
	uint32_t x = node->rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*out = node->rng = x != 0U ? x : 1U;
	return true;
}

static bool receive_data(void *ctx, const uint8_t *data, uint16_t len)
{
	node_t *const node = static_cast<node_t *>(ctx);
	if (node->blocked || len > sizeof(node->received) - node->received_len) {
		return false;
	}
	memcpy(node->received + node->received_len, data, len);
	node->received_len += len;
	return true;
}

static void init_node(node_t *node, zlrs_role_t role, const uint8_t dev_id[4],
		      const uint8_t root[32])
{
	zlrs_link_cfg_t cfg = {.role = role,
			       .domain = ZLRS_DOMAIN_ISM2400,
			       .profile = ZLRS_PROFILE_P1,
			       .mod = {.rate_bps = 2600000U},
			       .tx_power_dbm = 10,
			       .pair_tx_power_dbm = -10,
			       .rx_timeout_ms = 2U,
			       .acq_timeout_ms = 8U,
			       .sync_period_ms = 100U,
			       .pair_period_ms = 100U,
			       .pair_window_ms = 30000U,
			       .data_mtu = 128U,
			       .arq = 1U,
			       .arq_timeout_ms = 30U,
			       .feedback_period_ms = 250U,
			       .hopping = 1U,
			       .resync_every_n = 20U};
	zlrs_link_events_t events = {.on_data = receive_data, .ctx = node};
	zlrs_link_init(&node->link, &cfg, random_word, node, &events);
	zlrs_link_set_dev_id(&node->link, dev_id);
	zlrs_link_set_root_key(&node->link, root, 0U);
}

static void sim_init(sim_t *sim)
{
	static const uint8_t leader_id[4] = {1, 2, 3, 4};
	static const uint8_t follower_id[4] = {5, 6, 7, 8};
	uint8_t root[32];
	memset(sim, 0, sizeof(*sim));
	sim->wire_hash = UINT64_C(14695981039346656037);
	for (uint8_t i = 0U; i < sizeof(root); i++) {
		root[i] = i + 1U;
	}
	sim->leader.rng = 0x12345678U;
	sim->follower.rng = 0x87654321U;
	init_node(&sim->leader, ZLRS_ROLE_LEADER, leader_id, root);
	init_node(&sim->follower, ZLRS_ROLE_FOLLOWER, follower_id, root);
}

static void record_wire(sim_t *sim, const zlrs_radio_req_t *req, bool from_leader)
{
	if (req == nullptr || !req->is_tx) {
		return;
	}
	sim->wire_packets++;
	sim->wire_hash = (sim->wire_hash ^ from_leader) * UINT64_C(1099511628211);
	for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
		sim->wire_hash = (sim->wire_hash ^ ((req->freq_hz >> shift) & 0xffU)) *
				 UINT64_C(1099511628211);
	}
	for (uint16_t i = 0U; i < req->size; ++i) {
		sim->wire_hash = (sim->wire_hash ^ req->buf[i]) * UINT64_C(1099511628211);
	}
	zlrs_frame_t frame;
	if (from_leader && zlrs_frame_decode(&frame, req->buf, req->size, ZLRS_TAG_LEN) &&
	    frame.type == ZLRS_PKT_DATA_L2F &&
	    req->size > ZLRS_FRAME_HDR_LEN + ZLRS_DATA_PREFIX_LEN + ZLRS_TAG_LEN) {
		memcpy(sim->captured, req->buf, req->size);
		sim->captured_len = req->size;
	}
}

static void sim_slot(sim_t *sim, uint8_t slot)
{
	zlrs_link_t *const leader = &sim->leader.link;
	zlrs_link_t *const follower = &sim->follower.link;
	const zlrs_radio_req_t *const a = zlrs_test_slot(leader, slot, sim->now_ms);
	const zlrs_radio_req_t *const b = zlrs_test_slot(follower, slot, sim->now_ms);
	record_wire(sim, a, true);
	record_wire(sim, b, false);
	if (a != nullptr && b != nullptr && a->is_tx != b->is_tx && a->freq_hz == b->freq_hz &&
	    a->mod.rate_bps == b->mod.rate_bps && a->mod.lora_sf == b->mod.lora_sf &&
	    memcmp(a->sync_word, b->sync_word, ZLRS_SYNC_WORD_LEN) == 0) {
		const zlrs_radio_req_t *const source = a->is_tx ? a : b;
		const zlrs_radio_req_t *const dest = a->is_tx ? b : a;
		zlrs_link_t *const sender = a->is_tx ? leader : follower;
		zlrs_link_t *const receiver = a->is_tx ? follower : leader;
		uint16_t *const drops = a->is_tx ? &sim->drop_leader : &sim->drop_follower;
		uint16_t *const failures = a->is_tx ? &sim->fail_leader : &sim->fail_follower;
		const bool failed = *failures != 0;
		if (failed) {
			(*failures)--;
		}
		zlrs_frame_t air;
		zassert_true(zlrs_frame_decode(&air, source->buf, source->size,
					       zlrs_profile_tag_len(sender->cfg.profile)));
		const bool on_24 = source->freq_hz > 1000000000U;
		const bool blocked =
			on_24 && (a->is_tx ? sim->block_24_leader : sim->block_24_follower);
		const bool lost_plan = sim->lose_band_plans && air.type == ZLRS_PKT_SYNC &&
				       air.body.sync.band_plan.switch_frame != 0U;
		if (failed || blocked || lost_plan ||
		    (*drops != 0U && (!sim->drop_confirms_only || air.type == ZLRS_PKT_SESS_CFM))) {
			if (*drops != 0U) {
				(*drops)--;
			}
			zlrs_link_complete(receiver, ZLRS_RADIO_OK, 0U, 0, 0, sim->now_ms);
		} else {
			memcpy(dest->buf, source->buf, source->size);
			if (sim->corrupt_air) {
				dest->buf[source->size - 1] ^= 0x80;
			}
			zlrs_link_complete(receiver, ZLRS_RADIO_OK, source->size,
					   on_24 && sim->rssi_24 ? sim->rssi_24 : -60, 0,
					   sim->now_ms);
			/* The fake radio has zero phase error unless this case withholds proof. */
			if (!sim->withhold_phase) {
				(void)zlrs_link_radio_captured(receiver);
			}
			if (sim->duplicate_air) {
				uint8_t duplicate[ZLRS_FRAME_MAX_LEN];
				memcpy(duplicate, dest->buf, source->size);
				zlrs_test_rx(receiver, duplicate, source->size, -60, 0,
					     sim->now_ms);
			}
		}
		zlrs_link_complete(sender, failed ? ZLRS_RADIO_FAIL : ZLRS_RADIO_OK, 0U, 0, 0,
				   sim->now_ms);
	} else {
		if (a != nullptr) {
			zlrs_link_complete(leader, ZLRS_RADIO_OK, 0U, 0, 0, sim->now_ms);
		}
		if (b != nullptr) {
			zlrs_link_complete(follower, ZLRS_RADIO_OK, 0U, 0, 0, sim->now_ms);
		}
	}
}

static void sim_frame(sim_t *sim)
{
	for (uint8_t slot = 0U; slot < ZLRS_SLOT_COUNT; slot++) {
		sim_slot(sim, slot);
		sim->now_ms += 3U;
	}
	zlrs_link_tick(&sim->leader.link, sim->now_ms);
	zlrs_link_tick(&sim->follower.link, sim->now_ms);
	sim->frames++;
}

static bool both_connected(const sim_t *sim)
{
	return zlrs_link_get_state(&sim->leader.link) == ZLRS_STATE_CONNECTED &&
	       zlrs_link_get_state(&sim->follower.link) == ZLRS_STATE_CONNECTED;
}

static sim_t sim;

struct BandLoss {
	unsigned int mode;
	const char *name;
};

static void finish_quality_window(sim_t *sim)
{
	const zlrs_link_stats_t *stats = zlrs_link_get_stats(&sim->follower.link);
	uint16_t previous = stats->lq_expected;
	for (unsigned int i = 0; i < 6000U; i++) {
		sim_frame(sim);
		if (previous != 0U && stats->lq_expected == 0U) {
			return;
		}
		previous = stats->lq_expected;
	}
	zassert_unreachable();
}

ZTEST(Link, test_QualityWindowBoundary)
{
	sim_init(&sim);
	zlrs_link_press(&sim.leader.link, sim.now_ms);
	zlrs_link_press(&sim.follower.link, sim.now_ms);
	for (unsigned int i = 0; i < 3000U && !both_connected(&sim); i++) {
		sim_frame(&sim);
	}
	zassert_true(both_connected(&sim));
	const zlrs_link_stats_t *stats = zlrs_link_get_stats(&sim.follower.link);
	finish_quality_window(&sim);
	zexpect_equal(stats->lq_percent, 100U);

	/* The first transmission in the next window is lost; its gap belongs to that window. */
	sim.drop_leader = 1U;
	for (unsigned int i = 0; i < 1000U && sim.drop_leader != 0U; i++) {
		sim_frame(&sim);
	}
	zexpect_true(sim.drop_leader == 0U && stats->lq_expected == 0U);
	for (unsigned int i = 0; i < 1000U && stats->lq_expected == 0U; i++) {
		sim_frame(&sim);
	}
	zexpect_true(stats->lq_rx == 1U && stats->lq_expected == 2U);
	finish_quality_window(&sim);
	zexpect_equal(stats->lq_percent, 99U);
	finish_quality_window(&sim);
	zexpect_equal(stats->lq_percent, 100U);
}

static void connect_saved(sim_t *sim, zlrs_profile_t profile);

static void inject_quality(sim_t *sim, uint8_t quality, uint32_t now)
{
	uint8_t wire[ZLRS_FRAME_HDR_LEN + ZLRS_DATA_PREFIX_LEN + ZLRS_TAG_LEN];
	zlrs_frame_t frame = {
		.type = ZLRS_PKT_DATA_F2L,
		.body = {.data = {.header = {.rssi_magnitude = 60U,
				  .lq_percent = static_cast<uint8_t>(quality & ZLRS_LQ_VALUE_MASK),
				  .lq_toggle = (quality & ZLRS_LQ_TOGGLE_MASK) != 0U}}},
		.seq = sim->follower.link.tx_seq++};
	zassert_equal(zlrs_test_encode(&frame, wire, sizeof(wire), sizeof(wire), ZLRS_TAG_LEN),
		      sizeof(wire));
	zlrs_tag(sim->follower.link.session_key_out, ZLRS_KEY_LEN, wire,
		 sizeof(wire) - ZLRS_TAG_LEN, wire + sizeof(wire) - ZLRS_TAG_LEN);
	zlrs_test_rx(&sim->leader.link, wire, sizeof(wire), -61, 0, now);
}

ZTEST(Link, test_QualityToggleSeparatesRepeatedFeedbackFromIdenticalNewWindows)
{
	sim_init(&sim);
	connect_saved(&sim, ZLRS_PROFILE_P1);
	inject_quality(&sim, ZLRS_LQ_UNAVAILABLE, sim.now_ms + 1);
	uint32_t generation = sim.leader.link.peer_lq_generation;
	inject_quality(&sim, 90, sim.now_ms + 2);
	zexpect_equal(sim.leader.link.peer_lq_generation, ++generation);
	inject_quality(&sim, 90, sim.now_ms + 3);
	zexpect_equal(sim.leader.link.peer_lq_generation, generation);
	inject_quality(&sim, 91, sim.now_ms + 4);
	zexpect_equal(sim.leader.link.peer_lq_percent, 90U);
	inject_quality(&sim, 90 | ZLRS_LQ_TOGGLE_MASK, sim.now_ms + 5);
	zexpect_equal(sim.leader.link.peer_lq_generation, ++generation);
	zexpect_equal(sim.leader.link.peer_lq_percent, 90U);
	inject_quality(&sim, 90, sim.now_ms + 6);
	zexpect_equal(sim.leader.link.peer_lq_generation, ++generation);
}

ZTEST(Link, test_PendingOrInvalidQualityStillRefreshesRssiAndFeedback)
{
	sim_init(&sim);
	connect_saved(&sim, ZLRS_PROFILE_P1);
	for (uint8_t quality : {127, 101, 255}) {
		inject_quality(&sim, quality, ++sim.now_ms);
		const auto info = zlrs_link_get_info(&sim.leader.link);
		zexpect_equal(info.peer_rssi_dbm, -60);
		zexpect_equal(info.last_peer_feedback_ms, sim.now_ms);
		zexpect_equal(info.peer_lq_percent, ZLRS_LQ_UNAVAILABLE);
	}
}

ZTEST(Link, test_AmbiguousMarkerAfterFeedbackGapEstablishesANewBaseline)
{
	sim_init(&sim);
	connect_saved(&sim, ZLRS_PROFILE_P1);
	sim.leader.link.cfg.lq_feedback_gap_ms = 500;
	inject_quality(&sim, 90, sim.now_ms + 1);
	const uint32_t generation = sim.leader.link.peer_lq_generation;
	inject_quality(&sim, 90, sim.now_ms + 500);
	zexpect_equal(sim.leader.link.peer_lq_generation, generation);
	inject_quality(&sim, 90, sim.now_ms + 1000);
	zexpect_equal(sim.leader.link.peer_lq_generation, generation + 2);
}

using PowerSymmetryParam = int;

static const PowerSymmetryParam PowerSymmetry_values[] = {
	-1,
	0,
	1,
};

static const char *PowerSymmetry_param_name(size_t index, const void *value)
{
	ARG_UNUSED(value);
	static const char *const names[] = {"Ideal", "LeaderLoss", "FollowerLoss"};
	return names[index];
}

static const ztest_param_values PowerSymmetry_params = {
	.values = PowerSymmetry_values,
	.count = ARRAY_SIZE(PowerSymmetry_values),
	.elem_size = sizeof(PowerSymmetry_values[0]),
	.name_cb = PowerSymmetry_param_name,
};

ZTEST_P(PowerSymmetry, test_StrongDuplexLinkKeepsSavingWithOneOccasionalLostPacket)
{
	const auto param = ZTEST_GET_PARAM(PowerSymmetryParam);
	sim_init(&sim);
	zlrs_link_press(&sim.leader.link, sim.now_ms);
	zlrs_link_press(&sim.follower.link, sim.now_ms);
	for (unsigned int i = 0U; i < 3000U && !both_connected(&sim); i++) {
		sim_frame(&sim);
	}
	zassert_true(both_connected(&sim));
	zlrs_power_t powers[2];
	zlrs_link_t *const nodes[] = {&sim.leader.link, &sim.follower.link};
	for (unsigned int i = 0U; i < 2U; i++) {
		zlrs_power_init(&powers[i], 12, -17, sim.now_ms);
	}
	zlrs_link_t *const lossy_sender = param == 0 ? nodes[0] : nodes[1];
	uint32_t last_loss_seq = lossy_sender->tx_seq;
	const uint32_t finish_ms = sim.now_ms + 130000U;
	unsigned int losses = 0U;
	bool saw_99 = false;
	while (sim.now_ms < finish_ms) {
		/* One missing packet per 130 attempts: each completed quality window
		 * remains at least 99%, and the received RSSI is -60 dBm in both directions.
		 */
		if (param >= 0 && lossy_sender->tx_seq - last_loss_seq >= 130U) {
			last_loss_seq = lossy_sender->tx_seq;
			(param == 0 ? sim.drop_leader : sim.drop_follower) = 1U;
			losses++;
		}
		sim_frame(&sim);
		for (unsigned int i = 0U; i < 2U; i++) {
			const zlrs_link_info_t info = zlrs_link_get_info(nodes[i]);
			saw_99 |= info.peer_lq_percent == 99U;
			zlrs_link_set_power(
				nodes[i], ([&] {
					zlrs_power_observation_t o = {
						.now_ms = sim.now_ms,
						.feedback_ms = info.last_peer_feedback_ms,
						.lq_generation = info.peer_lq_generation,
						.rssi_dbm = info.peer_rssi_dbm,
						.rssi_floor_dbm = -89,
						.lq_percent = info.peer_lq_percent};
					return zlrs_power_update(&powers[i], &o);
				})());
		}
	}
	if (param >= 0) {
		zassert_true((losses) >= (2U));
		zassert_true(saw_99);
	} else {
		zexpect_equal(losses, 0U);
		zexpect_false(saw_99);
	}
	zassert_true(both_connected(&sim));
	zexpect_true((powers[0].lowers) >= (2U));
	zexpect_true((powers[1].lowers) >= (2U));
	zexpect_true((powers[0].current_dbm) <= (6));
	zexpect_true((powers[1].current_dbm) <= (6));
	zexpect_true((std::abs(powers[0].current_dbm - powers[1].current_dbm)) <= (3));
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, PowerSymmetry,
			       test_StrongDuplexLinkKeepsSavingWithOneOccasionalLostPacket,
			       PowerSymmetry_params);

ZTEST(Link, test_DataSequenceWrap)
{
	sim_init(&sim);
	sim.leader.link.cfg.data_mtu = 1U;
	sim.follower.link.cfg.data_mtu = 1U;
	zlrs_link_press(&sim.leader.link, sim.now_ms);
	zlrs_link_press(&sim.follower.link, sim.now_ms);
	for (unsigned int i = 0; i < 3000U && !both_connected(&sim); i++) {
		sim_frame(&sim);
	}
	zassert_true(both_connected(&sim));
	for (unsigned int i = 0U; i < 512U; i++) {
		const uint8_t up = (uint8_t)i;
		const uint8_t down = (uint8_t)(i ^ 0x5aU);
		zexpect_equal(zlrs_test_write(&sim.leader.link, &up, 1U), 1U);
		zexpect_equal(zlrs_test_write(&sim.follower.link, &down, 1U), 1U);
		/* Drop reverse ACKs, and hold the receiver at sequence wrap boundaries. */
		if (i % 31U == 0U) {
			sim.drop_follower = 2U;
		}
		const bool blocked = (i & 127U) == 127U;
		sim.follower.blocked = blocked;
		for (unsigned int wait = 0U; wait < 1000U; wait++) {
			if (wait == 12U) {
				sim.follower.blocked = false;
			}
			sim_frame(&sim);
			if (sim.leader.link.queue_count == 0U &&
			    sim.follower.link.queue_count == 0U &&
			    sim.leader.received_len == i + 1U &&
			    sim.follower.received_len == i + 1U) {
				break;
			}
		}
		zexpect_true(sim.leader.received_len == i + 1U &&
			     sim.follower.received_len == i + 1U);
		zexpect_true(sim.follower.received[i] == up && sim.leader.received[i] == down);
		zexpect_equal(sim.leader.link.arq_seq, ((i + 1U) & ZLRS_DATA_SEQ_MASK));
		zexpect_equal(sim.follower.link.arq_seq, ((i + 1U) & ZLRS_DATA_SEQ_MASK));
	}
	zexpect_true((sim.follower.link.stats.rx_dup) > (0U));
	zexpect_true((sim.leader.link.stats.tx_retries) > (0U));
}

/* Exercise the production RX entry with correctly authenticated attack packets. */
static void inject(node_t *receiver, const zlrs_frame_t *packet, uint16_t body_len,
		   const uint8_t *key, uint32_t now_ms, bool bad_tag)
{
	uint8_t bytes[ZLRS_FRAME_MAX_LEN];
	const uint8_t tag_len = zlrs_profile_tag_len(receiver->link.cfg.profile);
	const uint16_t size = zlrs_frame_len(body_len, tag_len);
	zassert_equal(zlrs_test_encode(packet, bytes, sizeof(bytes), size, tag_len), size);
	if (tag_len != 0U) {
		zlrs_tag(key, ZLRS_KEY_LEN, bytes, size - tag_len, bytes + size - tag_len);
		if (bad_tag) {
			bytes[size - 1U] ^= 0x80U;
		}
	}
	zlrs_test_rx(&receiver->link, bytes, size, -60, 0, now_ms);
}

static void connect_saved(sim_t *sim, zlrs_profile_t profile)
{
	const uint8_t salt[ZLRS_PAIR_SALT_LEN] = {1, 2, 3, 4, 5, 6, 7, 8};
	sim->leader.link.cfg.profile = profile;
	sim->follower.link.cfg.profile = profile;
	zlrs_link_set_peer(&sim->leader.link, sim->follower.link.dev_id, salt, sim->now_ms);
	zlrs_link_set_peer(&sim->follower.link, sim->leader.link.dev_id, salt, sim->now_ms);
	for (unsigned int i = 0U; i < 3000U && !both_connected(sim); i++) {
		sim_frame(sim);
	}
	zassert_true(both_connected(sim));
}

using LinkProfilesParam = zlrs_profile_t;

static const LinkProfilesParam LinkProfiles_values[] = {
	ZLRS_PROFILE_P0,
	ZLRS_PROFILE_P1,
};

static const char *LinkProfiles_param_name(size_t index, const void *value)
{
	ARG_UNUSED(value);
	static const char *const names[] = {"P0", "P1"};
	return names[index];
}

static const ztest_param_values LinkProfiles_params = {
	.values = LinkProfiles_values,
	.count = ARRAY_SIZE(LinkProfiles_values),
	.elem_size = sizeof(LinkProfiles_values[0]),
	.name_cb = LinkProfiles_param_name,
};

ZTEST_P(LinkProfiles, test_OversizedInputPreservesStateAndAllowsValidRetry)
{
	const auto param = ZTEST_GET_PARAM(LinkProfilesParam);
	sim_init(&sim);
	connect_saved(&sim, param);
	auto &receiver = sim.follower;
	auto &link = receiver.link;
	const uint8_t tag_len = zlrs_profile_tag_len(param);
	zlrs_frame_t frame = {.type = ZLRS_PKT_DATA_L2F, .seq = link.last_rx_seq + 1U};
	uint8_t payload[ZLRS_DATA_MAX_LEN];
	memset(payload, 0xa5, sizeof(payload));
	frame.body.data.bytes = payload;
	uint8_t bytes[ZLRS_FRAME_MAX_LEN + 1U];
	zassert_equal(zlrs_test_encode(&frame, bytes, sizeof(bytes), ZLRS_FRAME_MAX_LEN, tag_len),
		      ZLRS_FRAME_MAX_LEN);
	if (tag_len != 0U) {
		zlrs_tag(link.session_key_in, ZLRS_KEY_LEN, bytes, ZLRS_FRAME_MAX_LEN - tag_len,
			 bytes + ZLRS_FRAME_MAX_LEN - tag_len);
	}
	bytes[ZLRS_FRAME_MAX_LEN] = 0x5a;
	const uint32_t baseline = link.last_rx_seq;
	const uint32_t accepted = link.accepted_rx_events;
	zlrs_test_rx(&link, bytes, sizeof(bytes), -60, 0, sim.now_ms);
	zexpect_equal(receiver.received_len, 0U);
	zexpect_equal(link.last_rx_seq, baseline);
	zexpect_equal(link.accepted_rx_events, accepted);
	zexpect_false(link.arq_rx_started);
	/* Rejecting the extra byte must leave the original sequence available. */
	zlrs_test_rx(&link, bytes, ZLRS_FRAME_MAX_LEN, -60, 0, sim.now_ms);
	zexpect_equal(receiver.received_len,
		      ZLRS_FRAME_MAX_LEN - ZLRS_FRAME_HDR_LEN - ZLRS_DATA_PREFIX_LEN - tag_len);
	zexpect_equal(link.last_rx_seq, frame.seq);
	zexpect_equal(link.accepted_rx_events, accepted + 1U);
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, LinkProfiles,
			       test_OversizedInputPreservesStateAndAllowsValidRetry,
			       LinkProfiles_params);

ZTEST(Link, test_ReplayAndCounters)
{
	sim_init(&sim);
	connect_saved(&sim, ZLRS_PROFILE_P1);
	zlrs_link_t *leader = &sim.leader.link;
	zlrs_link_t *follower = &sim.follower.link;
	const uint8_t byte = 0x5a;
	zexpect_equal(zlrs_test_write(leader, &byte, 1U), 1U);
	leader->arq_used = 1U;
	leader->arq_launched = true;
	leader->arq_seq = 0U;
	leader->last_rx_seq = 32770U;
	zlrs_frame_t frame = {.type = ZLRS_PKT_DATA_F2L,
			      .body = {.data = {.header = {.ack = 1U, .lq_percent = 100U}}},
			      .seq = 1U};
	inject(&sim.leader, &frame, ZLRS_DATA_PREFIX_LEN, leader->session_key_in, sim.now_ms,
	       false);
	zexpect_true(leader->last_rx_seq == 32770U && leader->queue_count == 1U &&
		     leader->arq_used == 1U);
	frame.seq = UINT32_MAX;
	inject(&sim.leader, &frame, ZLRS_DATA_PREFIX_LEN, leader->session_key_in, sim.now_ms, true);
	zexpect_true(leader->last_rx_seq == 32770U && leader->queue_count == 1U);
	frame.type = ZLRS_PKT_DATA_L2F;
	inject(&sim.leader, &frame, ZLRS_DATA_PREFIX_LEN, leader->session_key_in, sim.now_ms,
	       false);
	zexpect_true(leader->last_rx_seq == 32770U && leader->queue_count == 1U);
	frame.type = ZLRS_PKT_DATA_F2L;
	frame.seq = 32771U;
	inject(&sim.leader, &frame, ZLRS_DATA_PREFIX_LEN, leader->session_key_in, sim.now_ms,
	       false);
	zexpect_true(leader->last_rx_seq == 32771U && leader->queue_count == 0U &&
		     leader->arq_used == 0U);
	const uint32_t increasing[] = {65535U, 65536U, 0x7fffffffU, 0x80000000U, UINT32_MAX};
	for (unsigned int i = 0U; i < sizeof(increasing) / sizeof(increasing[0]); i++) {
		frame.seq = increasing[i];
		inject(&sim.leader, &frame, ZLRS_DATA_PREFIX_LEN, leader->session_key_in,
		       sim.now_ms, false);
		zexpect_equal(leader->last_rx_seq, increasing[i]);
	}
	frame.seq = 1U;
	inject(&sim.leader, &frame, ZLRS_DATA_PREFIX_LEN, leader->session_key_in, sim.now_ms,
	       false);
	zexpect_equal(leader->last_rx_seq, UINT32_MAX);

	/* Correct tags do not authorize wrong nonces, versions or out-of-domain hops. */
	const uint32_t baseline = follower->last_rx_seq;
	const uint32_t last_ms = follower->last_rx_ms;
	const uint32_t index = follower->frame_index;
	frame = zlrs_frame_t{.type = ZLRS_PKT_SYNC,
			     .body = {.sync = {.profile = 1U,
					       .version = 9U,
					       .hop_index = 0U,
					       .nonce_leader = follower->sess_nonce_leader,
					       .frame_index = index + 100U,
					       .band_plan = {.domain = ZLRS_DOMAIN_ISM2400}}},
			     .seq = UINT32_MAX};
	inject(&sim.follower, &frame, ZLRS_SYNC_PAYLOAD_LEN, follower->link_auth_in, sim.now_ms,
	       false);
	zexpect_true(follower->last_rx_seq == baseline && follower->frame_index == index);
	frame.body.sync.version = ZLRS_PROTO_VERSION;
	frame.body.sync.profile = 0U;
	inject(&sim.follower, &frame, ZLRS_SYNC_PAYLOAD_LEN, follower->link_auth_in, sim.now_ms,
	       false);
	zexpect_true(follower->last_rx_seq == baseline && follower->last_rx_ms == last_ms);
	frame.body.sync.profile = 1U;
	frame.body.sync.hop_index = follower->fhss.sequence_count;
	inject(&sim.follower, &frame, ZLRS_SYNC_PAYLOAD_LEN, follower->link_auth_in, sim.now_ms,
	       false);
	zexpect_true(follower->last_rx_seq == baseline && follower->frame_index == index);
	frame = zlrs_frame_t{
		.type = ZLRS_PKT_SESS_CFM,
		.body = {.sess_cfm = {.nonce_follower = follower->sess_nonce_follower ^ 1U,
				      .hop_index = 0U,
				      .frame_index = index + 100U,
				      .band_plan = {.domain = ZLRS_DOMAIN_ISM2400}}},
		.seq = UINT32_MAX};
	inject(&sim.follower, &frame, ZLRS_SESS_CFM_PAYLOAD_LEN, follower->session_key_in,
	       sim.now_ms, false);
	zexpect_true(follower->last_rx_seq == baseline && follower->last_rx_ms == last_ms);
	zexpect_equal(zlrs_test_write(follower, &byte, 1U), 1U);
	follower->arq_used = 1U;
	follower->arq_seq = 9U;
	frame.seq = baseline + 1U;
	frame.body.sess_cfm.nonce_follower = follower->sess_nonce_follower;
	inject(&sim.follower, &frame, ZLRS_SESS_CFM_PAYLOAD_LEN, follower->session_key_in,
	       sim.now_ms + 1U, false);
	zexpect_true(follower->last_rx_seq == baseline + 1U &&
		     follower->frame_index == index + 100U);
	zexpect_true(follower->queue_count == 1U && follower->arq_used == 1U &&
		     follower->arq_seq == 9U);
	inject(&sim.follower, &frame, ZLRS_SESS_CFM_PAYLOAD_LEN, follower->session_key_in,
	       sim.now_ms + 2U, false);
	zexpect_equal(follower->last_rx_ms, sim.now_ms + 1U);
}

ZTEST(Link, test_ResponseBinding)
{
	sim_init(&sim);
	connect_saved(&sim, ZLRS_PROFILE_P1);
	zlrs_link_t *leader = &sim.leader.link;
	const uint32_t completed_nonce = leader->sess_nonce_leader;
	uint8_t old_key[ZLRS_KEY_LEN];
	memcpy(old_key, leader->session_key_in, sizeof(old_key));
	const uint32_t old_baseline = leader->last_rx_seq;
	zlrs_frame_t frame = {
		.type = ZLRS_PKT_SESS_RESP,
		.body = {.sess_resp = {.nonce_leader = completed_nonce,
				       .nonce_follower = leader->sess_nonce_follower ^ 1U}},
		.seq = UINT32_MAX};
	leader->last_rx_ms = sim.now_ms;
	inject(&sim.leader, &frame, ZLRS_SESS_RESP_PAYLOAD_LEN, leader->link_auth_in, sim.now_ms,
	       false);
	zexpect_true(leader->state == ZLRS_STATE_CONNECTED && leader->last_rx_seq == old_baseline);
	zexpect_equal(memcmp(leader->session_key_in, old_key, sizeof(old_key)), 0);
	/* Once silent, that hint can request a fresh challenge, never adopt the old answer. */
	sim.now_ms += leader->cfg.sync_period_ms + 1U;
	inject(&sim.leader, &frame, ZLRS_SESS_RESP_PAYLOAD_LEN, leader->link_auth_in, sim.now_ms,
	       false);
	zexpect_true(leader->state == ZLRS_STATE_SEARCHING && leader->session_pending &&
		     !leader->session_valid);
	zexpect_true(leader->sess_nonce_leader != completed_nonce && leader->sess_rx_seq == 0U);
	const uint32_t fresh = leader->sess_nonce_leader;
	inject(&sim.leader, &frame, ZLRS_SESS_RESP_PAYLOAD_LEN, leader->link_auth_in, sim.now_ms,
	       false);
	zexpect_true(leader->sess_nonce_leader == fresh && leader->sess_rx_seq == 0U &&
		     !leader->session_valid);
	frame.body.sess_resp.nonce_leader = fresh;
	frame.seq = 1U;
	inject(&sim.leader, &frame, ZLRS_SESS_RESP_PAYLOAD_LEN, leader->link_auth_in, sim.now_ms,
	       true);
	zexpect_true(leader->sess_rx_seq == 0U && !leader->session_valid);
	inject(&sim.leader, &frame, ZLRS_SESS_RESP_PAYLOAD_LEN, leader->link_auth_in, sim.now_ms,
	       false);
	zexpect_true(leader->session_valid && leader->sess_rx_seq == 1U &&
		     leader->last_rx_seq == 1U);
	memcpy(old_key, leader->session_key_in, sizeof(old_key));
	leader->arq_seq = 7U;
	leader->arq_used = 1U;
	frame.seq = 2U;
	inject(&sim.leader, &frame, ZLRS_SESS_RESP_PAYLOAD_LEN, leader->link_auth_in, sim.now_ms,
	       false);
	zexpect_true(leader->arq_seq == 7U && leader->arq_used == 1U && leader->last_rx_seq == 2U);
	zexpect_equal(memcmp(leader->session_key_in, old_key, sizeof(old_key)), 0);
	frame.body.sess_resp.nonce_follower ^= 1U;
	frame.seq = UINT32_MAX;
	inject(&sim.leader, &frame, ZLRS_SESS_RESP_PAYLOAD_LEN, leader->link_auth_in, sim.now_ms,
	       false);
	zexpect_true(leader->last_rx_seq == 2U && leader->arq_seq == 7U && leader->arq_used == 1U);
	zexpect_equal(memcmp(leader->session_key_in, old_key, sizeof(old_key)), 0);
}

ZTEST(Link, test_PairingContext)
{
	sim_init(&sim);
	zlrs_link_t *follower = &sim.follower.link;
	zlrs_link_press(follower, 0U);
	zlrs_frame_t frame = {.type = ZLRS_PKT_PAIR_REQ,
			      .body = {.pair_req = {.dev_a = {1, 2, 3, 4}, .nonce_a = 0U}},
			      .seq = 42U};
	inject(&sim.follower, &frame, ZLRS_PAIR_REQ_PAYLOAD_LEN, follower->auth_key, 1U, false);
	zexpect_true(follower->pair_request_valid && follower->pair_wait_cfm &&
		     follower->pair_nonce == 0U);
	const uint32_t nonce_b = follower->pair_nonce_b;
	frame.seq++;
	inject(&sim.follower, &frame, ZLRS_PAIR_REQ_PAYLOAD_LEN, follower->auth_key, 2U, false);
	zexpect_true(follower->pair_nonce_b == nonce_b && follower->pair_rx_seq == 43U);
	frame.seq--;
	inject(&sim.follower, &frame, ZLRS_PAIR_REQ_PAYLOAD_LEN, follower->auth_key, 3U, false);
	zexpect_true(follower->pair_rx_seq == 43U && follower->pair_nonce_b == nonce_b);
	/* The same nonce from another ID is a different request, not a retransmission. */
	frame.body.pair_req.dev_a[0] = 9U;
	frame.seq = 1U;
	inject(&sim.follower, &frame, ZLRS_PAIR_REQ_PAYLOAD_LEN, follower->auth_key, 4U, false);
	zexpect_true(follower->pair_peer_id[0] == 9U && follower->pair_nonce_b != nonce_b &&
		     follower->pair_rx_seq == 1U);
	frame = zlrs_frame_t{.type = ZLRS_PKT_PAIR_CFM,
			     .body = {.pair = {.dev_a = {9, 2, 3, 4},
					       .dev_b = {5, 6, 7, 8},
					       .nonce_a = 0U,
					       .nonce_b = follower->pair_nonce_b ^ 1U}},
			     .seq = UINT32_MAX};
	inject(&sim.follower, &frame, ZLRS_PAIR_PAYLOAD_LEN, follower->auth_key, 5U, false);
	zexpect_false(follower->paired && follower->pair_rx_seq == 1U);
	frame.body.pair.nonce_b = follower->pair_nonce_b;
	frame.seq = 2U;
	inject(&sim.follower, &frame, ZLRS_PAIR_PAYLOAD_LEN, follower->auth_key, 6U, false);
	zexpect_true(follower->paired && follower->state == ZLRS_STATE_SEARCHING &&
		     follower->pair_rx_seq == 2U);
	follower->session_pending = true;
	follower->sess_rx_seq = 77U;
	follower->fhss.index = 100U;
	frame.seq = 3U;
	inject(&sim.follower, &frame, ZLRS_PAIR_PAYLOAD_LEN, follower->auth_key, 7U, false);
	zexpect_true(follower->session_pending && follower->sess_rx_seq == 77U &&
		     follower->fhss.index == 100U);
	frame.type = ZLRS_PKT_PAIR_REQ;
	frame.body.pair_req = zlrs_pair_request_frame_t{.dev_a = {1, 2, 3, 4}};
	frame.seq = 4U;
	inject(&sim.follower, &frame, ZLRS_PAIR_REQ_PAYLOAD_LEN, follower->auth_key, 8U, false);
	zexpect_true(follower->peer_id[0] == 9U && follower->pair_rx_seq == 3U);
	/* Pairing a reflected/local identity is never permitted. */
	zlrs_link_press(follower, 9U);
	memcpy(frame.body.pair_req.dev_a, follower->dev_id, ZLRS_DEV_ID_LEN);
	inject(&sim.follower, &frame, ZLRS_PAIR_REQ_PAYLOAD_LEN, follower->auth_key, 10U, false);
	zexpect_false(follower->pair_request_valid && follower->pair_rx_seq == 0U);
}

enum class PairEnd {
	Cancel,
	Timeout,
	Confirm
};

using PairingCommitParam = std::tuple<zlrs_role_t, PairEnd>;

static const PairingCommitParam PairingCommit_values[] = {
	std::make_tuple(ZLRS_ROLE_LEADER, PairEnd::Cancel),
	std::make_tuple(ZLRS_ROLE_LEADER, PairEnd::Timeout),
	std::make_tuple(ZLRS_ROLE_LEADER, PairEnd::Confirm),
	std::make_tuple(ZLRS_ROLE_FOLLOWER, PairEnd::Cancel),
	std::make_tuple(ZLRS_ROLE_FOLLOWER, PairEnd::Timeout),
	std::make_tuple(ZLRS_ROLE_FOLLOWER, PairEnd::Confirm),
};

static const char *PairingCommit_param_name(size_t index, const void *value)
{
	ARG_UNUSED(value);
	static const char *const names[] = {"LeaderCancel",   "LeaderTimeout",   "LeaderConfirm",
					    "FollowerCancel", "FollowerTimeout", "FollowerConfirm"};
	return names[index];
}

static const ztest_param_values PairingCommit_params = {
	.values = PairingCommit_values,
	.count = ARRAY_SIZE(PairingCommit_values),
	.elem_size = sizeof(PairingCommit_values[0]),
	.name_cb = PairingCommit_param_name,
};

ZTEST_P(PairingCommit, test_SavedRecordChangesOnlyAfterConfirmation)
{
	const auto param = ZTEST_GET_PARAM(PairingCommitParam);
	sim_init(&sim);
	const auto [role, end] = param;
	node_t *node = role == ZLRS_ROLE_LEADER ? &sim.leader : &sim.follower;
	node_t *old_peer = role == ZLRS_ROLE_LEADER ? &sim.follower : &sim.leader;
	zlrs_link_t *link = &node->link;
	const uint8_t salt[ZLRS_PAIR_SALT_LEN] = {1, 2, 3, 4, 5, 6, 7, 8};
	const uint8_t candidate[ZLRS_DEV_ID_LEN] = {9, 10, 11, 12};
	zlrs_link_set_peer(link, old_peer->link.dev_id, salt, 0);
	zlrs_link_set_peer(&old_peer->link, link->dev_id, salt, 0);
	uint8_t old_key[ZLRS_KEY_LEN];
	memcpy(old_key, link->pair_key, sizeof(old_key));
	zlrs_link_press(link, 1);
	zlrs_frame_t frame = {.type = ZLRS_PKT_PAIR_REQ, .seq = 1};
	if (role == ZLRS_ROLE_LEADER) {
		frame.type = ZLRS_PKT_PAIR_RESP;
		memcpy(frame.body.pair.dev_a, link->dev_id, ZLRS_DEV_ID_LEN);
		memcpy(frame.body.pair.dev_b, candidate, ZLRS_DEV_ID_LEN);
		frame.body.pair.nonce_a = link->pair_nonce;
		frame.body.pair.nonce_b = 42;
	} else {
		memcpy(frame.body.pair_req.dev_a, candidate, ZLRS_DEV_ID_LEN);
		frame.body.pair_req.nonce_a = 42;
	}
	const uint16_t payload =
		role == ZLRS_ROLE_LEADER ? ZLRS_PAIR_PAYLOAD_LEN : ZLRS_PAIR_REQ_PAYLOAD_LEN;
	inject(node, &frame, payload, link->auth_key, 2, false);
	zassert_true(link->pair_cfm_pending || link->pair_wait_cfm);
	uint8_t saved_id[ZLRS_DEV_ID_LEN], saved_salt[ZLRS_PAIR_SALT_LEN];
	zassert_true(zlrs_link_get_pairing(link, saved_id, saved_salt));
	zexpect_equal(memcmp(saved_id, old_peer->link.dev_id, sizeof(saved_id)), 0);
	zexpect_equal(memcmp(saved_salt, salt, sizeof(salt)), 0);
	zexpect_equal(memcmp(link->pair_key, old_key, sizeof(old_key)), 0);
	zexpect_equal(memcmp(zlrs_link_get_info(link).peer_id, saved_id, sizeof(saved_id)), 0);
	const uint32_t nonce_a = link->pair_nonce;
	const uint32_t nonce_b = link->pair_nonce_b;
	if (end == PairEnd::Cancel) {
		zassert_true(zlrs_link_cancel_pair(link, 3));
	} else if (end == PairEnd::Timeout) {
		zlrs_link_tick(link, link->cfg.pair_window_ms + 2);
	} else if (role == ZLRS_ROLE_FOLLOWER) {
		frame = zlrs_frame_t{.type = ZLRS_PKT_PAIR_CFM, .seq = 2};
		memcpy(frame.body.pair.dev_a, candidate, ZLRS_DEV_ID_LEN);
		memcpy(frame.body.pair.dev_b, link->dev_id, ZLRS_DEV_ID_LEN);
		frame.body.pair.nonce_a = nonce_a;
		frame.body.pair.nonce_b = nonce_b;

		inject(node, &frame, ZLRS_PAIR_PAYLOAD_LEN, link->auth_key, 3, false);
	} else {
		for (unsigned int i = 1; i <= ZLRS_PAIR_CFM_REPEATS + 1; i++) {
			const auto *req = zlrs_test_slot(link, ZLRS_SLOT_DATA_A, i * 100);
			if (req) {
				zlrs_link_complete(link, ZLRS_RADIO_OK, 0, 0, 0, i * 100);
			}
		}
	}
	zassert_equal(zlrs_link_get_state(link), ZLRS_STATE_SEARCHING);
	zassert_true(zlrs_link_get_pairing(link, saved_id, saved_salt));
	if (end == PairEnd::Confirm) {
		zexpect_equal(memcmp(saved_id, candidate, sizeof(saved_id)), 0);
		uint8_t expected[ZLRS_PAIR_SALT_LEN];
		for (unsigned int i = 0; i < 4; i++) {
			expected[i] = (uint8_t)(nonce_a >> (8 * i));
			expected[i + 4] = (uint8_t)(nonce_b >> (8 * i));
		}
		zexpect_equal(memcmp(saved_salt, expected, sizeof(expected)), 0);
	} else {
		zexpect_equal(memcmp(saved_id, old_peer->link.dev_id, sizeof(saved_id)), 0);
		zexpect_equal(memcmp(saved_salt, salt, sizeof(salt)), 0);
		/* Late packets from the abandoned attempt cannot commit a new record. */
		frame.seq++;

		inject(node, &frame, payload, link->auth_key, 31000, false);
		zassert_true(zlrs_link_get_pairing(link, saved_id, saved_salt));
		zexpect_equal(memcmp(saved_id, old_peer->link.dev_id, sizeof(saved_id)), 0);
		zexpect_false(link->pair_cfm_pending || link->pair_wait_cfm);
		for (unsigned int i = 0; i < 3000 && !both_connected(&sim); i++) {
			sim_frame(&sim);
		}
		zexpect_true(both_connected(&sim));
	}
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, PairingCommit, test_SavedRecordChangesOnlyAfterConfirmation,
			       PairingCommit_params);

using QueueFailureParam = std::tuple<zlrs_role_t, bool>;

static const QueueFailureParam QueueFailure_values[] = {
	std::make_tuple(ZLRS_ROLE_LEADER, false),
	std::make_tuple(ZLRS_ROLE_LEADER, true),
	std::make_tuple(ZLRS_ROLE_FOLLOWER, false),
	std::make_tuple(ZLRS_ROLE_FOLLOWER, true),
};

static const char *QueueFailure_param_name(size_t index, const void *value)
{
	ARG_UNUSED(value);
	static const char *const names[] = {"LeaderBestEffort", "LeaderARQ", "FollowerBestEffort",
					    "FollowerARQ"};
	return names[index];
}

static const ztest_param_values QueueFailure_params = {
	.values = QueueFailure_values,
	.count = ARRAY_SIZE(QueueFailure_values),
	.elem_size = sizeof(QueueFailure_values[0]),
	.name_cb = QueueFailure_param_name,
};

ZTEST_P(QueueFailure, test_FailedTransactionsKeepTheQueuedBytes)
{
	const auto param = ZTEST_GET_PARAM(QueueFailureParam);
	sim_init(&sim);
	connect_saved(&sim, ZLRS_PROFILE_P1);
	const auto [role, arq] = param;
	zlrs_link_t *link = role == ZLRS_ROLE_LEADER ? &sim.leader.link : &sim.follower.link;
	link->cfg.arq = arq;
	link->cfg.resync_every_n = 0;
	link->sess_cfm_due = false;
	link->queue_head = ZLRS_LINK_TX_PACKETS - 2;
	const uint8_t bytes[] = {1, 2, 3, 4, 5};
	zassert_equal(zlrs_test_write(link, bytes, sizeof(bytes)), sizeof(bytes));
	const uint8_t slot = role == ZLRS_ROLE_LEADER ? ZLRS_SLOT_DATA_A : ZLRS_SLOT_DATA_B;
	for (unsigned int attempt = 0; attempt < 4; attempt++) {
		const uint32_t now = sim.now_ms + 100 * (attempt + 1);
		const auto *req = zlrs_test_slot(link, slot, now);
		zassert_not_equal(req, nullptr);
		zassert_true(req->is_tx);
		zlrs_frame_t frame;
		zassert_true(zlrs_frame_decode(&frame, req->buf, req->size, ZLRS_TAG_LEN));
		zassert_equal(req->size - ZLRS_FRAME_HDR_LEN - ZLRS_DATA_PREFIX_LEN - ZLRS_TAG_LEN,
			      sizeof(bytes));
		zexpect_equal(memcmp(frame.body.data.bytes, bytes, sizeof(bytes)), 0);
		zlrs_link_complete(link, attempt == 3 ? ZLRS_RADIO_OK : ZLRS_RADIO_FAIL, 0, 0, 0,
				   now);
		zexpect_equal(link->queue_count, arq || attempt != 3 ? sizeof(bytes) : 0);
		zexpect_equal(zlrs_link_tx_space(link) + link->queue_count, ZLRS_LINK_TX_QUEUE_LEN);
	}
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, QueueFailure, test_FailedTransactionsKeepTheQueuedBytes,
			       QueueFailure_params);

ZTEST_P(LinkProfiles, test_ConfirmationLossAndExhaustion)
{
	const auto param = ZTEST_GET_PARAM(LinkProfilesParam);
	sim_init(&sim);
	const auto profile = param;

	sim.drop_confirms_only = true;
	sim.drop_leader = 3U;
	connect_saved(&sim, profile);
	zexpect_true(sim.drop_leader == 0U && sim.leader.link.stats.sess_cfm_tx >= 4U);
	zexpect_true(sim.follower.link.lq_last_seq != 0U);
	const uint8_t byte = 0xa5;
	zexpect_equal(zlrs_test_write(&sim.leader.link, &byte, 1U), 1U);
	const uint32_t nonce = sim.leader.link.sess_nonce_leader;
	sim.leader.link.tx_seq = UINT32_MAX;
	sim_frame(&sim);
	zexpect_true(sim.leader.link.sess_nonce_leader != nonce && sim.leader.link.tx_seq != 0U);
	zexpect_equal(sim.leader.link.queue_count, 1U);
	for (unsigned int i = 0U;
	     i < 3000U && (!both_connected(&sim) || sim.follower.received_len == 0U); i++) {
		sim_frame(&sim);
	}
	zassert_true(both_connected(&sim) && sim.follower.received_len == 1U &&
		     sim.follower.received[0] == byte);
	/* Responder exhaustion also stops the old key and requires a fresh challenge. */
	const uint32_t before = sim.leader.link.sess_nonce_leader;
	sim.follower.link.tx_seq = UINT32_MAX;
	sim_frame(&sim);
	for (unsigned int i = 0U; i < 3000U && !both_connected(&sim); i++) {
		sim_frame(&sim);
	}
	zassert_true(both_connected(&sim) && sim.leader.link.sess_nonce_leader != before);
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, LinkProfiles, test_ConfirmationLossAndExhaustion,
			       LinkProfiles_params);

ZTEST(Link, test_EntropyFailure)
{
	sim_init(&sim);
	const uint8_t salt[ZLRS_PAIR_SALT_LEN] = {1, 2, 3, 4, 5, 6, 7, 8};
	sim.leader.random_failed = true;
	zexpect_false(zlrs_link_press(&sim.leader.link, 0U));
	zexpect_equal(sim.leader.link.state, ZLRS_STATE_UNPAIRED);
	zlrs_link_set_peer(&sim.leader.link, sim.follower.link.dev_id, salt, 0U);
	zexpect_false(sim.leader.link.session_pending && !sim.leader.link.session_valid);
	zexpect_equal(zlrs_test_slot(&sim.leader.link, ZLRS_SLOT_DATA_A, 1U), nullptr);
	sim.follower.random_failed = true;
	zlrs_link_set_peer(&sim.follower.link, sim.leader.link.dev_id, salt, 0U);
	zlrs_frame_t frame = {.type = ZLRS_PKT_SYNC,
			      .body = {.sync = {.profile = 1U,
						.version = ZLRS_PROTO_VERSION,
						.nonce_leader = 42U,
						.band_plan = {.domain = ZLRS_DOMAIN_ISM2400}}},
			      .seq = 1U};
	inject(&sim.follower, &frame, ZLRS_SYNC_PAYLOAD_LEN, sim.follower.link.link_auth_in, 1U,
	       false);
	zexpect_false(sim.follower.link.session_pending && !sim.follower.link.session_valid);
	zexpect_equal(zlrs_test_slot(&sim.follower.link, ZLRS_SLOT_DATA_B, 2U), nullptr);
	zlrs_link_press(&sim.follower.link, 3U);
	frame = zlrs_frame_t{.type = ZLRS_PKT_PAIR_REQ,
			     .body = {.pair_req = {.dev_a = {1, 2, 3, 4}}},
			     .seq = 1U};
	inject(&sim.follower, &frame, ZLRS_PAIR_REQ_PAYLOAD_LEN, sim.follower.link.auth_key, 4U,
	       false);
	zexpect_false(sim.follower.link.pair_request_valid && !sim.follower.link.pair_wait_cfm);
	sim.leader.random_failed = false;
	sim.follower.random_failed = false;
	zlrs_link_cancel_pair(&sim.follower.link, 5U);
	for (unsigned int i = 0U; i < 3000U && !both_connected(&sim); i++) {
		sim_frame(&sim);
	}
	zassert_true(both_connected(&sim));
}

ZTEST(Link, test_NonceReuse)
{
	sim_init(&sim);
	/* Zero is valid on the first pairing round, but cannot seed a second one. */
	sim.leader.force_random = true;
	sim.leader.forced_random = 0U;
	zlrs_link_press(&sim.leader.link, 0U);
	zexpect_true(sim.leader.link.pair_request_valid && sim.leader.link.pair_nonce == 0U);
	zlrs_link_cancel_pair(&sim.leader.link, 1U);
	zlrs_link_press(&sim.leader.link, 2U);
	zexpect_equal(sim.leader.link.state, ZLRS_STATE_UNPAIRED);
	sim.leader.force_random = false;
	connect_saved(&sim, ZLRS_PROFILE_P1);
	sim.leader.force_random = true;
	sim.leader.forced_random = sim.leader.link.sess_nonce_leader;
	sim.leader.link.tx_seq = UINT32_MAX;
	sim_frame(&sim);
	zexpect_false(sim.leader.link.session_pending && !sim.leader.link.session_valid);
	zexpect_equal(zlrs_test_slot(&sim.leader.link, ZLRS_SLOT_DATA_A, sim.now_ms), nullptr);
	sim.leader.force_random = false;
	for (unsigned int i = 0U; i < 3000U && !both_connected(&sim); i++) {
		sim_frame(&sim);
	}
	zassert_true(both_connected(&sim));
}

static void retune(node_t *node, bool subg, zlrs_phy_t phy)
{
	zlrs_link_cfg_t cfg = node->link.cfg;
	cfg.domain = subg ? ZLRS_DOMAIN_CN433 : ZLRS_DOMAIN_ISM2400;
	cfg.mod = subg ? zlrs_mod_flrc(325000U) : zlrs_mod_phy(phy);
	cfg.sync_period_ms = subg ? 600U : 100U;
	zexpect_true(zlrs_link_reconfigure_radio(&node->link, &cfg));
}

static void automatic_frame(sim_t *sim, zlrs_band_policy_t policies[2])
{
	sim_frame(sim);
	node_t *nodes[2] = {&sim->leader, &sim->follower};
	for (unsigned int i = 0; i < 2; i++) {
		zlrs_link_t *link = &nodes[i]->link;
		const zlrs_link_info_t info = zlrs_link_get_info(link);
		bool changed = false, target = false, probe = false;
		zlrs_phy_t phy = ZLRS_PHY_FLRC_2600;
		if (info.band_plan.switch_frame &&
		    info.frame_index >= info.band_plan.switch_frame - 1) {
			target = info.band_plan.target_subg;
			phy = static_cast<zlrs_phy_t>(info.band_plan.target_phy);
			probe = !target && (policies[i].subg || phy < policies[i].phy);
			changed = true;
		} else {
			zlrs_band_observation_t o = {
				.now_ms = sim->now_ms,
				.last_rx_ms = info.last_rx_ms,
				.feedback_ms = info.last_peer_feedback_ms,
				.lq_generation = link->stats.lq_generation,
				.peer_lq_generation = info.peer_lq_generation,
				.paired = info.paired,
				.pairing = info.state == ZLRS_STATE_PAIRING,
				.connected =
					info.session_valid && info.state == ZLRS_STATE_CONNECTED,
				.acquiring = info.radio_acquiring,
				.planned = info.band_plan.switch_frame != 0,
				.power_at_max = true, /* Constant-power link model. */
				.rssi_dbm = link->stats.last_rssi,
				.peer_rssi_dbm = info.peer_rssi_dbm,
				.lq_percent = link->stats.lq_percent,
				.peer_lq_percent = info.peer_lq_percent};
			const zlrs_band_action_t action = zlrs_band_policy_update(&policies[i], &o);
			if (action == ZLRS_BAND_SEARCH) {
				target = policies[i].target_subg;
				phy = policies[i].target_phy;
				changed = true;
			} else if (action == ZLRS_BAND_PLAN) {
				zexpect_true(zlrs_link_plan_radio(
					link, policies[i].target_subg, policies[i].target_phy,
					info.frame_index + link->cfg.resync_every_n + 334U));
			}
		}
		if (changed) {
			retune(nodes[i], target, phy);
			zlrs_link_acquire_radio(link, sim->now_ms);
			zlrs_band_policy_changed(&policies[i], target, phy, probe, sim->now_ms,
						 info.last_rx_ms);
		}
	}
}

static bool captured_band(const sim_t *sim, bool subg)
{
	const zlrs_fhss_domain_id_t domain = subg ? ZLRS_DOMAIN_CN433 : ZLRS_DOMAIN_ISM2400;
	return both_connected(sim) && sim->leader.link.cfg.domain == domain &&
	       sim->follower.link.cfg.domain == domain && !sim->leader.link.radio_acquiring &&
	       !sim->follower.link.radio_acquiring;
}

ZTEST(Link, test_CaptureWaitsForPhase)
{
	sim_init(&sim);
	sim.leader.link.cfg.automatic_band = sim.follower.link.cfg.automatic_band = true;
	connect_saved(&sim, ZLRS_PROFILE_P1);
	retune(&sim.leader, true, ZLRS_PHY_FLRC_2600);
	retune(&sim.follower, true, ZLRS_PHY_FLRC_2600);
	sim.withhold_phase = true;
	const uint8_t payload[] = {1, 2, 3};
	zexpect_equal(zlrs_test_write(&sim.leader.link, payload, sizeof(payload)), sizeof(payload));
	zexpect_equal(zlrs_test_write(&sim.follower.link, payload, sizeof(payload)),
		      sizeof(payload));
	const zlrs_radio_req_t *req =
		zlrs_test_slot(&sim.follower.link, ZLRS_SLOT_DATA_B, sim.now_ms);
	zexpect_equal(req, nullptr); /* No SYNC yet: an old-session DATA is not capture proof. */
	if (req) {
		zlrs_link_complete(&sim.follower.link, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms);
	}
	const zlrs_radio_req_t *a = zlrs_test_slot(&sim.leader.link, ZLRS_SLOT_DATA_A, sim.now_ms);
	const zlrs_radio_req_t *b =
		zlrs_test_slot(&sim.follower.link, ZLRS_SLOT_DATA_A, sim.now_ms);
	zassert_not_equal(a, nullptr);
	zassert_not_equal(b, nullptr);
	zassert_true(a->is_tx);
	zassert_false(b->is_tx);
	memcpy(b->buf, a->buf, a->size);
	zlrs_link_complete(&sim.follower.link, ZLRS_RADIO_OK, a->size, -60, 0, sim.now_ms);
	zlrs_link_complete(&sim.leader.link, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms);
	zexpect_true(sim.follower.link
			     .radio_acquiring); /* Valid SYNC still needs physical phase proof. */
	req = zlrs_test_slot(&sim.follower.link, ZLRS_SLOT_DATA_B, sim.now_ms);
	zexpect_equal(req, nullptr);
	if (req) {
		zlrs_link_complete(&sim.follower.link, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms);
	}
	req = zlrs_test_slot(&sim.leader.link, ZLRS_SLOT_DATA_A, sim.now_ms + 1);
	if (req) {
		zlrs_frame_t air;
		zassert_true(zlrs_frame_decode(&air, req->buf, req->size, ZLRS_TAG_LEN));
		zexpect_equal(+air.type, ZLRS_PKT_SYNC);
		zlrs_link_complete(&sim.leader.link, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms);
	}
	zexpect_true(sim.leader.link.queue_count == sizeof(payload) &&
		     sim.follower.link.queue_count == sizeof(payload));
	zexpect_true(zlrs_link_radio_captured(&sim.follower.link));
	zexpect_false(sim.follower.link.radio_acquiring && sim.follower.link.sess_ack_due);
}

using AutomaticBandLinkParam = BandLoss;

static const AutomaticBandLinkParam AutomaticBandLink_values[] = {
	BandLoss{0, "BothDirectionsLost"},
	BandLoss{1, "FollowerDirectionLost"},
	BandLoss{2, "WeakSignal"},
	BandLoss{3, "AnnouncementsLost"},
};

static const char *AutomaticBandLink_param_name(size_t index, const void *value)
{
	ARG_UNUSED(value);
	static const char *const names[] = {"BothDirectionsLost", "FollowerDirectionLost",
					    "WeakSignal", "AnnouncementsLost"};
	return names[index];
}

static const ztest_param_values AutomaticBandLink_params = {
	.values = AutomaticBandLink_values,
	.count = ARRAY_SIZE(AutomaticBandLink_values),
	.elem_size = sizeof(AutomaticBandLink_values[0]),
	.name_cb = AutomaticBandLink_param_name,
};

ZTEST_P(AutomaticBandLink, test_PreservesSessionAndDataAcrossTransition)
{
	const auto param = ZTEST_GET_PARAM(AutomaticBandLinkParam);
	sim_init(&sim);
	const auto scenario = param.mode;

	sim.leader.link.cfg.automatic_band = sim.follower.link.cfg.automatic_band = true;
	connect_saved(&sim, ZLRS_PROFILE_P1);
	for (unsigned int i = 0; i < 100; i++) {
		sim_frame(&sim);
	}
	zlrs_band_policy_t policies[2];
	zlrs_band_policy_init(&policies[0], true, true, false, 0, sim.now_ms);
	zlrs_band_policy_init(&policies[1], true, false, false, 0, sim.now_ms);
	const uint32_t nonce = sim.leader.link.sess_nonce_leader;
	const uint32_t seq = sim.leader.link.tx_seq;
	const uint8_t payload[] = {3, 1, 4, 1, 5, 9};
	/* First payload reaches Follower, but its ACK is lost before changing radio. */
	sim.drop_follower = 100;
	zexpect_equal(zlrs_test_write(&sim.leader.link, payload, sizeof(payload)), sizeof(payload));
	for (unsigned int i = 0; i < 20; i++) {
		sim_frame(&sim);
	}
	zexpect_true(sim.follower.received_len == sizeof(payload) && sim.leader.link.arq_used);
	sim.drop_follower = 0;
	zexpect_equal(zlrs_test_write(&sim.follower.link, payload, sizeof(payload)),
		      sizeof(payload));
	if (scenario < 2) {
		sim.block_24_leader = scenario == 0;
		sim.block_24_follower = true;
	} else {
		sim.rssi_24 = -120;
		sim.lose_band_plans = scenario == 3;
	}
	bool reached = false;
	for (unsigned int i = 0; i < 60000; i++) {
		automatic_frame(&sim, policies);
		if (captured_band(&sim, true) && sim.leader.link.queue_count == 0 &&
		    sim.follower.link.queue_count == 0) {
			reached = true;
			break;
		}
	}
	zexpect_true(reached && sim.leader.link.sess_nonce_leader == nonce &&
		     sim.leader.link.tx_seq > seq);
	zexpect_true(sim.follower.received_len == sizeof(payload) &&
		     sim.leader.received_len == sizeof(payload));
	zexpect_equal(memcmp(sim.follower.received, payload, sizeof(payload)), 0);
	zexpect_equal(memcmp(sim.leader.received, payload, sizeof(payload)), 0);
	zexpect_true(sim.leader.link.stats.data_drop_bytes == 0 &&
		     sim.follower.link.stats.data_drop_bytes == 0);
	if (scenario == 0) {
		/* Blocked 2.4 GHz probe must return to the same SubG session. */
		bool tried = false, returned = false;
		for (unsigned int i = 0; i < 16000; i++) {
			automatic_frame(&sim, policies);
			tried |= sim.leader.link.cfg.domain == ZLRS_DOMAIN_ISM2400;
			if (tried && captured_band(&sim, true)) {
				returned = true;
				break;
			}
		}
		zexpect_true(tried && returned && sim.leader.link.sess_nonce_leader == nonce);
		sim.block_24_leader = sim.block_24_follower = false;
		bool preferred = false;
		for (unsigned int i = 0; i < 18000; i++) {
			automatic_frame(&sim, policies);
			if (captured_band(&sim, false)) {
				preferred = true;
				break;
			}
		}
		zexpect_true(preferred && sim.leader.link.sess_nonce_leader == nonce);
	}
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, AutomaticBandLink,
			       test_PreservesSessionAndDataAcrossTransition,
			       AutomaticBandLink_params);

ZTEST(Link, test_BandPlanSecurity)
{
	sim_init(&sim);
	sim.leader.link.cfg.automatic_band = sim.follower.link.cfg.automatic_band = true;
	connect_saved(&sim, ZLRS_PROFILE_P1);
	zlrs_link_t *follower = &sim.follower.link;
	const uint32_t baseline = follower->last_rx_seq, index = follower->frame_index;
	const uint32_t events = follower->accepted_rx_events;
	const int16_t rssi = follower->stats.last_rssi;
	zlrs_frame_t f = {.type = ZLRS_PKT_SYNC,
			  .body = {.sync = {.profile = 1,
					    .version = ZLRS_PROTO_VERSION,
					    .hop_index = static_cast<uint8_t>(follower->fhss.index),
					    .nonce_leader = follower->sess_nonce_leader,
					    .frame_index = index,
					    .band_plan = {.domain = ZLRS_DOMAIN_ISM2400,
							  .automatic = 1,
							  .target_subg = 1,
							  .switch_frame = index + 20}}},
			  .seq = baseline + 1};
	inject(&sim.follower, &f, ZLRS_SYNC_PAYLOAD_LEN, follower->link_auth_in, sim.now_ms, true);
	zexpect_true(follower->last_rx_seq == baseline && follower->band_plan.switch_frame == 0);
	zexpect_equal(follower->stats.last_rssi, rssi);
	f.body.sync.band_plan.domain = ZLRS_DOMAIN_CN433;
	inject(&sim.follower, &f, ZLRS_SYNC_PAYLOAD_LEN, follower->link_auth_in, sim.now_ms, false);
	zexpect_true(follower->last_rx_seq == baseline && follower->accepted_rx_events == events);
	f.body.sync.band_plan.domain = ZLRS_DOMAIN_ISM2400;
	f.body.sync.band_plan.switch_frame = index;
	inject(&sim.follower, &f, ZLRS_SYNC_PAYLOAD_LEN, follower->link_auth_in, sim.now_ms, false);
	zexpect_true(follower->last_rx_seq == baseline && follower->frame_index == index);
	f.body.sync.band_plan.switch_frame = index + 65536U;
	inject(&sim.follower, &f, ZLRS_SYNC_PAYLOAD_LEN, follower->link_auth_in, sim.now_ms, false);
	zexpect_true(follower->last_rx_seq == baseline && follower->accepted_rx_events == events);
	f.body.sync.band_plan.switch_frame = index + 20;
	f.body.sync.band_plan.target_subg = 0;
	inject(&sim.follower, &f, ZLRS_SYNC_PAYLOAD_LEN, follower->link_auth_in, sim.now_ms, false);
	zexpect_equal(follower->last_rx_seq, baseline);
	f.body.sync.band_plan.target_subg = 1;
	inject(&sim.follower, &f, ZLRS_SYNC_PAYLOAD_LEN, follower->link_auth_in, sim.now_ms, false);
	zexpect_true(follower->last_rx_seq == baseline + 1 &&
		     follower->band_plan.switch_frame == index + 20);
	f.body.sync.band_plan.switch_frame++;
	inject(&sim.follower, &f, ZLRS_SYNC_PAYLOAD_LEN, follower->link_auth_in, sim.now_ms, false);
	zexpect_true(follower->band_plan.switch_frame == index + 20 &&
		     follower->accepted_rx_events == events + 1);
	zexpect_false(zlrs_link_plan_radio(follower, true, ZLRS_PHY_FLRC_2600, index + 20));
	zexpect_false(zlrs_link_plan_radio(&sim.leader.link, true, ZLRS_PHY_FLRC_2600, 0));
	/* Same-band PHY plans are authenticated and cannot lie about the current PHY. */
	f.seq = baseline + 2U;
	f.body.sync.band_plan.target_subg = 0U;
	f.body.sync.band_plan.target_phy = ZLRS_PHY_LORA_SF6;
	f.body.sync.band_plan.phy = ZLRS_PHY_FLRC_650;
	inject(&sim.follower, &f, ZLRS_SYNC_PAYLOAD_LEN, follower->link_auth_in, sim.now_ms, false);
	zexpect_equal(follower->last_rx_seq, baseline + 1U);
	f.body.sync.band_plan.phy = ZLRS_PHY_FLRC_2600;
	f.body.sync.band_plan.target_phy = ZLRS_PHY_FLRC_1300;
	inject(&sim.follower, &f, ZLRS_SYNC_PAYLOAD_LEN, follower->link_auth_in, sim.now_ms, false);
	zexpect_equal(follower->last_rx_seq, baseline + 1U);
	f.body.sync.band_plan.target_phy = ZLRS_PHY_LORA_SF6;
	inject(&sim.follower, &f, ZLRS_SYNC_PAYLOAD_LEN, follower->link_auth_in, sim.now_ms, false);
	zexpect_true(follower->last_rx_seq == baseline + 2U &&
		     follower->band_plan.target_phy == ZLRS_PHY_LORA_SF6);
	const zlrs_radio_req_t *req = zlrs_test_slot(follower, ZLRS_SLOT_DATA_A, sim.now_ms);
	zassert_not_equal(req, nullptr);
	zlrs_link_cfg_t cfg = follower->cfg;
	cfg.domain = ZLRS_DOMAIN_CN433;
	zexpect_false(zlrs_link_reconfigure_radio(follower, &cfg) &&
		      follower->cfg.domain == ZLRS_DOMAIN_ISM2400);
	zlrs_link_complete(follower, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms);
	cfg.profile = ZLRS_PROFILE_P0;
	zexpect_false(zlrs_link_reconfigure_radio(follower, &cfg));
}

ZTEST(Link, test_DuplexDeliveryAndWireTranscript)
{
	sim_init(&sim);
	uint8_t leader_data[1024];
	uint8_t follower_data[768];
	printf("data link: pair, session, duplex ARQ, loss and backpressure\n");

	zlrs_link_press(&sim.leader.link, sim.now_ms);
	zlrs_link_press(&sim.follower.link, sim.now_ms);
	for (uint32_t i = 0U; i < 3000U && !both_connected(&sim); i++) {
		sim_frame(&sim);
	}
	zassert_true(both_connected(&sim));
	zexpect_true(zlrs_link_get_info(&sim.leader.link).paired);
	zexpect_true(zlrs_link_get_info(&sim.follower.link).paired);
	for (uint16_t i = 0U; i < sizeof(leader_data); i++) {
		leader_data[i] = (uint8_t)(i * 37U + 11U);
	}
	for (uint16_t i = 0U; i < sizeof(follower_data); i++) {
		follower_data[i] = (uint8_t)(i * 19U + 7U);
	}
	zexpect_equal(zlrs_test_write(&sim.leader.link, leader_data, sizeof(leader_data)),
		      sizeof(leader_data));
	zexpect_equal(zlrs_test_write(&sim.follower.link, follower_data, sizeof(follower_data)),
		      sizeof(follower_data));
	sim.follower.blocked = true;
	sim.drop_leader = 4U;
	sim.drop_follower = 3U;
	for (uint32_t i = 0U; i < 300U; i++) {
		sim_frame(&sim);
	}
	zexpect_equal(sim.follower.received_len, 0U);
	sim.follower.blocked = false;
	for (uint32_t i = 0U; i < 5000U && (sim.follower.received_len < sizeof(leader_data) ||
					    sim.leader.received_len < sizeof(follower_data));
	     i++) {
		sim_frame(&sim);
	}
	zexpect_equal(sim.follower.received_len, sizeof(leader_data));
	zexpect_equal(sim.leader.received_len, sizeof(follower_data));
	zexpect_equal(memcmp(sim.follower.received, leader_data, sizeof(leader_data)), 0);
	zexpect_equal(memcmp(sim.leader.received, follower_data, sizeof(follower_data)), 0);
	zexpect_true((zlrs_link_get_stats(&sim.leader.link)->tx_retries) > (0U));
	zexpect_true((zlrs_link_get_stats(&sim.follower.link)->tx_retries) > (0U));
	const uint32_t leader_feedback_before =
		zlrs_link_get_info(&sim.leader.link).last_peer_feedback_ms;
	const uint32_t follower_feedback_before =
		zlrs_link_get_info(&sim.follower.link).last_peer_feedback_ms;
	for (uint32_t i = 0U; i < 100U; i++) {
		sim_frame(&sim);
	}
	zexpect_equal(zlrs_link_get_info(&sim.leader.link).peer_rssi_dbm, -60);
	zexpect_equal(zlrs_link_get_info(&sim.follower.link).peer_rssi_dbm, -60);
	zexpect_true((zlrs_link_get_info(&sim.leader.link).last_peer_feedback_ms) >
		     (leader_feedback_before));
	zexpect_true((zlrs_link_get_info(&sim.follower.link).last_peer_feedback_ms) >
		     (follower_feedback_before));
	/* Golden bytes include feedback pacing: payload DATA also refreshes the
	 * feedback deadline, so a separate empty report is not needed immediately. */
	zexpect_equal(sim.wire_packets, 192U);
	printf("wire v13: packets=%u hash=%016llx\n", sim.wire_packets,
	       (unsigned long long)sim.wire_hash);
	zexpect_equal(sim.wire_hash, UINT64_C(0x922e60a73a63c4b0));
	/* An authenticated DATA capture cannot be delivered twice, tampered with or reflected. */
	zassert_true((sim.captured_len) > (0U));
	const uint16_t received = sim.follower.received_len;
	const uint32_t duplicates = sim.follower.link.stats.rx_dup;
	zlrs_test_rx(&sim.follower.link, sim.captured, sim.captured_len, -60, 0, sim.now_ms);
	zexpect_equal(sim.follower.link.stats.rx_dup, duplicates + 1U);
	zexpect_equal(sim.follower.received_len, received);
	const uint32_t bad_tags = sim.follower.link.stats.rx_bad_tag;
	sim.captured[sim.captured_len - 1U] ^= 0x80U;
	zlrs_test_rx(&sim.follower.link, sim.captured, sim.captured_len, -60, 0, sim.now_ms);
	zexpect_equal(sim.follower.link.stats.rx_bad_tag, bad_tags + 1U);
	zexpect_equal(sim.follower.received_len, received);
	sim.captured[sim.captured_len - 1U] ^= 0x80U;
	const uint32_t reflected = sim.leader.link.stats.rx_bad_tag;
	zlrs_test_rx(&sim.leader.link, sim.captured, sim.captured_len, -60, 0, sim.now_ms);
	zexpect_equal(sim.leader.link.stats.rx_bad_tag, reflected + 1U);

	/* Admission does no encoding; the next TX incorporates the received ACK. */
	const uint8_t queued[] = {0x33, 0x55, 0xaa};
	uint16_t capacity;
	uint8_t *payload = zlrs_link_tx_reserve(&sim.leader.link, &capacity);
	zassert_not_equal(payload, nullptr);
	zassert_true((capacity) >= (sizeof(queued)));
	memcpy(payload, queued, sizeof(queued));
	zlrs_link_tx_commit(&sim.leader.link, sizeof(queued));
	const auto builds = sim.leader.link.stats.tx_frame_builds;
	const auto tags = sim.leader.link.stats.tx_auth_tags;
	const bool old_ack =
		sim.leader.link.arq_rx_started && ((sim.leader.link.arq_rx_seq + 1U) & 1U);
	uint8_t peer_frame[32];
	const uint8_t peer_byte = 0xab;
	zlrs_frame_t packet = {.type = ZLRS_PKT_DATA_F2L,
		.body = {.data = {.header = {.data_seq = sim.follower.link.arq_seq,
			.ack = (sim.leader.link.arq_seq & 1U) != 0U,
			.rssi_magnitude = 60U, .lq_percent = 100U}, .bytes = &peer_byte}},
		.seq = sim.follower.link.tx_seq++};
	const uint16_t peer_size = zlrs_frame_len(ZLRS_DATA_PREFIX_LEN + 1U, ZLRS_TAG_LEN);
	zassert_equal(
		zlrs_test_encode(&packet, peer_frame, sizeof(peer_frame), peer_size, ZLRS_TAG_LEN),
		peer_size);
	zlrs_tag(sim.follower.link.session_key_out, ZLRS_KEY_LEN, peer_frame,
		 peer_size - ZLRS_TAG_LEN, &peer_frame[peer_size - ZLRS_TAG_LEN]);
	const uint16_t delivered_before = sim.leader.received_len;
	zlrs_test_rx(&sim.leader.link, peer_frame, peer_size, -60, 0, sim.now_ms);
	zexpect_equal(sim.leader.received_len, delivered_before + 1U);
	zexpect_equal(sim.leader.received[delivered_before], 0xabU);
	const zlrs_radio_req_t *request =
		zlrs_test_slot(&sim.leader.link, ZLRS_SLOT_DATA_A, sim.now_ms);
	zexpect_true(request != nullptr && request->is_tx);
	if (request != nullptr) {
		zlrs_frame_t after;
		zassert_true(zlrs_frame_decode(&after, request->buf, request->size, ZLRS_TAG_LEN));
		zexpect_equal(+after.type, ZLRS_PKT_DATA_L2F);
		zexpect_not_equal(old_ack, after.body.data.header.ack);
		zexpect_true(sim.leader.link.arq_ack_pending);
		zexpect_equal(request->buf + ZLRS_DATA_HEADROOM, payload);
		zexpect_equal(sim.leader.link.stats.tx_frame_builds, builds + 1);
		zexpect_equal(sim.leader.link.stats.tx_auth_tags, tags + 1);
		zexpect_equal(request->size,
			      zlrs_frame_len(ZLRS_DATA_PREFIX_LEN + sizeof(queued), ZLRS_TAG_LEN));
		zexpect_equal(memcmp(after.body.data.bytes, queued, sizeof(queued)), 0);
		zlrs_link_complete(&sim.leader.link, ZLRS_RADIO_OK, 0U, 0, 0, sim.now_ms);
	}

	/* A receiver reboot derives fresh session keys and re-establishes both directions. */
	uint8_t root[ZLRS_KEY_LEN];
	uint8_t old_key[ZLRS_KEY_LEN];
	uint8_t old_capture[ZLRS_FRAME_MAX_LEN];
	const uint16_t old_size = sim.captured_len;
	memcpy(old_capture, sim.captured, old_size);
	memcpy(root, sim.follower.link.root_key, sizeof(root));
	memcpy(old_key, sim.follower.link.session_key_in, sizeof(old_key));
	zlrs_link_set_root_key(&sim.follower.link, root, sim.now_ms);
	for (unsigned int i = 0U; i < 3000U && !both_connected(&sim); ++i) {
		sim_frame(&sim);
	}
	zassert_true(both_connected(&sim));
	zexpect_not_equal(memcmp(old_key, sim.follower.link.session_key_in, sizeof(old_key)), 0);
	const uint32_t old_session_tags = sim.follower.link.stats.rx_bad_tag;
	zlrs_test_rx(&sim.follower.link, old_capture, old_size, -60, 0, sim.now_ms);
	zexpect_equal(sim.follower.link.stats.rx_bad_tag, old_session_tags + 1U);
}

using FinalizedAckParam = std::tuple<zlrs_profile_t, zlrs_role_t, bool>;

static const FinalizedAckParam FinalizedAck_values[] = {
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_LEADER, false),
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_LEADER, true),
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_FOLLOWER, false),
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_FOLLOWER, true),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_LEADER, false),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_LEADER, true),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_FOLLOWER, false),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_FOLLOWER, true),
};

static const char *FinalizedAck_param_name(size_t index, const void *value)
{
	ARG_UNUSED(value);
	static const char *const names[] = {
		"P0LeaderAccepted", "P0LeaderBlocked", "P0FollowerAccepted", "P0FollowerBlocked",
		"P1LeaderAccepted", "P1LeaderBlocked", "P1FollowerAccepted", "P1FollowerBlocked"};
	return names[index];
}

static const ztest_param_values FinalizedAck_params = {
	.values = FinalizedAck_values,
	.count = ARRAY_SIZE(FinalizedAck_values),
	.elem_size = sizeof(FinalizedAck_values[0]),
	.name_cb = FinalizedAck_param_name,
};

ZTEST_P(FinalizedAck, test_LatestAckIsEncodedOnceAndFailedTxPreservesPayload)
{
	const auto param = ZTEST_GET_PARAM(FinalizedAckParam);
	sim_init(&sim);
	const auto [profile, role, blocked] = param;
	connect_saved(&sim, profile);
	auto *node = role == ZLRS_ROLE_LEADER ? &sim.leader : &sim.follower;
	auto *peer = role == ZLRS_ROLE_LEADER ? &sim.follower : &sim.leader;
	auto *link = &node->link;
	link->cfg.resync_every_n = 0;
	link->sess_cfm_due = false;
	link->queue_head = ZLRS_LINK_TX_PACKETS - 2;
	const uint8_t bytes[] = {1, 3, 5, 7, 9};
	zassert_equal(zlrs_test_write(link, bytes, sizeof(bytes)), sizeof(bytes));
	const uint8_t slot = role == ZLRS_ROLE_LEADER ? ZLRS_SLOT_DATA_A : ZLRS_SLOT_DATA_B;
	const auto builds = link->stats.tx_frame_builds;
	const auto tags = link->stats.tx_auth_tags;
	const bool old_ack = link->arq_rx_started && ((link->arq_rx_seq + 1U) & 1U);
	node->blocked = blocked;
	zlrs_frame_t incoming = {};
	incoming.type = role == ZLRS_ROLE_LEADER ? ZLRS_PKT_DATA_F2L : ZLRS_PKT_DATA_L2F;
	incoming.seq = peer->link.tx_seq++;
	incoming.body.data.header.data_seq =
		link->arq_rx_started ? (link->arq_rx_seq + 1U) & ZLRS_DATA_SEQ_MASK : 0U;
	const uint8_t incoming_byte = 0xa5;
	incoming.body.data.bytes = &incoming_byte;
	const auto received = node->received_len;
	inject(node, &incoming, ZLRS_DATA_PREFIX_LEN + 1, link->session_key_in, sim.now_ms, false);
	zexpect_equal(node->received_len, received + !blocked);
	const auto *req = zlrs_test_slot(link, slot, sim.now_ms);
	zassert_not_equal(req, nullptr);
	zassert_true(req->is_tx);
	zexpect_equal(link->stats.tx_frame_builds, builds + 1);
	zexpect_equal(link->stats.tx_auth_tags, tags + (profile == ZLRS_PROFILE_P1));
	zlrs_frame_t sent;
	zassert_true(zlrs_frame_decode(&sent, req->buf, req->size, zlrs_profile_tag_len(profile)));
	zexpect_equal(+sent.body.data.header.ack, old_ack ^ !blocked);
	zexpect_equal(memcmp(sent.body.data.bytes, bytes, sizeof(bytes)), 0);
	if (profile == ZLRS_PROFILE_P1) {
		uint8_t tag[ZLRS_TAG_LEN];
		zlrs_tag(link->session_key_out, ZLRS_KEY_LEN, req->buf, req->size - ZLRS_TAG_LEN,
			 tag);
		zexpect_equal(memcmp(tag, sent.tag, sizeof(tag)), 0);
	}
	zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0, 0, 0, sim.now_ms);
	zexpect_equal(link->queue_count, sizeof(bytes));
	node->blocked = false;
	const auto peer_received = peer->received_len;
	for (unsigned int i = 0; i < 100 && link->queue_count != 0; i++) {
		sim_frame(&sim);
	}
	zexpect_equal(link->queue_count, 0);
	zassert_equal(peer->received_len, peer_received + sizeof(bytes));
	zexpect_equal(memcmp(peer->received + peer_received, bytes, sizeof(bytes)), 0);
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, FinalizedAck,
			       test_LatestAckIsEncodedOnceAndFailedTxPreservesPayload,
			       FinalizedAck_params);

ZTEST_P(LinkProfiles, test_DuplexDataAcknowledgedInTheNextSlot)
{
	const auto param = ZTEST_GET_PARAM(LinkProfilesParam);
	sim_init(&sim);
	connect_saved(&sim, param);
	for (auto *node : {&sim.leader, &sim.follower}) {
		node->link.cfg.resync_every_n = 0;
		node->link.sess_cfm_due = false;
	}
	/* Sixteen full packets in each direction should take sixteen frame periods. */
	std::vector<uint8_t> leader_bytes(16 * 128, 0x35);
	std::vector<uint8_t> follower_bytes(16 * 128, 0xac);
	zassert_equal(zlrs_test_write(&sim.leader.link, leader_bytes.data(), leader_bytes.size()),
		      leader_bytes.size());
	zassert_equal(
		zlrs_test_write(&sim.follower.link, follower_bytes.data(), follower_bytes.size()),
		follower_bytes.size());
	const auto start_ms = sim.now_ms;
	for (unsigned int frame = 0; frame < 16; frame++) {
		sim_frame(&sim);
	}
	printf("duplex P%d: leader=%u follower=%u elapsed=%u ms ack_max=%u/%u ms\n", param,
	       sim.leader.received_len, sim.follower.received_len, sim.now_ms - start_ms,
	       sim.leader.link.stats.tx_ack_delay_ms_max,
	       sim.follower.link.stats.tx_ack_delay_ms_max);
	zexpect_equal(sim.follower.received_len, leader_bytes.size());
	zexpect_equal(sim.leader.received_len, follower_bytes.size());
	zexpect_equal(memcmp(sim.follower.received, leader_bytes.data(), leader_bytes.size()), 0);
	zexpect_equal(memcmp(sim.leader.received, follower_bytes.data(), follower_bytes.size()), 0);
	sim_frame(&sim); /* ACK for the final Follower packet. */
	for (auto *node : {&sim.leader, &sim.follower}) {
		zexpect_equal(node->link.queue_count, 0);
		zexpect_true((node->link.stats.tx_ack_delay_ms_max) <= (3U));
		zexpect_equal(node->link.stats.tx_retries, 0U);
	}
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, LinkProfiles, test_DuplexDataAcknowledgedInTheNextSlot,
			       LinkProfiles_params);

using StreamFaultsParam = std::tuple<zlrs_profile_t, uint32_t>;

static const StreamFaultsParam StreamFaults_values[] = {
	std::make_tuple(ZLRS_PROFILE_P0, 0x12345678U),
	std::make_tuple(ZLRS_PROFILE_P0, 0x87654321U),
	std::make_tuple(ZLRS_PROFILE_P0, 0x9e3779b9U),
	std::make_tuple(ZLRS_PROFILE_P1, 0x12345678U),
	std::make_tuple(ZLRS_PROFILE_P1, 0x87654321U),
	std::make_tuple(ZLRS_PROFILE_P1, 0x9e3779b9U),
};

static const char *StreamFaults_param_name(size_t index, const void *value)
{
	ARG_UNUSED(value);
	static const char *const names[] = {"P0Seed305419896",  "P0Seed2271560481",
					    "P0Seed2654435769", "P1Seed305419896",
					    "P1Seed2271560481", "P1Seed2654435769"};
	return names[index];
}

static const ztest_param_values StreamFaults_params = {
	.values = StreamFaults_values,
	.count = ARRAY_SIZE(StreamFaults_values),
	.elem_size = sizeof(StreamFaults_values[0]),
	.name_cb = StreamFaults_param_name,
};

ZTEST_P(StreamFaults, test_DuplexStreamMatchesReferenceUnderFaultSequences)
{
	const auto param = ZTEST_GET_PARAM(StreamFaultsParam);
	sim_init(&sim);
	const auto [profile, seed] = param;
	TC_PRINT("stream seed=%u\n", seed);
	sim.now_ms = UINT32_MAX - 1000;
	connect_saved(&sim, profile);
	std::vector<uint8_t> expected[2];
	node_t *nodes[] = {&sim.leader, &sim.follower};
	uint32_t rng = seed;
	auto next = [&]() {
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		return rng;
	};
	bool queue_wrapped[2] = {};
	unsigned int step = 0;
	auto check = [&]() {
		for (unsigned int side = 0; side < 2; side++) {
			const auto &sender = nodes[side]->link;
			const auto &receiver = *nodes[1 - side];
			zassert_true((sender.queue_count) <= (ZLRS_LINK_TX_QUEUE_LEN),
				     "seed=%u step=%u", seed, step);
			if (sender.tx_packet_limit != 0U) {
				zassert_true((sender.queue_head) < (sender.tx_packet_limit),
					     "seed=%u step=%u", seed, step);
			}
			zassert_true((sender.arq_used ? sender.tx_packets[sender.queue_head].len
						      : 0) <= (sender.queue_count),
				     "seed=%u step=%u", seed, step);
			zexpect_equal(zlrs_link_tx_space(&sender) + sender.queue_count,
				      ZLRS_LINK_TX_QUEUE_LEN, "seed=%u step=%u", seed, step);
			zassert_true((receiver.received_len) <= (expected[side].size()),
				     "seed=%u step=%u", seed, step);
			if (receiver.received_len) {
				zassert_equal(memcmp(receiver.received, expected[side].data(),
						     receiver.received_len),
					      0, "seed=%u step=%u", seed, step);
			}
			/* Queued includes delivered bytes awaiting ACK, so delivery can be ahead.
			 */
			zassert_true((expected[side].size() - receiver.received_len) <=
					     (sender.queue_count),
				     "seed=%u step=%u", seed, step);
		}
	};
	for (step = 0; step < 5000; step++) {
		/* Step is included in stream-oracle assertion failures. */;
		for (unsigned int side = 0; side < 2; side++) {
			auto &sender = nodes[side]->link;
			uint8_t bytes[32];
			const uint16_t count = next() % sizeof(bytes) + 1;
			if (expected[side].size() + count <= 12000 && next() % 2 == 0 &&
			    zlrs_link_tx_space(&sender) >= count) {
				for (auto &byte : bytes) {
					byte = (uint8_t)next();
				}
				zassert_equal(zlrs_test_write(&sender, bytes, count), count,
					      "seed=%u step=%u", seed, step);
				expected[side].insert(expected[side].end(), bytes, bytes + count);
			}
			nodes[side]->blocked = next() % 4 == 0;
		}
		sim.drop_leader = next() % 3 == 0;
		sim.drop_follower = next() % 3 == 0;
		sim.fail_leader = next() % 5 == 0;
		sim.fail_follower = next() % 5 == 0;
		sim.duplicate_air = next() % 5 == 0;
		/* P0 cannot detect intentionally corrupted payloads. */
		sim.corrupt_air = profile == ZLRS_PROFILE_P1 && next() % 5 == 0;
		const uint16_t heads[] = {nodes[0]->link.queue_head, nodes[1]->link.queue_head};
		sim_frame(&sim);
		for (unsigned int side = 0; side < 2; side++) {
			queue_wrapped[side] |= nodes[side]->link.queue_head < heads[side];
		}
		check();
	}
	sim.drop_leader = sim.drop_follower = sim.fail_leader = sim.fail_follower = 0;
	sim.corrupt_air = sim.duplicate_air = false;
	sim.leader.blocked = sim.follower.blocked = false;
	for (step = 0; step < 10000 && (nodes[0]->link.queue_count || nodes[1]->link.queue_count);
	     step++) {
		sim_frame(&sim);
		check();
	}
	for (unsigned int side = 0; side < 2; side++) {
		zexpect_equal(nodes[side]->link.queue_count, 0, "seed=%u step=%u", seed, step);
		zexpect_equal(nodes[1 - side]->received_len, expected[side].size(),
			      "seed=%u step=%u", seed, step);
		zexpect_true((expected[side].size()) > (ZLRS_LINK_TX_QUEUE_LEN), "seed=%u step=%u",
			     seed, step);
		zexpect_true(queue_wrapped[side], "seed=%u step=%u", seed, step);
		zexpect_true((nodes[side]->link.stats.radio_fail) > (0U), "seed=%u step=%u", seed,
			     step);
		zexpect_true((nodes[side]->link.stats.tx_retries) > (0U), "seed=%u step=%u", seed,
			     step);
		zexpect_equal(nodes[side]->link.stats.data_drop_bytes, 0U, "seed=%u step=%u", seed,
			      step);
	}
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, StreamFaults,
			       test_DuplexStreamMatchesReferenceUnderFaultSequences,
			       StreamFaults_params);

struct TxMode {
	bool arq;
	bool plan_first;
	const char *name;
};

using TxAccountingParam = std::tuple<zlrs_profile_t, zlrs_role_t, TxMode, bool>;

static const TxAccountingParam TxAccounting_values[] = {
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_LEADER, TxMode{false, false, "BestEffort"},
			false),
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_LEADER, TxMode{false, false, "BestEffort"},
			true),
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_LEADER, TxMode{true, false, "ARQ"}, false),
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_LEADER, TxMode{true, false, "ARQ"}, true),
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_LEADER, TxMode{true, true, "PlannedARQ"}, false),
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_LEADER, TxMode{true, true, "PlannedARQ"}, true),
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_FOLLOWER, TxMode{false, false, "BestEffort"},
			false),
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_FOLLOWER, TxMode{false, false, "BestEffort"},
			true),
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_FOLLOWER, TxMode{true, false, "ARQ"}, false),
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_FOLLOWER, TxMode{true, false, "ARQ"}, true),
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_FOLLOWER, TxMode{true, true, "PlannedARQ"},
			false),
	std::make_tuple(ZLRS_PROFILE_P0, ZLRS_ROLE_FOLLOWER, TxMode{true, true, "PlannedARQ"},
			true),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_LEADER, TxMode{false, false, "BestEffort"},
			false),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_LEADER, TxMode{false, false, "BestEffort"},
			true),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_LEADER, TxMode{true, false, "ARQ"}, false),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_LEADER, TxMode{true, false, "ARQ"}, true),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_LEADER, TxMode{true, true, "PlannedARQ"}, false),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_LEADER, TxMode{true, true, "PlannedARQ"}, true),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_FOLLOWER, TxMode{false, false, "BestEffort"},
			false),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_FOLLOWER, TxMode{false, false, "BestEffort"},
			true),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_FOLLOWER, TxMode{true, false, "ARQ"}, false),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_FOLLOWER, TxMode{true, false, "ARQ"}, true),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_FOLLOWER, TxMode{true, true, "PlannedARQ"},
			false),
	std::make_tuple(ZLRS_PROFILE_P1, ZLRS_ROLE_FOLLOWER, TxMode{true, true, "PlannedARQ"},
			true),
};

static const char *TxAccounting_param_name(size_t index, const void *value)
{
	ARG_UNUSED(value);
	static const char *const names[] = {
		"P0LeaderBestEffortPartial",   "P0LeaderBestEffortFull",
		"P0LeaderARQPartial",          "P0LeaderARQFull",
		"P0LeaderPlannedARQPartial",   "P0LeaderPlannedARQFull",
		"P0FollowerBestEffortPartial", "P0FollowerBestEffortFull",
		"P0FollowerARQPartial",        "P0FollowerARQFull",
		"P0FollowerPlannedARQPartial", "P0FollowerPlannedARQFull",
		"P1LeaderBestEffortPartial",   "P1LeaderBestEffortFull",
		"P1LeaderARQPartial",          "P1LeaderARQFull",
		"P1LeaderPlannedARQPartial",   "P1LeaderPlannedARQFull",
		"P1FollowerBestEffortPartial", "P1FollowerBestEffortFull",
		"P1FollowerARQPartial",        "P1FollowerARQFull",
		"P1FollowerPlannedARQPartial", "P1FollowerPlannedARQFull"};
	return names[index];
}

static const ztest_param_values TxAccounting_params = {
	.values = TxAccounting_values,
	.count = ARRAY_SIZE(TxAccounting_values),
	.elem_size = sizeof(TxAccounting_values[0]),
	.name_cb = TxAccounting_param_name,
};

ZTEST_P(TxAccounting, test_CountsTheSubmittedPacketExactlyOnce)
{
	const auto param = ZTEST_GET_PARAM(TxAccountingParam);
	sim_init(&sim);
	const auto [profile, role, mode, full] = param;
	connect_saved(&sim, profile);
	auto *link = role == ZLRS_ROLE_LEADER ? &sim.leader.link : &sim.follower.link;
	link->cfg.resync_every_n = 0;
	link->sess_cfm_due = false;
	link->cfg.arq = mode.arq;
	link->cfg.data_mtu = ZLRS_FRAME_MAX_LEN - ZLRS_FRAME_HDR_LEN - ZLRS_DATA_PREFIX_LEN -
			     zlrs_profile_tag_len(profile);
	const auto payload = full ? link->cfg.data_mtu : 17;
	std::vector<uint8_t> bytes(payload, 0x42);
	zassert_equal(zlrs_test_write(link, bytes.data(), bytes.size()), bytes.size());
	const auto slot = role == ZLRS_ROLE_LEADER ? ZLRS_SLOT_DATA_A : ZLRS_SLOT_DATA_B;
	const zlrs_radio_req_t *req;
	if (mode.plan_first) {
		zassert_true(zlrs_link_advance_slot(link, slot, sim.now_ms));
		const auto builds = link->stats.tx_frame_builds;
		const auto seq = link->tx_seq;
		const auto plan = zlrs_link_plan(link);
		zexpect_equal(plan.op, ZLRS_OP_TX);
		zexpect_equal(plan.radio.buf, nullptr);
		zexpect_equal(plan.radio.size,
			      ZLRS_DATA_HEADROOM + payload + zlrs_profile_tag_len(profile));
		zexpect_equal(link->stats.tx_frame_builds, builds);
		zexpect_equal(link->tx_seq, seq);
		req = zlrs_link_finalize(link, &plan);
	} else {
		req = zlrs_test_slot(link, slot, sim.now_ms);
	}
	zassert_not_equal(req, nullptr);
	zassert_true(req->is_tx);
	const uint16_t wire_size = req->size;
	const auto before = *zlrs_link_get_stats(link);
	zlrs_link_complete(link, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms);
	zexpect_equal(link->stats.tx_frames, before.tx_frames + 1);
	zexpect_equal(link->stats.tx_bytes, before.tx_bytes + wire_size);
	zexpect_equal(link->stats.tx_data_frames, before.tx_data_frames + 1);
	zexpect_equal(link->stats.tx_data_bytes, before.tx_data_bytes + payload);
	zexpect_equal(link->stats.tx_full_data_frames, before.tx_full_data_frames + full);
	zexpect_equal(link->stats.tx_data_ack_frames, before.tx_data_ack_frames + mode.arq);
	zexpect_equal(link->stats.tx_ack_only_frames, before.tx_ack_only_frames);
	const auto completed = *zlrs_link_get_stats(link);
	zlrs_link_complete(link, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms);
	zexpect_equal(memcmp(&link->stats, &completed, sizeof(completed)), 0);
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, TxAccounting, test_CountsTheSubmittedPacketExactlyOnce,
			       TxAccounting_params);

ZTEST_P(LinkProfiles, test_EmptyDataCompletionIsAckOnly)
{
	const auto param = ZTEST_GET_PARAM(LinkProfilesParam);
	sim_init(&sim);
	connect_saved(&sim, param);
	auto *link = &sim.leader.link;
	link->cfg.resync_every_n = 0;
	link->sess_cfm_due = false;
	link->arq_ack_pending = true;
	const auto *req = zlrs_test_slot(link, ZLRS_SLOT_DATA_A, sim.now_ms);
	zassert_not_equal(req, nullptr);
	zassert_true(req->is_tx);
	const auto before = *zlrs_link_get_stats(link);
	zlrs_link_complete(link, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms);
	zexpect_equal(link->stats.tx_ack_only_frames, before.tx_ack_only_frames + 1);
	zexpect_equal(link->stats.tx_data_frames, before.tx_data_frames);
	zexpect_equal(link->stats.tx_data_bytes, before.tx_data_bytes);
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, LinkProfiles, test_EmptyDataCompletionIsAckOnly,
			       LinkProfiles_params);

ZTEST(Link, test_UnsolicitedCompletionLeavesStateAndCountersIntact)
{
	sim_init(&sim);
	const auto before = *zlrs_link_get_stats(&sim.leader.link);
	zlrs_link_complete(&sim.leader.link, ZLRS_RADIO_FAIL, 0, 0, 0, sim.now_ms);
	zexpect_equal(memcmp(&sim.leader.link.stats, &before, sizeof(before)), 0);
}

ZTEST_P(LinkProfiles, test_DirectPayloadAdmissionAndRetryKeepTheSameStorage)
{
	const auto param = ZTEST_GET_PARAM(LinkProfilesParam);
	sim_init(&sim);
	connect_saved(&sim, param);
	auto *link = &sim.leader.link;
	link->cfg.resync_every_n = 0;
	link->cfg.arq_timeout_ms = 1000;
	link->sess_cfm_due = false;
	uint16_t capacity;
	uint8_t *payload = zlrs_link_tx_reserve(link, &capacity);
	zassert_not_equal(payload, nullptr);
	zassert_true((capacity) >= (5));
	const uint8_t bytes[] = {9, 8, 7, 6, 5};
	memcpy(payload, bytes, sizeof(bytes));
	zlrs_link_tx_commit(link, sizeof(bytes));
	zexpect_equal(link->queue_count, sizeof(bytes));
	const auto *req = zlrs_test_slot(link, ZLRS_SLOT_DATA_A, sim.now_ms);
	zassert_not_equal(req, nullptr);
	zexpect_equal(req->buf + ZLRS_DATA_HEADROOM, payload);
	const auto builds = link->stats.tx_frame_builds;
	const std::vector<uint8_t> frozen(req->buf, req->buf + req->size);
	zexpect_false(zlrs_link_advance_slot(link, ZLRS_SLOT_DATA_A, sim.now_ms));
	zexpect_equal(zlrs_link_plan(link).op, ZLRS_OP_IDLE);
	zexpect_equal(zlrs_test_slot(link, ZLRS_SLOT_DATA_A, sim.now_ms), nullptr);
	zexpect_equal(memcmp(req->buf, frozen.data(), frozen.size()), 0);
	const uint32_t first_seq = link->tx_seq - 1;
	zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0, 0, 0, sim.now_ms);
	/* A local failure retries next opportunity, even before the ACK timeout. */
	req = zlrs_test_slot(link, ZLRS_SLOT_DATA_A, sim.now_ms + 1);
	zassert_not_equal(req, nullptr);
	zassert_true(req->is_tx);
	zexpect_equal(req->buf + ZLRS_DATA_HEADROOM, payload);
	zexpect_equal(link->stats.tx_frame_builds, builds + 1);
	zlrs_frame_t frame;
	zassert_true(zlrs_frame_decode(&frame, req->buf, req->size, zlrs_profile_tag_len(param)));
	zexpect_true((frame.seq) > (first_seq));
	zexpect_equal(frame.body.data.header.data_seq, 0);
	zexpect_equal(memcmp(payload, bytes, sizeof(bytes)), 0);
	zlrs_link_complete(link, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms + 1);
	zexpect_equal(link->queue_count, sizeof(bytes));
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, LinkProfiles,
			       test_DirectPayloadAdmissionAndRetryKeepTheSameStorage,
			       LinkProfiles_params);

ZTEST_P(LinkProfiles, test_NextBufferWaitsForAckAndBecomesCurrentWithoutPayloadCopy)
{
	const auto param = ZTEST_GET_PARAM(LinkProfilesParam);
	sim_init(&sim);
	connect_saved(&sim, param);
	auto *link = &sim.leader.link;
	link->cfg.resync_every_n = 0;
	link->cfg.arq_timeout_ms = 1000;
	link->sess_cfm_due = false;
	const uint8_t first[] = {1, 2, 3};
	zassert_equal(zlrs_test_write(link, first, sizeof(first)), sizeof(first));
	const auto *req = zlrs_test_slot(link, ZLRS_SLOT_DATA_A, sim.now_ms);
	zassert_not_equal(req, nullptr);
	const uint8_t *first_buf = req->buf;
	zlrs_link_complete(link, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms);
	const uint8_t next[] = {4, 5, 6, 7};
	uint16_t capacity;
	uint8_t *payload = zlrs_link_tx_reserve(link, &capacity);
	zassert_not_equal(payload, nullptr);
	zassert_true((capacity) >= (sizeof(next)));
	memcpy(payload, next, sizeof(next));
	zlrs_link_tx_commit(link, sizeof(next));
	const uint8_t *second_buf = payload - ZLRS_DATA_HEADROOM;
	zexpect_not_equal(second_buf, first_buf);
	const auto builds = link->stats.tx_frame_builds;
	/* A is not acknowledged yet: B remains payload-only and cannot be selected. */
	req = zlrs_test_slot(link, ZLRS_SLOT_DATA_A, sim.now_ms + 1);
	zexpect_true(req == nullptr ||
		     req->size == ZLRS_DATA_HEADROOM + zlrs_profile_tag_len(param));
	if (req) {
		zlrs_link_complete(link, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms + 1);
	}
	zexpect_equal(link->stats.tx_frame_builds, builds);
	zlrs_frame_t ack = {};
	ack.type = ZLRS_PKT_DATA_F2L;
	ack.seq = sim.follower.link.tx_seq++;
	ack.body.data.header.ack = 1;
	inject(&sim.leader, &ack, ZLRS_DATA_PREFIX_LEN, link->session_key_in, sim.now_ms + 2,
	       false);
	zexpect_equal(link->queue_count, sizeof(next));
	req = zlrs_test_slot(link, ZLRS_SLOT_DATA_A, sim.now_ms + 2);
	zassert_not_equal(req, nullptr);
	zexpect_equal(req->buf, second_buf);
	zexpect_equal(memcmp(req->buf + ZLRS_DATA_HEADROOM, next, sizeof(next)), 0);
	zexpect_equal(link->stats.tx_frame_builds, builds + 1);
	zlrs_frame_t frame;
	zassert_true(zlrs_frame_decode(&frame, req->buf, req->size, zlrs_profile_tag_len(param)));
	zexpect_equal(frame.body.data.header.data_seq, 1);
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, LinkProfiles,
			       test_NextBufferWaitsForAckAndBecomesCurrentWithoutPayloadCopy,
			       LinkProfiles_params);

ZTEST(Link, test_PacketPoolKeepsByteBudgetWithRetainedHeadAndAppendableTail)
{
	sim_init(&sim);
	for (uint16_t mtu = 128; mtu <= 503; mtu++) {
		TC_PRINT("mtu=%u\n", (unsigned int)(mtu));
		auto cfg = sim.leader.link.cfg;
		cfg.data_mtu = mtu;
		cfg.profile = mtu > 499 ? ZLRS_PROFILE_P0 : ZLRS_PROFILE_P1;
		zlrs_link_t link;
		zlrs_link_init(&link, &cfg, nullptr, nullptr, nullptr);
		zlrs_fhss_init(&link.fhss, cfg.domain, 42);
		link.state = ZLRS_STATE_CONNECTED;
		link.session_valid = true;
		link.cfg.resync_every_n = 0;
		link.cfg.arq_timeout_ms = 1000;
		const uint8_t byte = 0x42;
		zassert_equal(zlrs_test_write(&link, &byte, 1), 1);
		const auto *req = zlrs_test_slot(&link, ZLRS_SLOT_DATA_A, 100);
		zassert_not_equal(req, nullptr);
		zlrs_link_complete(&link, ZLRS_RADIO_OK, 0, 0, 0, 100);
		zassert_equal(zlrs_test_write(&link, &byte, 1), 1);
		uint16_t size;
		zexpect_equal(zlrs_link_tx_space(&link), ZLRS_LINK_TX_QUEUE_LEN - 2);
		std::vector<uint8_t> bytes(ZLRS_LINK_TX_QUEUE_LEN - 2, 0xa5);
		zassert_equal(zlrs_test_write(&link, bytes.data(), bytes.size()), bytes.size());
		zexpect_equal(link.queue_count, ZLRS_LINK_TX_QUEUE_LEN);
		zexpect_equal(zlrs_link_tx_space(&link), 0);
		zexpect_equal(zlrs_link_tx_reserve(&link, &size), nullptr);
		zexpect_equal(size, 0);
	}
}

ZTEST(Link, test_CancelledDirectReservationDoesNotAdmitBytesOrLoseCapacity)
{
	sim_init(&sim);
	auto *link = &sim.leader.link;
	uint16_t capacity;
	uint8_t *payload = zlrs_link_tx_reserve(link, &capacity);
	zassert_not_equal(payload, nullptr);
	payload[0] = 0xa5;
	zlrs_link_tx_commit(link, 0);
	zexpect_equal(link->queue_count, 0);
	zexpect_equal(link->tx_packet_count, 0);
	zexpect_equal(zlrs_link_tx_space(link), ZLRS_LINK_TX_QUEUE_LEN);
	zexpect_equal(zlrs_link_tx_reserve(link, &capacity), payload);
	payload[0] = 0x42;
	zlrs_link_tx_commit(link, 1);
	zexpect_equal(link->queue_count, 1);
}

ZTEST_P(LinkProfiles, test_FreshSessionRetainsCurrentAndQueuedPayloadsInOrder)
{
	const auto param = ZTEST_GET_PARAM(LinkProfilesParam);
	sim_init(&sim);
	for (const auto role : {ZLRS_ROLE_LEADER, ZLRS_ROLE_FOLLOWER}) {
		TC_PRINT("role=%u\n", (unsigned int)(role));
		sim_init(&sim);
		connect_saved(&sim, param);
		auto *sender = role == ZLRS_ROLE_LEADER ? &sim.leader : &sim.follower;
		auto *receiver = role == ZLRS_ROLE_LEADER ? &sim.follower : &sim.leader;
		auto *link = &sender->link;
		link->cfg.resync_every_n = 0;
		link->sess_cfm_due = false;
		std::vector<uint8_t> bytes(600);
		for (size_t i = 0; i < bytes.size(); i++) {
			bytes[i] = static_cast<uint8_t>(i * 17 + 3);
		}
		zassert_equal(zlrs_test_write(link, bytes.data(), bytes.size()), bytes.size());
		const auto slot = role == ZLRS_ROLE_LEADER ? ZLRS_SLOT_DATA_A : ZLRS_SLOT_DATA_B;
		const auto *req = zlrs_test_slot(link, slot, sim.now_ms);
		zassert_not_equal(req, nullptr);
		zlrs_link_complete(link, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms);
		/* Local TX completed but no bytes reached the receiver, and no ACK arrived. */
		const auto retained = link->queue_count;
		link->tx_seq = UINT32_MAX;
		for (unsigned int i = 0;
		     i < 3000 && (link->queue_count || receiver->received_len < bytes.size());
		     i++) {
			sim_frame(&sim);
		}
		zexpect_equal(retained, bytes.size());
		zexpect_equal(link->queue_count, 0);
		zassert_equal(receiver->received_len, bytes.size());
		zexpect_equal(memcmp(receiver->received, bytes.data(), bytes.size()), 0);
	}
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, LinkProfiles,
			       test_FreshSessionRetainsCurrentAndQueuedPayloadsInOrder,
			       LinkProfiles_params);

ZTEST_P(LinkProfiles, test_RxResolvesSuccessorOrRetryBeforeFinalization)
{
	const auto param = ZTEST_GET_PARAM(LinkProfilesParam);
	sim_init(&sim);
	for (const bool acknowledged : {false, true}) {
		TC_PRINT("acknowledged=%u\n", (unsigned int)(acknowledged));
		sim_init(&sim);
		connect_saved(&sim, param);
		auto *link = &sim.leader.link;
		link->cfg.resync_every_n = 0;
		link->cfg.arq_timeout_ms = 1;
		link->sess_cfm_due = false;
		const uint8_t first[] = {1, 2, 3};
		const uint8_t next[] = {4, 5, 6, 7};
		zassert_equal(zlrs_test_write(link, first, sizeof(first)), sizeof(first));
		const auto *req = zlrs_test_slot(link, ZLRS_SLOT_DATA_A, sim.now_ms);
		zassert_not_equal(req, nullptr);
		const auto *first_buf = req->buf;
		zlrs_link_complete(link, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms);
		uint16_t capacity;
		uint8_t *payload = zlrs_link_tx_reserve(link, &capacity);
		zassert_not_equal(payload, nullptr);
		zassert_true((capacity) >= (sizeof(next)));
		memcpy(payload, next, sizeof(next));
		zlrs_link_tx_commit(link, sizeof(next));
		const auto *second_buf = payload - ZLRS_DATA_HEADROOM;
		const auto builds = link->stats.tx_frame_builds;
		req = zlrs_test_slot(link, ZLRS_SLOT_DATA_B, sim.now_ms + 1);
		zassert_not_equal(req, nullptr);
		zassert_false(req->is_tx);
		zexpect_equal(link->stats.tx_frame_builds, builds);
		if (acknowledged) {
			zlrs_frame_t ack = {};
			ack.type = ZLRS_PKT_DATA_F2L;
			ack.seq = sim.follower.link.tx_seq++;
			ack.body.data.header.ack = 1;
			inject(&sim.leader, &ack, ZLRS_DATA_PREFIX_LEN, link->session_key_in,
			       sim.now_ms + 1, false);
		} else {
			zlrs_link_complete(link, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms + 1);
		}
		req = zlrs_test_slot(link, ZLRS_SLOT_DATA_A, sim.now_ms + 2);
		zassert_not_equal(req, nullptr);
		zassert_true(req->is_tx);
		zexpect_equal(req->buf, acknowledged ? second_buf : first_buf);
		zexpect_equal(link->stats.tx_frame_builds, builds + 1);
		zlrs_frame_t frame;
		zassert_true(zlrs_frame_decode(&frame, req->buf, req->size,
					       zlrs_profile_tag_len(param)));
		zexpect_equal(frame.body.data.header.data_seq, acknowledged ? 1 : 0);
		zexpect_equal(memcmp(frame.body.data.bytes, acknowledged ? next : first,
				     acknowledged ? sizeof(next) : sizeof(first)),
			      0);
		zexpect_equal(memcmp(second_buf + ZLRS_DATA_HEADROOM, next, sizeof(next)), 0);
		zlrs_link_complete(link, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms + 2);
	}
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, LinkProfiles,
			       test_RxResolvesSuccessorOrRetryBeforeFinalization,
			       LinkProfiles_params);

ZTEST_P(LinkProfiles, test_PlanningIsReadOnlyAndOnlyLaunchStartsAckTimeout)
{
	const auto param = ZTEST_GET_PARAM(LinkProfilesParam);
	sim_init(&sim);
	connect_saved(&sim, param);
	auto *link = &sim.leader.link;
	link->cfg.resync_every_n = 0;
	link->cfg.arq_timeout_ms = 30;
	link->sess_cfm_due = false;
	const uint8_t bytes[] = {1, 2, 3};
	zassert_equal(zlrs_test_write(link, bytes, sizeof(bytes)), sizeof(bytes));
	zassert_true(zlrs_link_advance_slot(link, ZLRS_SLOT_DATA_A, sim.now_ms));
	const auto snapshot = std::make_unique<zlrs_link_t>(*link);
	zlrs_link_plan_t plan;
	for (unsigned int i = 0; i < 5; i++) {
		plan = zlrs_link_plan(link);
		zexpect_equal(plan.op, ZLRS_OP_TX);
		zexpect_equal(plan.radio.buf, nullptr);
		zexpect_equal(memcmp(link, snapshot.get(), sizeof(*link)), 0);
	}
	const auto *req = zlrs_link_finalize(link, &plan);
	zassert_not_equal(req, nullptr);
	zexpect_false(link->arq_launched);
	zexpect_equal(link->tx_seq, snapshot->tx_seq + 1);
	zexpect_equal(link->stats.tx_frame_builds, snapshot->stats.tx_frame_builds + 1);
	zexpect_equal(link->stats.tx_auth_tags,
		      snapshot->stats.tx_auth_tags + (param == ZLRS_PROFILE_P1));
	const uint32_t launch_ms = sim.now_ms + 100;
	zlrs_link_tx_started(link, launch_ms);
	zexpect_true(link->arq_launched);
	zexpect_equal(link->arq_first_sent_ms, launch_ms);
	zexpect_equal(link->arq_sent_ms, launch_ms);
	zlrs_link_tx_started(link, launch_ms + 1);
	zexpect_equal(link->arq_sent_ms, launch_ms);
	zlrs_link_complete(link, ZLRS_RADIO_OK, 0, 0, 0, launch_ms + 5);
	zexpect_equal(link->arq_sent_ms, launch_ms);
	zassert_true(zlrs_link_advance_slot(link, ZLRS_SLOT_DATA_A, launch_ms + 29));
	zexpect_true(zlrs_link_plan(link).arq_wait);
	zassert_true(zlrs_link_advance_slot(link, ZLRS_SLOT_DATA_A, launch_ms + 30));
	plan = zlrs_link_plan(link);
	zexpect_equal(plan.op, ZLRS_OP_TX);
	zexpect_false(plan.arq_wait);
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, LinkProfiles,
			       test_PlanningIsReadOnlyAndOnlyLaunchStartsAckTimeout,
			       LinkProfiles_params);

ZTEST_P(LinkProfiles, test_AckSurvivesCancelledPreparationAndCannotReleaseUnlaunchedData)
{
	const auto param = ZTEST_GET_PARAM(LinkProfilesParam);
	sim_init(&sim);
	connect_saved(&sim, param);
	auto *link = &sim.leader.link;
	link->cfg.resync_every_n = 0;
	link->sess_cfm_due = false;
	const uint8_t bytes[] = {3, 2, 1};
	zassert_equal(zlrs_test_write(link, bytes, sizeof(bytes)), sizeof(bytes));
	link->arq_ack_pending = true;
	const auto *req = zlrs_test_slot(link, ZLRS_SLOT_DATA_A, sim.now_ms);
	zassert_not_equal(req, nullptr);
	const auto accepted = link->accepted_rx_events;
	zlrs_frame_t ack = {.type = ZLRS_PKT_DATA_F2L,
			    .body = {.data = {.header = {.ack = 1}}},
			    .seq = sim.follower.link.tx_seq++};
	inject(&sim.leader, &ack, ZLRS_DATA_PREFIX_LEN, link->session_key_in, sim.now_ms, false);
	zexpect_equal(link->accepted_rx_events, accepted); /* TX is immutable while pending. */
	zexpect_true(zlrs_link_is_busy(link));
	zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0, 0, 0, sim.now_ms);
	zexpect_true(link->arq_ack_pending);
	inject(&sim.leader, &ack, ZLRS_DATA_PREFIX_LEN, link->session_key_in, sim.now_ms, false);
	zexpect_equal(link->queue_count, sizeof(bytes));
	zexpect_false(link->arq_launched);
	req = zlrs_test_slot(link, ZLRS_SLOT_DATA_A, sim.now_ms + 1);
	zassert_not_equal(req, nullptr);
	zlrs_link_tx_started(link, sim.now_ms + 1);
	zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0, 0, 0, sim.now_ms + 1);
	/* Once SET_TX was issued, even an ambiguous local failure may have reached peer. */
	ack.seq++;
	inject(&sim.leader, &ack, ZLRS_DATA_PREFIX_LEN, link->session_key_in, sim.now_ms + 2,
	       false);
	zexpect_equal(link->queue_count, 0);
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, LinkProfiles,
			       test_AckSurvivesCancelledPreparationAndCannotReleaseUnlaunchedData,
			       LinkProfiles_params);

ZTEST_P(LinkProfiles, test_PayloadAdmissionDuringTxLeavesTheActiveFrameFrozen)
{
	const auto param = ZTEST_GET_PARAM(LinkProfilesParam);
	sim_init(&sim);
	connect_saved(&sim, param);
	auto *link = &sim.leader.link;
	link->cfg.resync_every_n = 0;
	link->sess_cfm_due = false;
	const uint8_t first[] = {1, 2, 3};
	const uint8_t next[] = {4, 5, 6, 7};
	zassert_equal(zlrs_test_write(link, first, sizeof(first)), sizeof(first));
	const auto *req = zlrs_test_slot(link, ZLRS_SLOT_DATA_A, sim.now_ms);
	zassert_not_equal(req, nullptr);
	const std::vector<uint8_t> frozen(req->buf, req->buf + req->size);
	const auto builds = link->stats.tx_frame_builds;
	const auto tags = link->stats.tx_auth_tags;
	uint16_t capacity;
	auto *payload = zlrs_link_tx_reserve(link, &capacity);
	zassert_not_equal(payload, nullptr);
	zassert_true((capacity) >= (sizeof(next)));
	zexpect_not_equal(payload, req->buf + ZLRS_DATA_HEADROOM + sizeof(first));
	memcpy(payload, next, sizeof(next));
	zlrs_link_tx_commit(link, sizeof(next));
	zexpect_equal(memcmp(req->buf, frozen.data(), frozen.size()), 0);
	zexpect_equal(link->stats.tx_frame_builds, builds);
	zexpect_equal(link->stats.tx_auth_tags, tags);
	zexpect_equal(link->tx_packet_count, 2);
	zexpect_equal(link->queue_count, sizeof(first) + sizeof(next));
	zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0, 0, 0, sim.now_ms);
	zexpect_equal(link->stats.tx_cancelled_frames, 1U);
	zexpect_true(link->arq_local_failed);
	zexpect_equal(link->queue_count, sizeof(first) + sizeof(next));
	zexpect_equal(memcmp(payload, next, sizeof(next)), 0);
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, LinkProfiles,
			       test_PayloadAdmissionDuringTxLeavesTheActiveFrameFrozen,
			       LinkProfiles_params);

ZTEST_SUITE(AutomaticBandLink, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(FinalizedAck, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(Link, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(LinkProfiles, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(PairingCommit, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(PowerSymmetry, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(QueueFailure, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(StreamFaults, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(TxAccounting, NULL, NULL, NULL, NULL, NULL);
