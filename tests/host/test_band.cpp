/* SPDX-License-Identifier: Apache-2.0 */
#include <gtest/gtest.h>
#include <tuple>

extern "C" {
#include "band.h"
}

namespace
{
zlrs_band_observation_t good(uint32_t now)
{
	zlrs_band_observation_t o{};
	o.now_ms = o.last_rx_ms = o.feedback_ms = now;
	o.paired = o.connected = o.power_at_max = true;
	o.rssi_dbm = o.peer_rssi_dbm = -60;
	o.lq_percent = o.peer_lq_percent = 100;
	return o;
}

class BandPolicy: public testing::Test
{
      protected:
	zlrs_band_policy_t p{};

	void SetUp() override
	{
		zlrs_band_policy_init(&p, true, true, false, 0, 0);
	}
};

struct PhyMargin {
	zlrs_phy_t phy;
	int16_t floor;
	const char *name;
};

class BandProfiles: public BandPolicy, public testing::WithParamInterface<PhyMargin>
{
      protected:
	void SetUp() override
	{
		BandPolicy::SetUp();
		p.phy = GetParam().phy;
	}
};

TEST_P(BandProfiles, RssiAtFloorKeepsCurrentProfile)
{
	auto o = good(100);
	o.rssi_dbm = o.peer_rssi_dbm = GetParam().floor;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
}

TEST_P(BandProfiles, DowngradeRequiresTwoSecondsAfterPowerBoost)
{
	auto o = good(200);
	o.peer_rssi_dbm = GetParam().floor - 1;
	ASSERT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_BOOST);
	for (uint32_t now : {201U, 2200U}) {
		o = good(now);
		o.peer_rssi_dbm = GetParam().floor - 1;
		EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
	}
	o = good(2201);
	o.peer_rssi_dbm = GetParam().floor - 1;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_PLAN);
	EXPECT_EQ(p.target_subg, GetParam().phy == ZLRS_PHY_LORA_SF7);
	EXPECT_EQ(p.target_phy, GetParam().phy == ZLRS_PHY_LORA_SF7 ? 0U : GetParam().phy + 1U);
}

TEST_P(BandProfiles, StaleQualityFeedbackRestartsQualification)
{
	auto o = good(2300);
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
	o = good(2400);
	o.peer_lq_percent = 94;
	ASSERT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_BOOST);
	o = good(4300);
	o.peer_lq_percent = 94;
	o.feedback_ms = 1;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
	o = good(4400);
	o.peer_lq_percent = 94;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
	o = good(6400);
	o.peer_lq_percent = 94;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_PLAN);
	p.leader = false;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
	p.automatic = false;
	o.now_ms += 100000;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
}

TEST_P(BandProfiles, ContactLossAdvancesThroughEveryProfileBeforeSubg)
{
	auto o = good(1000);
	o.last_rx_ms = 0;
	o.now_ms = 2500;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_SEARCH);
	EXPECT_EQ(p.target_subg, GetParam().phy == ZLRS_PHY_LORA_SF7);
}

INSTANTIATE_TEST_SUITE_P(AutomaticLadder, BandProfiles,
			 testing::Values(PhyMargin{ZLRS_PHY_FLRC_2600, -89, "Flrc2600"},
					 PhyMargin{ZLRS_PHY_FLRC_650, -94, "Flrc650"},
					 PhyMargin{ZLRS_PHY_FLRC_325, -98, "Flrc325"},
					 PhyMargin{ZLRS_PHY_LORA_SF6, -103, "LoraSf6"},
					 PhyMargin{ZLRS_PHY_LORA_SF7, -106, "LoraSf7"}),
			 [](const testing::TestParamInfo<PhyMargin> &info) {
				 return info.param.name;
			 });

class BandBoost: public testing::TestWithParam<std::tuple<bool, bool>>
{
};

TEST_P(BandBoost, WaitsForFreshFeedbackAfterMaximumPower)
{
	const auto [leader, peer_direction] = GetParam();
	zlrs_band_policy_t p{};
	zlrs_band_policy_init(&p, true, leader, false, 0, 0);
	auto o = good(100);
	o.power_at_max = false;
	if (peer_direction) {
		o.peer_rssi_dbm = -90;
	} else {
		o.rssi_dbm = -90;
	}
	ASSERT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_BOOST);
	EXPECT_TRUE(p.power_boosted);
	EXPECT_FALSE(p.bad);
	o.now_ms = o.last_rx_ms = o.feedback_ms = 150;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_BOOST);
	o.now_ms = o.last_rx_ms = 200;
	o.power_at_max = true;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
	EXPECT_FALSE(p.bad);
	for (uint32_t now : {300U, 2299U}) {
		o.now_ms = o.last_rx_ms = o.feedback_ms = now;
		EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
	}
	o.now_ms = o.last_rx_ms = o.feedback_ms = 2300;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), leader ? ZLRS_BAND_PLAN : ZLRS_BAND_KEEP);
	o = good(2400);
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
	EXPECT_FALSE(p.power_boosted);
	EXPECT_FALSE(p.bad);
}

INSTANTIATE_TEST_SUITE_P(RolesAndDirections, BandBoost,
			 testing::Combine(testing::Bool(), testing::Bool()),
			 [](const testing::TestParamInfo<BandBoost::ParamType> &info) {
				 return std::string(std::get<0>(info.param) ? "Leader"
									    : "Follower") +
					(std::get<1>(info.param) ? "Peer" : "Local");
			 });

TEST_F(BandPolicy, WeakHealthySlowLinkStaysOn24)
{
	p.phy = ZLRS_PHY_FLRC_325;
	for (uint32_t t = 100; t < 100000; t += 100) {
		SCOPED_TRACE(t);
		auto o = good(t);
		o.rssi_dbm = o.peer_rssi_dbm = -97;
		EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
	}
}

TEST_F(BandPolicy, UpgradeNeedsMarginAndContinuousMinute)
{
	zlrs_band_policy_changed(&p, false, ZLRS_PHY_LORA_SF7, false, 0, 0);
	for (uint32_t now : {100U, 60099U}) {
		auto o = good(now);
		o.rssi_dbm = o.peer_rssi_dbm = -97;
		EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
	}
	auto o = good(60100);
	o.rssi_dbm = o.peer_rssi_dbm = -97;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_PLAN);
	EXPECT_FALSE(p.target_subg);
	EXPECT_EQ(p.target_phy, ZLRS_PHY_LORA_SF6);
}

TEST_F(BandPolicy, LeaderDwellOutlastsFollowerSweep)
{
	p.phy = ZLRS_PHY_LORA_SF7;
	auto o = good(2500);
	o.last_rx_ms = 0;
	ASSERT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_SEARCH);
	zlrs_band_policy_t follower{};
	zlrs_band_policy_init(&follower, true, false, false, 0, 0);
	EXPECT_GT(p.dwell_ms, (ZLRS_PHY_AUTO_COUNT + 1U) * follower.dwell_ms);
}

TEST_F(BandPolicy, HealthySubgProbesRobust24Profile)
{
	zlrs_band_policy_changed(&p, true, ZLRS_PHY_FLRC_2600, false, 0, 0);
	auto o = good(100);
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
	o = good(60100);
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_PLAN);
	EXPECT_FALSE(p.target_subg);
	EXPECT_EQ(p.target_phy, ZLRS_PHY_LORA_SF7);
}

TEST_F(BandPolicy, FailedSubgCaptureResumesFullSweep)
{
	zlrs_band_policy_changed(&p, true, ZLRS_PHY_FLRC_2600, false, 0, 0);
	auto o = good(28000);
	o.acquiring = true;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_SEARCH);
	EXPECT_FALSE(p.target_subg);
	EXPECT_EQ(p.target_phy, ZLRS_PHY_FLRC_2600);
}

TEST_F(BandPolicy, DutySilenceExtendsLossTimeoutAndFollowerDwell)
{
	zlrs_band_policy_t follower{};
	zlrs_band_policy_init(&p, true, true, true, 60000, 0);
	zlrs_band_policy_init(&follower, true, false, true, 60000, 0);
	EXPECT_EQ(p.loss_ms, 62000U);
	EXPECT_EQ(follower.dwell_ms, 62000U);
	EXPECT_GT(p.dwell_ms, (ZLRS_PHY_AUTO_COUNT + 1U) * follower.dwell_ms);
	auto o = good(0);
	o.now_ms = 61999;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
	o.now_ms = 62000;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_SEARCH);
}

TEST_F(BandPolicy, LossTimeoutCrossesClockWrapAndPairingSuppressesSearch)
{
	zlrs_band_policy_init(&p, true, true, false, 0, UINT32_MAX - 1000U);
	auto o = good(0);
	o.last_rx_ms = 0;
	o.now_ms = 498;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
	o.now_ms = 499;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_SEARCH);
	o.pairing = true;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
}

TEST_F(BandPolicy, BoostQualificationCrossesClockWrap)
{
	zlrs_band_policy_init(&p, true, true, false, 0, UINT32_MAX - 50U);
	auto o = good(UINT32_MAX - 49U);
	o.peer_rssi_dbm = -90;
	o.power_at_max = false;
	ASSERT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_BOOST);
	for (uint32_t now : {10U, 2009U}) {
		o = good(now);
		o.peer_rssi_dbm = -90;
		EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_KEEP);
	}
	o = good(2010);
	o.peer_rssi_dbm = -90;
	EXPECT_EQ(zlrs_band_policy_update(&p, &o), ZLRS_BAND_PLAN);
}
} // namespace
