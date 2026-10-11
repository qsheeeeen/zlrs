/* SPDX-License-Identifier: Apache-2.0 */
#include "test.h"
#include "power.h"

static int8_t observe(zlrs_power_t *power, int16_t rssi, uint8_t lq, uint32_t now)
{
	return zlrs_power_update(power, rssi, lq, now, now);
}

void test_power(void)
{
	zlrs_power_t power;
	printf("power: fresh feedback, limits, hold, timeout and clock wrap\n");
	zlrs_power_init(&power, 20, 0, 10000U);
	CHECK(zlrs_power_update(&power, -60, 100U, 0U, 13000U) == 20);
	CHECK(observe(&power, -60, 100U, 11999U) == 20);
	CHECK(observe(&power, -60, 100U, 12000U) == 20);
	/* Reading the same feedback repeatedly cannot fill quality windows. */
	CHECK(zlrs_power_update(&power, -60, 100U, 12000U, 12001U) == 20);
	CHECK(zlrs_power_update(&power, -60, 100U, 12000U, 12002U) == 20);
	CHECK(observe(&power, -60, 100U, 12003U) == 20);
	CHECK(observe(&power, -60, 100U, 12004U) == 17);
	CHECK(power.lowers == 1U);
	CHECK(observe(&power, -100, 80U, 12005U) == 17);
	CHECK(observe(&power, -100, 80U, 14004U) == 17);
	CHECK(observe(&power, -60, 89U, 14005U) == 17);
	CHECK(observe(&power, -96, 100U, 14006U) == 20);
	CHECK(power.raises == 1U);

	/* Threshold equality and middling quality interrupt consecutive windows. */
	CHECK(observe(&power, -60, 100U, 16006U) == 20);
	CHECK(observe(&power, -70, 100U, 16007U) == 20);
	CHECK(observe(&power, -60, 98U, 16008U) == 20);
	CHECK(observe(&power, -95, 90U, 16009U) == 20);
	CHECK(observe(&power, -69, 98U, 16010U) == 20);
	CHECK(observe(&power, -69, 98U, 16011U) == 20);
	CHECK(observe(&power, -69, 98U, 16012U) == 17);
	/* Timeout uses reception time, not the time the caller first saw the record. */
	CHECK(zlrs_power_update(&power, -60, 100U, 16012U, 17011U) == 17);
	CHECK(zlrs_power_update(&power, -60, 100U, 16012U, 17012U) == 20);
	CHECK(power.fallbacks == 1U);
	CHECK(zlrs_power_update(&power, -60, 100U, 16012U, 17013U) == 20);
	CHECK(power.fallbacks == 1U);

	zlrs_power_init(&power, 1, 0, 0U);
	for (uint32_t ms = 2000U; ms < 2006U; ++ms) {
		observe(&power, -60, 100U, ms);
	}
	CHECK(power.current_dbm == 0);
	CHECK(power.lowers == 1U);
	for (uint32_t ms = 4002U; ms < 4008U; ++ms) {
		observe(&power, -100, 80U, ms);
	}
	CHECK(power.current_dbm == 1);
	CHECK(power.raises == 1U);
	zlrs_power_init(&power, -10, 0, 0U);
	for (uint32_t ms = 2000U; ms < 2006U; ++ms) {
		observe(&power, -60, 100U, ms);
	}
	CHECK(power.current_dbm == -10);
	CHECK(power.lowers == 0U);

	zlrs_power_init(&power, 10, -10, UINT32_MAX - 1000U);
	CHECK(observe(&power, -60, 100U, 998U) == 10);
	CHECK(observe(&power, -60, 100U, 999U) == 10);
	CHECK(observe(&power, -60, 100U, 1000U) == 10);
	CHECK(observe(&power, -60, 100U, 1001U) == 7);
	CHECK(zlrs_power_update(&power, -60, 100U, 1001U, 2001U) == 10);
	CHECK(power.fallbacks == 1U);
	/* Feedback before wrap remains fresh across wrap. */
	zlrs_power_init(&power, 10, -10, UINT32_MAX - 3000U);
	CHECK(zlrs_power_update(&power, -60, 100U, UINT32_MAX - 2U, 1U) == 10);
	CHECK(observe(&power, -60, 100U, 2U) == 10);
	CHECK(observe(&power, -60, 100U, 3U) == 7);
}
