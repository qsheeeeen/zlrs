/* SPDX-License-Identifier: Apache-2.0 */
#include "power.h"
#include <zephyr/ztest.h>

ZTEST(power, test_data_feedback_lowers_and_timeout_restores_ceiling)
{
	zlrs_power_t power;
	zlrs_power_init(&power, 20, 0, 0U);
	zlrs_link_status_t link = {
		.cfg = {.region = ZLRS_REGION_ISM2400, .phy = ZLRS_PHY_FLRC_2600},
		.peer_rssi_dbm = -60,
		.peer_lq_percent = 127};
	zlrs_link_observation_t o = {.link = &link};
	for (uint32_t now_ms = 1U; now_ms <= 2501U; now_ms += 250U) {
		o.now_ms = link.last_peer_feedback_ms = now_ms;
		zlrs_power_update(&power, &o);
	}
	zassert_equal(power.current_dbm, 17);
	zassert_equal(power.lowers, 1U);
	o.now_ms = 3500U;
	zassert_equal(zlrs_power_update(&power, &o), 17);
	o.now_ms = 3501U;
	zassert_equal(zlrs_power_update(&power, &o), 20);
	zassert_equal(power.fallbacks, 1U);
}

ZTEST(power, test_cached_packet_loss_does_not_repeat_power_raises)
{
	zlrs_power_t power;
	zlrs_power_init(&power, 10, 0, 0U);
	zlrs_link_status_t link = {
		.cfg = {.region = ZLRS_REGION_ISM2400, .phy = ZLRS_PHY_FLRC_2600},
		.peer_rssi_dbm = -60,
		.peer_lq_percent = 98};
	zlrs_link_observation_t o = {.link = &link};
	for (uint32_t now_ms = 1U; now_ms <= 15001U; now_ms += 250U) {
		o.now_ms = link.last_peer_feedback_ms = now_ms;
		zlrs_power_update(&power, &o);
	}
	zassert_equal(power.current_dbm, 0);
	link.peer_lq_percent = 90;
	link.peer_lq_generation = 1;
	o.now_ms = link.last_peer_feedback_ms = 15251U;
	zassert_equal(zlrs_power_update(&power, &o), 3);
	for (uint32_t now_ms = 15501U; now_ms <= 20001U; now_ms += 250U) {
		o.now_ms = link.last_peer_feedback_ms = now_ms;
		zlrs_power_update(&power, &o);
	}
	zassert_equal(power.current_dbm, 0);
	zassert_equal(power.raises, 1U);
}

ZTEST_SUITE(power, NULL, NULL, NULL, NULL, NULL);
