/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include "power.h"

ZTEST(power, test_data_feedback_lowers_and_timeout_restores_ceiling)
{
	zlrs_power_t power;
	zlrs_power_init(&power, 20, 0, 0U);
	zlrs_power_update(&power, -60, 100U, 1U, 1U);
	for (uint32_t now_ms = 251U; now_ms <= 40001U; now_ms += 250U) {
		zlrs_power_update(&power, -60, 100U, now_ms, now_ms);
	}
	zassert_equal(power.current_dbm, 17);
	zassert_equal(power.lowers, 1U);
	zassert_equal(zlrs_power_update(&power, -60, 100U, 40001U, 41000U), 17);
	zassert_equal(zlrs_power_update(&power, -60, 100U, 40001U, 41001U), 20);
	zassert_equal(power.fallbacks, 1U);
}

ZTEST(power, test_packet_loss_raises_power_with_strong_rssi)
{
	zlrs_power_t power;
	zlrs_power_init(&power, 10, 0, 0U);
	zlrs_power_update(&power, -60, 100U, 1U, 1U);
	for (uint32_t now_ms = 251U; now_ms <= 168001U; now_ms += 250U) {
		zlrs_power_update(&power, -60, 100U, now_ms, now_ms);
	}
	zassert_equal(power.current_dbm, 0);
	for (uint32_t now_ms = 168002U; now_ms <= 168004U; now_ms++) {
		zlrs_power_update(&power, -60, 98U, now_ms, now_ms);
	}
	zassert_equal(power.current_dbm, 3);
	zassert_equal(power.raises, 1U);
}

ZTEST_SUITE(power, NULL, NULL, NULL, NULL, NULL);
