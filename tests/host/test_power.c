/* SPDX-License-Identifier: Apache-2.0 */
#include "test.h"
#include "power.h"

static int8_t observe(zlrs_power_t *power, int16_t rssi, uint8_t lq, uint32_t now)
{
	return zlrs_power_update(power, rssi, lq, now, now);
}

static void test_quality_policy(void)
{
	zlrs_power_t power;
	zlrs_power_init(&power, 10, 0, 0U);
	observe(&power, -60, 100U, 2000U);
	observe(&power, -60, 100U, 3000U);
	CHECK(observe(&power, -60, 100U, 4000U) == 10);

	for (uint32_t step = 1U; step <= 4U; step++) {
		for (uint32_t sample = 0U; sample < 3U; sample++) {
			observe(&power, -60, 100U, step * 42000U + sample);
		}
	}
	CHECK(power.current_dbm == 0);
	observe(&power, -60, 98U, 170002U);
	observe(&power, -60, 98U, 170003U);
	CHECK(observe(&power, -60, 98U, 170004U) == 3);

	/* Boot/search time without peer feedback must not satisfy the lowering hold. */
	zlrs_power_init(&power, 10, 0, 1000U);
	observe(&power, -60, 100U, 51000U);
	observe(&power, -60, 100U, 52000U);
	CHECK(observe(&power, -60, 100U, 53000U) == 10);
}

void test_power(void)
{
	zlrs_power_t power;
	printf("power: fresh feedback, limits, hold, timeout and clock wrap\n");
	zlrs_power_init(&power, 20, 0, 10000U);
	CHECK(zlrs_power_update(&power, -60, 100U, 0U, 11000U) == 20);
	CHECK(observe(&power, -60, 100U, 11000U) == 20);
	/* Start this run at the first feedback reception. */
	zlrs_power_init(&power, 20, 0, 10000U);
	CHECK(observe(&power, -60, 100U, 10000U) == 20);
	CHECK(observe(&power, -60, 100U, 49999U) == 20);
	CHECK(observe(&power, -60, 100U, 50000U) == 20);
	/* Reading the same feedback repeatedly cannot fill the consecutive sample count. */
	CHECK(zlrs_power_update(&power, -60, 100U, 50000U, 50001U) == 20);
	CHECK(zlrs_power_update(&power, -60, 100U, 50000U, 50002U) == 20);
	CHECK(observe(&power, -60, 100U, 50001U) == 20);
	CHECK(observe(&power, -60, 100U, 50002U) == 17);
	CHECK(power.lowers == 1U);
	CHECK(observe(&power, -100, 80U, 50003U) == 17);
	CHECK(observe(&power, -100, 80U, 52002U) == 17);
	CHECK(observe(&power, -60, 98U, 52003U) == 17);
	CHECK(observe(&power, -96, 100U, 52004U) == 20);
	CHECK(power.raises == 1U);

	/* Exactly 99% holds power; RSSI threshold equality also interrupts good samples. */
	CHECK(observe(&power, -60, 100U, 92004U) == 20);
	CHECK(observe(&power, -70, 100U, 92005U) == 20);
	CHECK(observe(&power, -60, 100U, 92006U) == 20);
	CHECK(observe(&power, -95, 99U, 92007U) == 20);
	CHECK(observe(&power, -69, 100U, 92008U) == 20);
	CHECK(observe(&power, -69, 100U, 92009U) == 20);
	CHECK(observe(&power, -69, 100U, 92010U) == 17);
	/* Timeout uses reception time, not the time the caller first saw the record. */
	CHECK(zlrs_power_update(&power, -60, 100U, 92010U, 93009U) == 17);
	CHECK(zlrs_power_update(&power, -60, 100U, 92010U, 93010U) == 20);
	CHECK(power.fallbacks == 1U);
	CHECK(zlrs_power_update(&power, -60, 100U, 92010U, 93011U) == 20);
	CHECK(power.fallbacks == 1U);

	zlrs_power_init(&power, 1, 0, 0U);
	observe(&power, -60, 100U, 1U);
	for (uint32_t ms = 40000U; ms < 40006U; ++ms) {
		observe(&power, -60, 100U, ms);
	}
	CHECK(power.current_dbm == 0);
	CHECK(power.lowers == 1U);
	for (uint32_t ms = 42002U; ms < 42008U; ++ms) {
		observe(&power, -100, 80U, ms);
	}
	CHECK(power.current_dbm == 1);
	CHECK(power.raises == 1U);
	zlrs_power_init(&power, -10, 0, 0U);
	observe(&power, -60, 100U, 1U);
	for (uint32_t ms = 40000U; ms < 40006U; ++ms) {
		observe(&power, -60, 100U, ms);
	}
	CHECK(power.current_dbm == -10);
	CHECK(power.lowers == 0U);

	zlrs_power_init(&power, 10, -10, UINT32_MAX - 1000U);
	observe(&power, -60, 100U, UINT32_MAX - 1000U);
	CHECK(observe(&power, -60, 100U, 38998U) == 10);
	CHECK(observe(&power, -60, 100U, 38999U) == 10);
	CHECK(observe(&power, -60, 100U, 39000U) == 10);
	CHECK(observe(&power, -60, 100U, 39001U) == 7);
	CHECK(zlrs_power_update(&power, -60, 100U, 39001U, 40001U) == 10);
	CHECK(power.fallbacks == 1U);
	/* Feedback before wrap remains fresh across wrap. */
	zlrs_power_init(&power, 10, -10, UINT32_MAX - 3000U);
	observe(&power, -60, 100U, UINT32_MAX - 3000U);
	CHECK(zlrs_power_update(&power, -60, 100U, UINT32_MAX - 2U, 1U) == 10);
	CHECK(observe(&power, -60, 100U, 36999U) == 10);
	CHECK(observe(&power, -60, 100U, 37000U) == 10);
	CHECK(observe(&power, -60, 100U, 37001U) == 7);
	test_quality_policy();
}
