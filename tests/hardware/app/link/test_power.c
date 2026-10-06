/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>

#include "link/power.h"

static zlrs_link_t control;

ZTEST(power, test_power_tracks_telemetry_and_falls_back)
{
	zlrs_link_cfg_t cfg = {
		.kind = ZLRS_LINK_CONTROL,
		.role = ZLRS_ROLE_TX,
		.domain = ZLRS_DOMAIN_ISM2400,
		.second_domain = 0xffU,
		.profile = ZLRS_PROFILE_P1,
		.data_mtu = 64U,
		.tx_power_dbm = 20,
		.mod = zlrs_mod_flrc(1300000U),
	};
	int8_t current_dbm;
	uint32_t lowers;
	uint32_t fallbacks;

	zlrs_link_init(&control, &cfg, NULL, NULL, NULL);
	zlrs_link_power_init(20, 0);
	for (uint32_t now_ms = 2000U; now_ms <= 2002U; now_ms++) {
		zlrs_link_power_telemetry(60U, 100U);
		zlrs_link_power_tick(&control, now_ms);
	}
	zassert_equal(control.cfg.tx_power_dbm, 17);
	zlrs_link_power_stats(&current_dbm, NULL, &lowers, &fallbacks);
	zassert_equal(current_dbm, 17);
	zassert_equal(lowers, 1U);

	zlrs_link_power_tick(&control, 3002U);
	zassert_equal(control.cfg.tx_power_dbm, 20);
	zlrs_link_power_stats(&current_dbm, NULL, &lowers, &fallbacks);
	zassert_equal(current_dbm, 20);
	zassert_equal(fallbacks, 1U);
}

ZTEST_SUITE(power, NULL, NULL, NULL, NULL, NULL);
