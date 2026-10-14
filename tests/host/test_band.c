/* Product-policy acceptance without a radio, including clock wrap and duty silence.
 * SPDX-License-Identifier: Apache-2.0 */
#include "band.h"
#include "test.h"

static zlrs_band_observation_t good(uint32_t now)
{
	return (zlrs_band_observation_t){.now_ms = now,
					 .last_rx_ms = now,
					 .feedback_ms = now,
					 .paired = true,
					 .connected = true,
					 .rssi_dbm = -60,
					 .peer_rssi_dbm = -60,
					 .lq_percent = 100,
					 .peer_lq_percent = 100};
}

void test_band(void)
{
	printf("band: hysteresis, authority, recovery, probe and duty silence\n");
	zlrs_band_policy_t p;
	zlrs_band_policy_init(&p, true, true, false, 0, 0);
	zlrs_band_observation_t o = good(100);
	o.peer_lq_percent = 94;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	for (uint32_t time = 200; time < 5100; time += 100) {
		o = good(time);
		o.peer_lq_percent = 94;
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	}
	o = good(5100);
	o.peer_lq_percent = 94;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_PLAN);
	/* A brief fade followed by valid healthy feedback does not accumulate. */
	o = good(5200);
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	o = good(5300);
	o.rssi_dbm = -96;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	o = good(10200);
	o.rssi_dbm = -96;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	o = good(10300);
	o.rssi_dbm = -96;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_PLAN);
	/* Follower never originates a quality-driven transition; fixed mode never searches. */
	p.leader = false;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	p.automatic = false;
	o.now_ms += 100000;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	zlrs_band_policy_init(&p, true, true, false, 0, 1000);
	o = good(1000);
	o.now_ms = 2499;
	o.last_rx_ms = 0;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	o.now_ms = 2500;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_SEARCH);
	/* Different dwell periods prevent synchronized opposite-band searching. */
	zlrs_band_policy_t receiver;
	zlrs_band_policy_init(&receiver, true, false, false, 0, 0);
	CHECK(p.dwell_ms == 20000 && receiver.dwell_ms == 4000);
	zlrs_band_policy_changed(&p, true, false, 2500, 0);
	o.acquiring = true;
	o.now_ms = 22499;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	o.now_ms = 22500;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_SEARCH);
	/* A healthy SubG link probes only after an uninterrupted minute. */
	zlrs_band_policy_changed(&p, true, false, 30000, 0);
	for (uint32_t time = 30100; time < 90100; time += 500) {
		o = good(time);
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	}
	o = good(90100);
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_PLAN);
	zlrs_band_policy_changed(&p, false, true, 100000, 0);
	o = good(100100);
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	o = good(105099);
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP && p.probing);
	o = good(105100);
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP && !p.probing);
	zlrs_band_policy_changed(&p, false, true, 110000, o.last_rx_ms);
	o.acquiring = true;
	o.now_ms = 111500;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_SEARCH);
	/* Budget-induced silence is legal; switching cannot accelerate this timeout. */
	zlrs_band_policy_init(&p, true, true, true, 60000, 0);
	zlrs_band_policy_init(&receiver, true, false, true, 60000, 0);
	CHECK(p.loss_ms == 62000 && p.dwell_ms > 2U * 60000U);
	CHECK(receiver.dwell_ms == 62000 && p.dwell_ms >= 2U * receiver.dwell_ms);
	o = good(0);
	o.now_ms = 61999;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	o.now_ms = 62000;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_SEARCH);
	zlrs_band_policy_init(&p, true, true, false, 0, UINT32_MAX - 1000U);
	o = good(0);
	o.now_ms = 498;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	o.now_ms = 499;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_SEARCH);
	o.pairing = true;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
}
