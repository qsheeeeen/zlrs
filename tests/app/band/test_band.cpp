/* SPDX-License-Identifier: Apache-2.0 */
#include <initializer_list>
#include <tuple>
#include <zephyr/ztest.h>

extern "C" {
#include "band.h"
}

namespace
{
struct BandSample {
	zlrs_link_status_t link;
	zlrs_power_t power;
	uint32_t now_ms, settle_ms;
};

zlrs_band_action_t evaluate(zlrs_band_policy_t *policy, const BandSample *sample)
{
	const zlrs_link_observation_t observation = {
		.link = &sample->link,
		.now_ms = sample->now_ms,
		.settle_ms = sample->settle_ms,
	};
	return zlrs_band_policy_update(policy, &observation, &sample->power);
}

void good(uint32_t now, BandSample *o)
{
	*o = {};
	o->link.state = ZLRS_STATE_CONNECTED;
	o->power.ceiling_dbm = 1;
	o->now_ms = o->link.last_rx_ms = o->link.last_peer_feedback_ms = now;
	o->link.paired = o->link.session_valid = o->link.cfg.tx_power_dbm = true;
	o->link.stats.last_rssi = o->link.peer_rssi_dbm = -60;
	o->link.stats.lq_percent = o->link.peer_lq_percent = 100;
}

zlrs_band_policy_t p{};

void reset_BandPolicy()
{
	p = {};
	zlrs_band_policy_init(&p, true, ZLRS_ROLE_LEADER, false, 0, 0);
}

struct PhyMargin {
	zlrs_phy_t phy;
	int16_t floor;
	const char *name;
};

using BandProfilesParam = PhyMargin;

static const BandProfilesParam BandProfiles_values[] = {
	PhyMargin{ZLRS_PHY_FLRC_2600, -89, "Flrc2600"},
	PhyMargin{ZLRS_PHY_FLRC_650, -94, "Flrc650"},
	PhyMargin{ZLRS_PHY_FLRC_325, -98, "Flrc325"},
	PhyMargin{ZLRS_PHY_LORA_SF6, -103, "LoraSf6"},
	PhyMargin{ZLRS_PHY_LORA_SF7, -106, "LoraSf7"},
};

static const char *BandProfiles_param_name(size_t index, const void *value)
{
	ARG_UNUSED(value);
	static const char *const names[] = {"Flrc2600", "Flrc650", "Flrc325", "LoraSf6", "LoraSf7"};
	return names[index];
}

static const ztest_param_values BandProfiles_params = {
	.values = BandProfiles_values,
	.count = ARRAY_SIZE(BandProfiles_values),
	.elem_size = sizeof(BandProfiles_values[0]),
	.name_cb = BandProfiles_param_name,
};

ZTEST_P(BandProfiles, test_RssiAtFloorKeepsCurrentProfile)
{
	const auto param = ZTEST_GET_PARAM(BandProfilesParam);
	reset_BandPolicy();
	p.phy = param.phy;
	BandSample o;
	good(100, &o);
	o.link.stats.last_rssi = o.link.peer_rssi_dbm = param.floor;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, BandProfiles, test_RssiAtFloorKeepsCurrentProfile,
			       BandProfiles_params);

ZTEST_P(BandProfiles, test_DowngradeRequiresTwoSecondsAfterPowerBoost)
{
	const auto param = ZTEST_GET_PARAM(BandProfilesParam);
	reset_BandPolicy();
	p.phy = param.phy;
	BandSample o;
	good(200, &o);
	o.link.peer_rssi_dbm = param.floor - 1;
	zassert_equal(evaluate(&p, &o), ZLRS_BAND_BOOST);
	for (uint32_t now : {201U, 2200U}) {
		good(now, &o);
		o.link.peer_rssi_dbm = param.floor - 1;
		zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	}
	good(2201, &o);
	o.link.peer_rssi_dbm = param.floor - 1;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_PLAN);
	zexpect_equal(p.target_subg, param.phy == ZLRS_PHY_LORA_SF7);
	zexpect_equal(p.target_phy, param.phy == ZLRS_PHY_LORA_SF7 ? 0U : param.phy + 1U);
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, BandProfiles, test_DowngradeRequiresTwoSecondsAfterPowerBoost,
			       BandProfiles_params);

ZTEST_P(BandProfiles, test_CachedLowQualityCannotBoostOrQualifyDowngrade)
{
	const auto param = ZTEST_GET_PARAM(BandProfilesParam);
	reset_BandPolicy();
	p.phy = param.phy;
	for (uint32_t now : {100U, 2400U, 6400U}) {
		BandSample o;
		good(now, &o);
		o.link.peer_lq_percent = 90;
		o.link.peer_lq_generation = 1;
		zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
		zexpect_false(p.power_boosted);
	}
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, BandProfiles,
			       test_CachedLowQualityCannotBoostOrQualifyDowngrade,
			       BandProfiles_params);

ZTEST_P(BandProfiles, test_ContactLossAdvancesThroughEveryProfileBeforeSubg)
{
	const auto param = ZTEST_GET_PARAM(BandProfilesParam);
	reset_BandPolicy();
	p.phy = param.phy;
	BandSample o;
	good(1000, &o);
	o.link.last_rx_ms = 0;
	o.now_ms = 2500;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_SEARCH);
	zexpect_equal(p.target_subg, param.phy == ZLRS_PHY_LORA_SF7);
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, BandProfiles,
			       test_ContactLossAdvancesThroughEveryProfileBeforeSubg,
			       BandProfiles_params);

using BandBoostParam = std::tuple<zlrs_role_t, bool>;

static const BandBoostParam BandBoost_values[] = {
	std::make_tuple(ZLRS_ROLE_FOLLOWER, false),
	std::make_tuple(ZLRS_ROLE_FOLLOWER, true),
	std::make_tuple(ZLRS_ROLE_LEADER, false),
	std::make_tuple(ZLRS_ROLE_LEADER, true),
};

static const char *BandBoost_param_name(size_t index, const void *value)
{
	ARG_UNUSED(value);
	static const char *const names[] = {"FollowerLocalDirection", "FollowerPeerDirection",
					    "LeaderLocalDirection", "LeaderPeerDirection"};
	return names[index];
}

static const ztest_param_values BandBoost_params = {
	.values = BandBoost_values,
	.count = ARRAY_SIZE(BandBoost_values),
	.elem_size = sizeof(BandBoost_values[0]),
	.name_cb = BandBoost_param_name,
};

ZTEST_P(BandBoost, test_WaitsForFreshFeedbackAfterMaximumPower)
{
	const auto param = ZTEST_GET_PARAM(BandBoostParam);
	const auto [role, peer_direction] = param;
	zlrs_band_policy_t p{};
	zlrs_band_policy_init(&p, true, role, false, 0, 0);
	BandSample o;
	good(100, &o);
	o.link.cfg.tx_power_dbm = false;
	if (peer_direction) {
		o.link.peer_rssi_dbm = -90;
	} else {
		o.link.stats.last_rssi = -90;
	}
	zassert_equal(evaluate(&p, &o), ZLRS_BAND_BOOST);
	zexpect_true(p.power_boosted);
	zexpect_false(p.bad.active);
	o.now_ms = o.link.last_rx_ms = o.link.last_peer_feedback_ms = 150;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_BOOST);
	o.now_ms = o.link.last_rx_ms = 200;
	o.link.cfg.tx_power_dbm = true;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	zexpect_false(p.bad.active);
	for (uint32_t now : {300U, 2299U}) {
		o.now_ms = o.link.last_rx_ms = o.link.last_peer_feedback_ms = now;
		zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	}
	o.now_ms = o.link.last_rx_ms = o.link.last_peer_feedback_ms = 2300;
	zexpect_equal(evaluate(&p, &o), role == ZLRS_ROLE_LEADER ? ZLRS_BAND_PLAN : ZLRS_BAND_KEEP);
	good(2400, &o);
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	zexpect_false(p.power_boosted);
	zexpect_false(p.bad.active);
}

ZTEST_INSTANTIATE_TEST_SUITE_P(cases, BandBoost, test_WaitsForFreshFeedbackAfterMaximumPower,
			       BandBoost_params);

ZTEST(BandPolicy, test_QualityDowngradeWaitsForAnEntireWindowAtMaximum)
{
	reset_BandPolicy();
	BandSample o;
	good(100, &o);
	o.link.stats.lq_generation = o.link.peer_lq_generation = 10;
	o.link.peer_lq_percent = 90;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	good(2000, &o);
	o.link.peer_lq_percent = 90;
	o.link.stats.lq_generation = o.link.peer_lq_generation = 11;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	good(30000, &o);
	o.link.peer_lq_percent = 90;
	o.link.stats.lq_generation = o.link.peer_lq_generation = 11;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	good(40000, &o);
	o.link.peer_lq_percent = 90;
	o.link.stats.lq_generation = o.link.peer_lq_generation = 12;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_PLAN);
}

ZTEST(BandPolicy, test_LoweringPowerInvalidatesPostMaximumQualityEvidence)
{
	reset_BandPolicy();
	BandSample o;
	good(100, &o);
	o.link.peer_lq_percent = 90;
	o.link.peer_lq_generation = 1;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	o.now_ms = o.link.last_rx_ms = o.link.last_peer_feedback_ms = 2000;
	o.link.peer_lq_generation = 2;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	o.link.cfg.tx_power_dbm = false;
	o.link.peer_lq_generation = 3;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	o.link.cfg.tx_power_dbm = true;
	o.now_ms = o.link.last_rx_ms = o.link.last_peer_feedback_ms = 3000;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	o.link.peer_lq_generation = 4;
	o.now_ms = o.link.last_rx_ms = o.link.last_peer_feedback_ms = 4000;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
}

ZTEST(BandPolicy, test_UnknownQualityCannotQualifyAnUpgrade)
{
	reset_BandPolicy();
	p.phy = ZLRS_PHY_FLRC_650;
	for (uint32_t now : {100U, 60100U}) {
		BandSample o;
		good(now, &o);
		o.link.stats.lq_percent = o.link.peer_lq_percent = 127;
		zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	}
}

ZTEST(BandPolicy, test_WeakHealthySlowLinkStaysOn24)
{
	reset_BandPolicy();
	p.phy = ZLRS_PHY_FLRC_325;
	for (uint32_t t = 100; t < 100000; t += 100) {
		TC_PRINT("t=%u\n", (unsigned int)(t));
		BandSample o;
		good(t, &o);
		o.link.stats.last_rssi = o.link.peer_rssi_dbm = -97;
		zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	}
}

ZTEST(BandPolicy, test_UpgradeNeedsMarginAndContinuousMinute)
{
	reset_BandPolicy();
	zlrs_band_policy_changed(&p, false, ZLRS_PHY_LORA_SF7, false, 0, 0);
	for (uint32_t now : {100U, 60099U}) {
		BandSample o;
		good(now, &o);
		o.link.stats.last_rssi = o.link.peer_rssi_dbm = -97;
		zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	}
	BandSample o;
	good(60100, &o);
	o.link.stats.last_rssi = o.link.peer_rssi_dbm = -97;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_PLAN);
	zexpect_false(p.target_subg);
	zexpect_equal(p.target_phy, ZLRS_PHY_LORA_SF6);
}

ZTEST(BandPolicy, test_UpgradeToleratesSmallLossButRequiresReceiveMargin)
{
	reset_BandPolicy();
	for (uint8_t quality : {95, 97, 98, 99}) {
		zlrs_band_policy_changed(&p, false, ZLRS_PHY_FLRC_650, false, 0, 0);
		BandSample o;
		good(100, &o);
		o.link.stats.lq_percent = o.link.peer_lq_percent = quality;
		zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
		o.now_ms = o.link.last_rx_ms = o.link.last_peer_feedback_ms = 60100;
		zexpect_equal(evaluate(&p, &o), ZLRS_BAND_PLAN);
		zexpect_equal(p.target_phy, ZLRS_PHY_FLRC_2600);
	}
	zlrs_band_policy_changed(&p, false, ZLRS_PHY_FLRC_650, false, 0, 0);
	for (uint32_t now : {100U, 60100U}) {
		BandSample o;
		good(now, &o);
		o.link.stats.last_rssi = -84; /* Below the faster PHY's -83 dBm upgrade boundary. */
		zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	}
}

ZTEST(BandPolicy, test_FreshPoorWindowRestartsUpgradeQualification)
{
	reset_BandPolicy();
	zlrs_band_policy_changed(&p, false, ZLRS_PHY_FLRC_650, false, 0, 0);
	BandSample o;
	good(100, &o);
	o.link.peer_lq_percent = 98;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	good(60099, &o);
	o.link.peer_lq_percent = 94;
	o.link.peer_lq_generation = 1;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	good(60100, &o);
	o.link.peer_lq_percent = 98;
	o.link.peer_lq_generation = 2;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	good(120099, &o);
	o.link.peer_lq_percent = 98;
	o.link.peer_lq_generation = 3;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	good(120100, &o);
	o.link.peer_lq_percent = 98;
	o.link.peer_lq_generation = 3;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_PLAN);
}

ZTEST(BandPolicy, test_ProbeNeedsFiveSecondsOfValidHealthyQuality)
{
	reset_BandPolicy();
	for (uint8_t quality : {94, 95, 127}) {
		zlrs_band_policy_changed(&p, false, ZLRS_PHY_FLRC_650, true, 0, 0);
		for (uint32_t now : {100U, 5100U}) {
			BandSample o;
			good(now, &o);
			o.link.stats.lq_percent = o.link.peer_lq_percent = quality;
			zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
		}
		zexpect_equal(p.probing, quality != 95);
	}
}

ZTEST(BandPolicy, test_LeaderDwellOutlastsFollowerSweep)
{
	reset_BandPolicy();
	p.phy = ZLRS_PHY_LORA_SF7;
	BandSample o;
	good(2500, &o);
	o.link.last_rx_ms = 0;
	zassert_equal(evaluate(&p, &o), ZLRS_BAND_SEARCH);
	zlrs_band_policy_t follower{};
	zlrs_band_policy_init(&follower, true, ZLRS_ROLE_FOLLOWER, false, 0, 0);
	zexpect_true((p.dwell_ms) > ((ZLRS_PHY_AUTO_COUNT + 1U) * follower.dwell_ms));
}

ZTEST(BandPolicy, test_HealthySubgProbesRobust24Profile)
{
	reset_BandPolicy();
	zlrs_band_policy_changed(&p, true, ZLRS_PHY_FLRC_2600, false, 0, 0);
	BandSample o;
	good(100, &o);
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	good(60100, &o);
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_PLAN);
	zexpect_false(p.target_subg);
	zexpect_equal(p.target_phy, ZLRS_PHY_LORA_SF7);
}

ZTEST(BandPolicy, test_FailedSubgCaptureResumesFullSweep)
{
	reset_BandPolicy();
	zlrs_band_policy_changed(&p, true, ZLRS_PHY_FLRC_2600, false, 0, 0);
	BandSample o;
	good(28000, &o);
	o.link.radio_acquiring = true;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_SEARCH);
	zexpect_false(p.target_subg);
	zexpect_equal(p.target_phy, ZLRS_PHY_FLRC_2600);
}

ZTEST(BandPolicy, test_DutySilenceExtendsLossTimeoutAndFollowerDwell)
{
	reset_BandPolicy();
	zlrs_band_policy_t follower{};
	zlrs_band_policy_init(&p, true, ZLRS_ROLE_LEADER, true, 60000, 0);
	zlrs_band_policy_init(&follower, true, ZLRS_ROLE_FOLLOWER, true, 60000, 0);
	zexpect_equal(p.loss_ms, 62000U);
	zexpect_equal(follower.dwell_ms, 62000U);
	zexpect_true((p.dwell_ms) > ((ZLRS_PHY_AUTO_COUNT + 1U) * follower.dwell_ms));
	BandSample o;
	good(0, &o);
	o.now_ms = 61999;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	o.now_ms = 62000;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_SEARCH);
}

ZTEST(BandPolicy, test_LossTimeoutCrossesClockWrapAndPairingSuppressesSearch)
{
	reset_BandPolicy();
	zlrs_band_policy_init(&p, true, ZLRS_ROLE_LEADER, false, 0, UINT32_MAX - 1000U);
	BandSample o;
	good(0, &o);
	o.link.last_rx_ms = 0;
	o.now_ms = 498;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	o.now_ms = 499;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_SEARCH);
	o.link.state = ZLRS_STATE_PAIRING;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
}

ZTEST(BandPolicy, test_BoostQualificationCrossesClockWrap)
{
	reset_BandPolicy();
	zlrs_band_policy_init(&p, true, ZLRS_ROLE_LEADER, false, 0, UINT32_MAX - 50U);
	BandSample o;
	good(UINT32_MAX - 49U, &o);
	o.link.peer_rssi_dbm = -90;
	o.link.cfg.tx_power_dbm = false;
	zassert_equal(evaluate(&p, &o), ZLRS_BAND_BOOST);
	for (uint32_t now : {10U, 2009U}) {
		good(now, &o);
		o.link.peer_rssi_dbm = -90;
		zexpect_equal(evaluate(&p, &o), ZLRS_BAND_KEEP);
	}
	good(2010, &o);
	o.link.peer_rssi_dbm = -90;
	zexpect_equal(evaluate(&p, &o), ZLRS_BAND_PLAN);
}
} // namespace

ZTEST_SUITE(BandBoost, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(BandPolicy, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(BandProfiles, NULL, NULL, NULL, NULL, NULL);
