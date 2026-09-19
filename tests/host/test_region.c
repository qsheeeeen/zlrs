/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Region layer tests: every channel plan stays inside its band edges, power within the ceiling,
 * duty is accounted over a sliding window and overruns are refused and counted.
 */

#include "test.h"

#include "zlrs/region/region.h"

static void test_tables(void)
{
	for (uint8_t i = 0U; i < ZLRS_REGION_COUNT; i++) {
		const zlrs_region_t *region = zlrs_region_get(i);

		CHECK(region != NULL);
		CHECK(region->name != NULL);
		/* Matching band, power within the ceiling: must pass */
		CHECK(zlrs_region_validate(region, region->band, region->max_tx_power_dbm) == 0);
		/* A little over the ceiling has to be refused */
		if (region->max_tx_power_dbm < 22) {
			CHECK(zlrs_region_validate(region, region->band,
						   (int8_t)(region->max_tx_power_dbm + 1)) != 0);
		}
		/* A band mismatch has to be refused: an LF board cannot do 915, an HF one cannot do 470/433 */
		CHECK(zlrs_region_validate(region,
					   (region->band == ZLRS_BAND_SUBG_HF) ? ZLRS_BAND_SUBG_LF
									       : ZLRS_BAND_SUBG_HF,
					   region->max_tx_power_dbm) != 0);
	}

	CHECK(zlrs_region_get(ZLRS_REGION_COUNT) == NULL);
	CHECK(zlrs_region_validate(NULL, ZLRS_BAND_SUBG_HF, 0) != 0);

	/* On the prototype: the HF board accepts FCC915/EU868, the LF board CN470/CN433 */
	CHECK(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_FCC915), ZLRS_BAND_SUBG_HF, 10) ==
	      0);
	CHECK(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_FCC915), ZLRS_BAND_SUBG_LF, 10) ==
	      -5);
	CHECK(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_CN470), ZLRS_BAND_SUBG_LF, 10) == 0);
	CHECK(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_CN433), ZLRS_BAND_SUBG_LF, 10) == 0);
	CHECK(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_CN433), ZLRS_BAND_SUBG_HF, 10) ==
	      -5);
}

/* Rate ceiling: narrow bands pull the configured rate down */
static void test_rate_limit(void)
{
	printf("region: rate limit\n");

	CHECK(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_FCC915), 2600000U) == 2600000U);
	CHECK(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_FCC915), 1300000U) == 1300000U);
	/* EU868, CN470 and CN433 cannot fit the wideband tiers */
	CHECK(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_EU868), 1300000U) == 325000U);
	CHECK(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_CN470), 2600000U) == 325000U);
	CHECK(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_CN433), 650000U) == 325000U);
	/* Anything already below the ceiling stays put */
	CHECK(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_CN433), 325000U) == 325000U);
	CHECK(zlrs_region_clamp_rate(NULL, 1300000U) == 1300000U);
}

static void test_airtime(void)
{
	printf("region: airtime\n");

	/* 128 bytes at 1.3 Mbps: (128+4)*8 bit = 812.3 us, rounded up to 813 us */
	CHECK(zlrs_airtime_us(128U, 1300000U) == 813U);
	/* The same frame at 0.325 Mbps is about 3.25 ms */
	CHECK(zlrs_airtime_us(128U, 325000U) == 3250U);
	/* 128 bytes at 1.3 Mbps: (128+4)*8 bit ~ 812 us, rounded up to 1 ms */
	CHECK(zlrs_airtime_ms(128U, 1300000U) == 1U);
	/* The same frame at 0.325 Mbps is about 3.25 ms */
	CHECK(zlrs_airtime_ms(128U, 325000U) == 4U);
	/* No accounting when the rate is unknown */
	CHECK(zlrs_airtime_ms(128U, 0U) == 0U);
	CHECK(zlrs_airtime_us(128U, 0U) == 0U);
}

/* Region duty -> control rate ceiling: EU868's 1% cannot feed 150 Hz, FCC/CN are unlimited */
static void test_max_frames(void)
{
	/* 31-byte control frame at 1.3 Mbps: (31+4)*8/1.3e6 = 215.4 us, rounded up to 216 us */
	printf("region: max control rate\n");

	CHECK(zlrs_airtime_us(31U, 1300000U) == 216U);

	/* 1% -> 10000 us/s -> 10000/216 = 46 frames per second */
	CHECK(zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_EU868), 1300000U, 31U) ==
	      46U);
	/* Unlimited-duty regions: as fast as you like */
	CHECK(zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_FCC915), 1300000U, 31U) ==
	      UINT32_MAX);
	CHECK(zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_CN470), 1300000U, 31U) ==
	      UINT32_MAX);
	/* No ceiling when the rate is unknown (the layer above decides) */
	CHECK(zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_EU868), 0U, 31U) ==
	      UINT32_MAX);
}

static void test_duty(void)
{
	const zlrs_region_t *eu = zlrs_region_get(ZLRS_REGION_EU868);
	const zlrs_region_t *fcc = zlrs_region_get(ZLRS_REGION_FCC915);
	zlrs_duty_t duty;
	uint32_t now = 1000U;

	printf("region: duty cycle\n");

	/* EU868: 1% of 60 s = 600 ms of budget */
	zlrs_duty_init(&duty, eu, now);
	CHECK(duty.limited);
	CHECK(zlrs_duty_remaining_ms(&duty, now) == 600U);

	/* One 4 ms transmission leaves 596 ms */
	CHECK(zlrs_duty_reserve(&duty, 4U, now));
	CHECK(zlrs_duty_remaining_ms(&duty, now) == 596U);

	/* Spend the rest in 100 ms chunks; the next attempt is refused and counted */
	for (uint8_t i = 0U; i < 5U; i++) {
		CHECK(zlrs_duty_reserve(&duty, 100U, now));
	}
	CHECK(zlrs_duty_remaining_ms(&duty, now) == 96U);
	CHECK(!zlrs_duty_reserve(&duty, 100U, now));
	CHECK(zlrs_duty_reserve(&duty, 96U, now));
	CHECK(zlrs_duty_remaining_ms(&duty, now) == 0U);
	CHECK(!zlrs_duty_reserve(&duty, 1U, now));
	CHECK(duty.denied == 2U); /* that 100 ms attempt was refused too */

	/* New window: the budget is back */
	now += 60001U;
	CHECK(zlrs_duty_remaining_ms(&duty, now) == 600U);
	CHECK(zlrs_duty_reserve(&duty, 4U, now));

	/* FCC915 has no duty limit: transmit freely, no counter moves */
	zlrs_duty_init(&duty, fcc, now);
	CHECK(!duty.limited);
	for (uint8_t i = 0U; i < 5U; i++) {
		CHECK(zlrs_duty_reserve(&duty, 4U, now));
	}
	CHECK(zlrs_duty_remaining_ms(&duty, now) == UINT32_MAX);
	CHECK(duty.denied == 0U);
}

/* Control plan: rate is bounded by duty and airtime, the cadence factors follow the rate */
static void test_control_plan(void)
{
	zlrs_control_plan_t plan;
	const zlrs_mod_cfg_t fast = zlrs_mod_flrc(1300000U);
	const zlrs_mod_cfg_t narrow = zlrs_mod_flrc(325000U);
	zlrs_plan_req_t req = {
		.mod = &fast,
		.frame_len = 31U,
	};

	printf("region: control plan\n");

	/* Default tier: FCC915 at 1.3 Mbps runs 150 Hz unchanged */
	req.region = zlrs_region_get(ZLRS_REGION_FCC915);
	req.want_hz = 150U;
	zlrs_region_control_plan(&req, &plan);
	CHECK(plan.rate_hz == 150U);
	CHECK(plan.frame_period_us == 6666U);
	CHECK(plan.slot_budget_us == 1666U);
	CHECK(plan.tlm_every_n == 1U);
	CHECK(plan.data_every_n == 1U);
	CHECK(!plan.clamped_by_duty && !plan.clamped_by_airtime);

	/* 500 Hz tier: FCC915 has no duty limit and 1.3 Mbps airtime keeps up */
	req.want_hz = 500U;
	zlrs_region_control_plan(&req, &plan);
	CHECK(plan.rate_hz == 500U);
	CHECK(plan.frame_period_us == 2000U);
	CHECK(plan.slot_budget_us == 500U);
	CHECK(plan.tlm_every_n == 3U);  /* about 167 times per second */
	CHECK(plan.data_every_n == 5U); /* 100 times per second */
	CHECK(!plan.clamped_by_duty && !plan.clamped_by_airtime);

	/* EU868's 1% duty pulls 500 Hz down to 46 Hz */
	req.region = zlrs_region_get(ZLRS_REGION_EU868);
	zlrs_region_control_plan(&req, &plan);
	CHECK(plan.rate_hz == 46U);
	CHECK(plan.clamped_by_duty);
	CHECK(!plan.clamped_by_airtime);
	CHECK(plan.tlm_every_n ==
	      1U); /* the rate is below the target, so every frame carries telemetry */

	/* At the narrow-band rate (CN470 clamps to 0.325 Mbps) airtime bounds the frame rate */
	req.region = zlrs_region_get(ZLRS_REGION_CN470);
	req.mod = &narrow;
	zlrs_region_control_plan(&req, &plan);
	CHECK(plan.clamped_by_airtime);
	/* Airtime 862 us plus 200 us of headroom -> two control slots per frame ~ 470 Hz */
	CHECK(plan.rate_hz == 470U);
	/* A quarter frame (531 us) cannot hold a 1062 us slot, so the budget follows the real need */
	CHECK(plan.slot_budget_us == 1062U);
}

/*
 * LoRa long-range tier (docs/design/07-architecture.md 3): a frame is tens of ms, so airtime decides
 * the frame rate, not the configured number.
 */
static void test_lora_plan(void)
{
	const zlrs_mod_cfg_t lora = zlrs_mod_lora(7U, 125000U, 1U, 8U);
	const zlrs_mod_cfg_t sub_g_slow = zlrs_mod_flrc(325000U);
	zlrs_control_plan_t plan;
	zlrs_plan_req_t req = {
		.region = NULL, /* FCC915: no duty limit */
		.mod = &lora,
		.want_hz = 10U,
		.frame_len = 31U,
		.tlm_len = 7U,
	};

	printf("region: lora control plan\n");

	req.region = zlrs_region_get(ZLRS_REGION_FCC915);
	zlrs_region_control_plan(&req, &plan);

	/* 10 Hz does not fit (72 + 36 + headroom = 108 ms), so it clamps to 9 Hz */
	CHECK(plan.clamped_by_airtime);
	CHECK(!plan.clamped_by_duty);
	CHECK(plan.rate_hz == 9U);
	CHECK(plan.frame_period_us == 111111U);
	/* The slot budget has to hold a 72 ms RC frame: FLRC's 2 ms ceiling does not apply here */
	CHECK(plan.slot_budget_us >= 71936U + 200U);

	/* The data link runs narrow-band on sub-G: two 135 B windows amortise in and trim the rate */
	req.other_mod = &sub_g_slow;
	req.other_frame_len = 135U;
	zlrs_region_control_plan(&req, &plan);
	CHECK(plan.rate_hz == 8U);
	CHECK(plan.frame_period_us >= 108432U + 2U * 3622U);

	/* EU868's 1% duty cannot feed LoRa: it warns and the rate falls back to 1 Hz */
	req.region = zlrs_region_get(ZLRS_REGION_EU868);
	req.other_mod = NULL;
	req.other_frame_len = 0U;
	zlrs_region_control_plan(&req, &plan);
	CHECK(plan.clamped_by_duty);
	CHECK(plan.rate_hz == 1U);
}

/*
 * Racing mode (docs/design/07-architecture.md 2): control owns 2.4G, has no region table and the duty
 * gate does not touch it; the link that moved to sub-G (data) is the regulated one, and its
 * windows amortise into the superframe through data_every_n, trimming the peak rate.
 */
static void test_racing_plan(void)
{
	const zlrs_mod_cfg_t ctl = zlrs_mod_flrc(2600000U);
	const zlrs_mod_cfg_t sub_g_slow = zlrs_mod_flrc(325000U);
	zlrs_control_plan_t plan;
	zlrs_plan_req_t req = {
		.region = NULL, /* 2.4G: no region table */
		.mod = &ctl,
		.want_hz = 500U,
		.frame_len = 31U,
		.tlm_len = 15U,
		.other_mod = &sub_g_slow,
		.other_frame_len = 135U,
	};

	printf("region: racing plan (control on 2.4g, data on sub-g)\n");

	zlrs_region_control_plan(&req, &plan);

	/* No duty limit, but the sub-G data window reservation pulls the peak to 420 Hz */
	CHECK(!plan.clamped_by_duty);
	CHECK(plan.clamped_by_airtime);
	CHECK(plan.rate_hz == 420U);
	/* Data every 4 frames: 7204 us of reservation spreads to 1811 us per frame, which is consistent */
	CHECK(plan.data_every_n == 4U);
	CHECK(plan.frame_period_us == 2380U);
}

/*
 * 1000 Hz timing budget (docs/design/09-protocol.md 4.1): at 2.6 Mbps a 31 B RC frame takes 108 us and
 * a frame is 1000 us, so the four slots separate cleanly. The protocol side is fine; whether it
 * runs depends on driving the radio directly (RAC schedules in whole milliseconds).
 */
static void test_plan_1000hz(void)
{
	const zlrs_mod_cfg_t fast = zlrs_mod_flrc(2600000U);
	zlrs_control_plan_t plan;
	zlrs_plan_req_t req = {
		.region = NULL, /* FCC915: no duty limit */
		.mod = &fast,
		.want_hz = 1000U,
		.frame_len = 31U,
		.tlm_len = 15U,
		.other_mod = &fast,
		.other_frame_len = 135U,
	};

	printf("region: 1000 Hz plan\n");

	req.region = zlrs_region_get(ZLRS_REGION_FCC915);
	zlrs_region_control_plan(&req, &plan);

	CHECK(plan.rate_hz == 1000U);
	CHECK(plan.frame_period_us == 1000U);
	/* Slot budget = 108 us of airtime plus 200 us of headroom */
	CHECK(plan.slot_budget_us == 308U);
	CHECK(plan.tlm_every_n == 6U);   /* about 167 times per second */
	CHECK(plan.data_every_n == 10U); /* 100 times per second */
	CHECK(!plan.clamped_by_airtime && !plan.clamped_by_duty);
}

static void test_mix_limit(void)
{
	const zlrs_region_t *eu = zlrs_region_get(ZLRS_REGION_EU868);
	const zlrs_region_t *fcc = zlrs_region_get(ZLRS_REGION_FCC915);

	printf("region: sub-band mix limit\n");

	/* No duty limit: 100% straight away (level 3 = all 4 slots) */
	CHECK(zlrs_region_mix_level(fcc, 24U, 1000U) == 3U);
	CHECK(zlrs_region_mix_level(NULL, 24U, 1000U) == 3U);

	/* EU868 1%: a 24 ms window allows 0.24 ms of sub-G airtime,
	 * so 1 ms per slot fits none of the levels */
	CHECK(zlrs_region_mix_level(eu, 24U, 1000U) == 0U);
	/* Shorter slots (200 us): 0.24 ms allows one -> 25% */
	CHECK(zlrs_region_mix_level(eu, 24U, 200U) == 1U);
	/* 100 us per slot: two allowed -> 50% */
	CHECK(zlrs_region_mix_level(eu, 24U, 100U) == 2U);
	/* Window stretched to 400 ms: 1% = 4 ms, enough for four 1 ms slots -> 100% */
	CHECK(zlrs_region_mix_level(eu, 400U, 1000U) == 3U);

	/* Invalid arguments do not crash */
	CHECK(zlrs_region_mix_level(eu, 0U, 100U) == 3U);
	CHECK(zlrs_region_mix_level(eu, 24U, 0U) == 3U);
}

void test_region(void)
{
	printf("region: tables\n");
	test_tables();
	test_rate_limit();
	test_airtime();
	test_max_frames();
	test_control_plan();
	test_lora_plan();
	test_racing_plan();
	test_mix_limit();
	test_plan_1000hz();
	test_duty();
}
