/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include "power.h"

ZTEST(power, test_data_feedback_lowers_and_timeout_restores_ceiling)
{
	zlrs_power_t power;
	zlrs_power_init(&power, 20, 0, 0U);
	zlrs_power_update(&power, -60, 100U, 1U, 1U);
	for (uint32_t now_ms = 40001U; now_ms <= 40003U; now_ms++) {
		zlrs_power_update(&power, -60, 100U, now_ms, now_ms);
	}
	zassert_equal(power.current_dbm, 17);
	zassert_equal(power.lowers, 1U);
	zassert_equal(zlrs_power_update(&power, -60, 100U, 40003U, 41002U), 17);
	zassert_equal(zlrs_power_update(&power, -60, 100U, 40003U, 41003U), 20);
	zassert_equal(power.fallbacks, 1U);
}

ZTEST(power, test_packet_loss_raises_power_with_strong_rssi)
{
	zlrs_power_t power;
	zlrs_power_init(&power, 10, 0, 0U);
	zlrs_power_update(&power, -60, 100U, 1U, 1U);
	for (uint32_t step = 1U; step <= 4U; step++) {
		for (uint32_t sample = 0U; sample < 3U; sample++) {
			const uint32_t now_ms = step * 42000U + sample;
			zlrs_power_update(&power, -60, 100U, now_ms, now_ms);
		}
	}
	zassert_equal(power.current_dbm, 0);
	for (uint32_t now_ms = 170002U; now_ms <= 170004U; now_ms++) {
		zlrs_power_update(&power, -60, 98U, now_ms, now_ms);
	}
	zassert_equal(power.current_dbm, 3);
	zassert_equal(power.raises, 1U);
}

ZTEST_SUITE(power, NULL, NULL, NULL, NULL, NULL);
