/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include "power.h"

ZTEST(power, test_data_feedback_lowers_and_timeout_restores_ceiling)
{
	zlrs_power_t power;
	zlrs_power_init(&power, 20, 0, 0U);
	zlrs_power_observation_t o = {.rssi_dbm = -60, .rssi_floor_dbm = -89, .lq_percent = 127};
	for (uint32_t now_ms = 1U; now_ms <= 2501U; now_ms += 250U) {
		o.now_ms = o.feedback_ms = now_ms;
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
	zlrs_power_observation_t o = {.rssi_dbm = -60, .rssi_floor_dbm = -89, .lq_percent = 98};
	for (uint32_t now_ms = 1U; now_ms <= 15001U; now_ms += 250U) {
		o.now_ms = o.feedback_ms = now_ms;
		zlrs_power_update(&power, &o);
	}
	zassert_equal(power.current_dbm, 0);
	o.lq_percent = 90;
	o.lq_generation = 1;
	o.now_ms = o.feedback_ms = 15251U;
	zassert_equal(zlrs_power_update(&power, &o), 3);
	for (uint32_t now_ms = 15501U; now_ms <= 20001U; now_ms += 250U) {
		o.now_ms = o.feedback_ms = now_ms;
		zlrs_power_update(&power, &o);
	}
	zassert_equal(power.current_dbm, 0);
	zassert_equal(power.raises, 1U);
}

ZTEST_SUITE(power, NULL, NULL, NULL, NULL, NULL);
