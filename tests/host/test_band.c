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
					 .power_at_max = true,
					 .rssi_dbm = -60,
					 .peer_rssi_dbm = -60,
					 .lq_percent = 100,
					 .peer_lq_percent = 100};
}

void test_band(void)
{
	printf("band: 2.4 GHz ladder, per-PHY margins, hysteresis and recovery\n");
	zlrs_band_policy_t p, follower;
	static const int16_t floors[] = {-89, -94, -98, -103, -106};
	for (unsigned int phy = 0; phy < ZLRS_PHY_AUTO_COUNT; phy++) {
		zlrs_band_policy_init(&p, true, true, false, 0, 0);
		p.phy = phy;
		zlrs_band_observation_t o = good(100);
		o.rssi_dbm = floors[phy];
		o.peer_rssi_dbm = floors[phy];
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
		o = good(200);
		o.peer_rssi_dbm = floors[phy] - 1;
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_BOOST);
		o = good(201);
		o.peer_rssi_dbm = floors[phy] - 1;
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
		o = good(2200);
		o.peer_rssi_dbm = floors[phy] - 1;
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
		o = good(2201);
		o.peer_rssi_dbm = floors[phy] - 1;
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_PLAN);
		CHECK(p.target_subg == (phy == ZLRS_PHY_LORA_SF7));
		CHECK(p.target_phy == (phy == ZLRS_PHY_LORA_SF7 ? 0U : phy + 1U));
		/* LQ alone also qualifies; stale feedback breaks qualification. */
		o = good(2300);
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
		o = good(2400);
		o.peer_lq_percent = 94;
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_BOOST);
		o = good(4300);
		o.peer_lq_percent = 94;
		o.feedback_ms = 1;
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
		o = good(4400);
		o.peer_lq_percent = 94;
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
		o = good(6400);
		o.peer_lq_percent = 94;
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_PLAN);
		p.leader = false;
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
		p.automatic = false;
		o.now_ms += 100000;
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	}
	/* Either direction boosts both roles, including RSSI -90 with perfect LQ.
	 * Only fresh post-boost feedback at maximum starts the two-second hold. */
	for (unsigned int leader = 0; leader < 2; leader++) {
		for (unsigned int direction = 0; direction < 2; direction++) {
			zlrs_band_policy_init(&p, true, leader, false, 0, 0);
			zlrs_band_observation_t o = good(100);
			o.power_at_max = false;
			if (direction == 0) {
				o.rssi_dbm = -90;
			} else {
				o.peer_rssi_dbm = -90;
			}
			CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_BOOST);
			CHECK(p.power_boosted && !p.bad);
			o.now_ms = o.last_rx_ms = o.feedback_ms = 150;
			CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_BOOST);
			o.now_ms = o.last_rx_ms = 200;
			o.power_at_max = true;
			CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
			CHECK(!p.bad); /* Feedback predates reaching maximum power. */
			o.now_ms = o.last_rx_ms = o.feedback_ms = 300;
			CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
			o.now_ms = o.last_rx_ms = o.feedback_ms = 2299;
			CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
			o.now_ms = o.last_rx_ms = o.feedback_ms = 2300;
			CHECK(zlrs_band_policy_update(&p, &o) ==
			      (leader ? ZLRS_BAND_PLAN : ZLRS_BAND_KEEP));
			o = good(2400);
			CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
			CHECK(!p.power_boosted && !p.bad);
		}
	}
	/* Weak-but-healthy slow 2.4 GHz links stay on that band. */
	zlrs_band_policy_init(&p, true, true, false, 0, 0);
	p.phy = ZLRS_PHY_FLRC_325;
	for (uint32_t t = 100; t < 100000; t += 100) {
		zlrs_band_observation_t o = good(t);
		o.rssi_dbm = o.peer_rssi_dbm = -97;
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	}
	/* Upgrades need margin for the faster profile, then a continuous minute. */
	zlrs_band_policy_changed(&p, false, ZLRS_PHY_LORA_SF7, false, 0, 0);
	zlrs_band_observation_t o = good(100);
	o.rssi_dbm = o.peer_rssi_dbm = -97;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	o = good(60099);
	o.rssi_dbm = o.peer_rssi_dbm = -97;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	o = good(60100);
	o.rssi_dbm = o.peer_rssi_dbm = -97;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_PLAN);
	CHECK(!p.target_subg && p.target_phy == ZLRS_PHY_LORA_SF6);
	/* Contact loss traverses every 2.4 GHz profile before SubG. */
	for (unsigned int phy = 0; phy < ZLRS_PHY_AUTO_COUNT; phy++) {
		zlrs_band_policy_init(&p, true, true, false, 0, 1000);
		p.phy = phy;
		o = good(1000);
		o.last_rx_ms = 0;
		o.now_ms = 2500;
		CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_SEARCH);
		CHECK(p.target_subg == (phy == ZLRS_PHY_LORA_SF7));
	}
	zlrs_band_policy_init(&follower, true, false, false, 0, 0);
	CHECK(p.dwell_ms > (ZLRS_PHY_AUTO_COUNT + 1U) * follower.dwell_ms);
	/* Healthy SubG probes robust 2.4 GHz; failed capture resumes the full sweep. */
	zlrs_band_policy_changed(&p, true, 0, false, 0, 0);
	o = good(100);
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	o = good(60100);
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_PLAN);
	CHECK(!p.target_subg && p.target_phy == ZLRS_PHY_LORA_SF7);
	zlrs_band_policy_changed(&p, true, 0, false, 0, 0);
	o = good(28000);
	o.acquiring = true;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_SEARCH);
	CHECK(!p.target_subg && p.target_phy == ZLRS_PHY_FLRC_2600);
	zlrs_band_policy_init(&p, true, true, true, 60000, 0);
	zlrs_band_policy_init(&follower, true, false, true, 60000, 0);
	CHECK(p.loss_ms == 62000 && follower.dwell_ms == 62000);
	CHECK(p.dwell_ms > (ZLRS_PHY_AUTO_COUNT + 1U) * follower.dwell_ms);
	o = good(0);
	o.now_ms = 61999;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	o.now_ms = 62000;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_SEARCH);
	zlrs_band_policy_init(&p, true, true, false, 0, UINT32_MAX - 1000U);
	o = good(0);
	o.last_rx_ms = 0;
	o.now_ms = 498;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	o.now_ms = 499;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_SEARCH);
	o.pairing = true;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	/* Post-boost feedback and downgrade qualification also cross clock wrap. */
	zlrs_band_policy_init(&p, true, true, false, 0, UINT32_MAX - 50U);
	o = good(UINT32_MAX - 49U);
	o.peer_rssi_dbm = -90;
	o.power_at_max = false;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_BOOST);
	o = good(10);
	o.peer_rssi_dbm = -90;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	o = good(2009);
	o.peer_rssi_dbm = -90;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_KEEP);
	o = good(2010);
	o.peer_rssi_dbm = -90;
	CHECK(zlrs_band_policy_update(&p, &o) == ZLRS_BAND_PLAN);
}
