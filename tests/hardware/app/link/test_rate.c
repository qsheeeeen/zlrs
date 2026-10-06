/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>

#include "link/rate.h"

static zlrs_link_t control;

ZTEST(rate, test_rate_adapts_after_hold_and_windows)
{
	zlrs_link_cfg_t cfg = {
		.kind = ZLRS_LINK_CONTROL,
		.role = ZLRS_ROLE_TX,
		.domain = ZLRS_DOMAIN_ISM2400,
		.second_domain = 0xffU,
		.profile = ZLRS_PROFILE_P1,
		.data_mtu = 64U,
		.mod = zlrs_mod_flrc(1300000U),
	};

	zlrs_link_init(&control, &cfg, NULL, NULL, NULL);
	zlrs_link_rate_init(&cfg.mod);
	control.stats.lq_percent = 20U;
	zlrs_link_rate_tick(&control, 2000U);
	zassert_equal(control.cfg.mod.rate_bps, 1300000U);
	zlrs_link_rate_tick(&control, 2001U);
	zassert_equal(control.cfg.mod.rate_bps, 1300000U);
	zlrs_link_rate_tick(&control, 2002U);
	zassert_equal(control.cfg.mod.rate_bps, 650000U);

	control.stats.lq_percent = 100U;
	zlrs_link_rate_tick(&control, 4002U);
	zlrs_link_rate_tick(&control, 4003U);
	zlrs_link_rate_tick(&control, 4004U);
	zassert_equal(control.cfg.mod.rate_bps, 1300000U);
}

ZTEST_SUITE(rate, NULL, NULL, NULL, NULL, NULL);
