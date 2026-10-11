/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include "power.h"

ZTEST(power, test_data_feedback_lowers_and_timeout_restores_ceiling)
{
	zlrs_power_t power;
	zlrs_power_init(&power, 20, 0, 0U);
	for (uint32_t now_ms = 2000U; now_ms <= 2002U; now_ms++) {
		zlrs_power_update(&power, -60, 100U, now_ms, now_ms);
	}
	zassert_equal(power.current_dbm, 17);
	zassert_equal(power.lowers, 1U);
	zassert_equal(zlrs_power_update(&power, -60, 100U, 2002U, 3001U), 17);
	zassert_equal(zlrs_power_update(&power, -60, 100U, 2002U, 3002U), 20);
	zassert_equal(power.fallbacks, 1U);
}

ZTEST_SUITE(power, NULL, NULL, NULL, NULL, NULL);
