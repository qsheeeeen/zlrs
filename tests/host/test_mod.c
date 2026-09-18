/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 调制档与空中时间模型（lib/proto/mod.c）。
 *
 * 期望值来源：
 *   - FLRC 与 lib/region 的 zlrs_airtime_us() 必须给出同一个数（同一个模型两处用）
 *   - LoRa 时间按 Semtech 的经典公式手算：Ts = 2^SF/BW，前导码 (n + 4.25) 个符号，
 *     载荷符号数 n_payload = 8 + ceil((8*PL - 4*SF + 28 + 16) / (4*(SF - 2*DE))) * (CR+4)
 */

#include "test.h"

#include "zlrs/proto/mod.h"
#include "zlrs/region/region.h"

#include "test.h"

#include "zlrs/proto/mod.h"
#include "zlrs/region/region.h"

/* FLRC：与地区层的模型逐点对齐 */
static void test_mod_flrc_matches_region(void)
{
	static const uint32_t rates[] = { 2600000U, 1300000U, 650000U, 325000U };
	static const uint16_t lens[] = { 7U, 31U, 128U, 255U };
	const zlrs_mod_cfg_t no_rate = zlrs_mod_flrc(0U);

	printf("mod: flrc airtime matches region model\n");

	for (uint8_t i = 0U; i < sizeof(rates) / sizeof(rates[0]); i++) {
		const zlrs_mod_cfg_t mod = zlrs_mod_flrc(rates[i]);

		for (uint8_t j = 0U; j < sizeof(lens) / sizeof(lens[0]); j++) {
			CHECK(zlrs_mod_airtime_us(&mod, lens[j]) ==
			      zlrs_airtime_us(lens[j], rates[i]));
			CHECK(zlrs_mod_airtime_ms(&mod, lens[j]) ==
			      zlrs_airtime_ms(lens[j], rates[i]));
		}
	}

	/* 速率没给（0）算不出来，返回 0 而不是除零 */
	CHECK(zlrs_mod_airtime_us(&no_rate, 31U) == 0U);
}

/*
 * SF7 / BW125 / CR 4/5 / 前导码 8 个符号：
 *   Ts = 128 / 125000 = 1.024 ms
 *   31 B 载荷：ceil((248 - 28 + 44) / 28) = ceil(9.43) = 10 → 10 * 5 + 8 = 58 个符号
 *   72.2 ms？不：前导 (8 + 4.25) * 1.024 = 12.544 ms，载荷 58 * 1.024 = 59.392 ms
 *   合计 71.936 ms
 *   7 B 心跳帧：ceil((56 - 28 + 44) / 28) = 3 → 3 * 5 + 8 = 23 个符号 = 23.552 ms
 *   合计 36.096 ms
 */
static void test_mod_lora_reference(void)
{
	const zlrs_mod_cfg_t sf7 = zlrs_mod_lora(7U, 125000U, 1U, 8U);
	const zlrs_mod_cfg_t sf12 = zlrs_mod_lora(12U, 125000U, 1U, 8U);
	const zlrs_mod_cfg_t sf7_250 = zlrs_mod_lora(7U, 250000U, 1U, 8U);
	const zlrs_mod_cfg_t sf7_cr8 = zlrs_mod_lora(7U, 125000U, 4U, 8U);
	const zlrs_mod_cfg_t no_bw = zlrs_mod_lora(7U, 0U, 1U, 8U);
	const zlrs_mod_cfg_t bad_sf = zlrs_mod_lora(13U, 125000U, 1U, 8U);
	uint32_t rc_us;
	uint32_t tlm_us;

	printf("mod: lora airtime reference values\n");

	rc_us = zlrs_mod_airtime_us(&sf7, 31U);
	tlm_us = zlrs_mod_airtime_us(&sf7, 7U);
	CHECK(rc_us == 71936U);
	CHECK(tlm_us == 36096U);
	CHECK(zlrs_mod_airtime_ms(&sf7, 31U) == 72U);

	/* 带宽翻倍 → 时间减半（整数取整允许 1 µs 的差） */
	CHECK(zlrs_mod_airtime_us(&sf7_250, 31U) == rc_us / 2U);

	/* 编码率 4/8 比 4/5 慢，但快不过前导码加满：单调即可 */
	CHECK(zlrs_mod_airtime_us(&sf7_cr8, 31U) > rc_us);

	/* SF 越大越慢：SF12 比 SF7 慢一个数量级 */
	CHECK(zlrs_mod_airtime_us(&sf12, 31U) > 10U * rc_us);

	/* 载荷越长越慢（同一条曲线，不给拐点） */
	CHECK(zlrs_mod_airtime_us(&sf7, 64U) > zlrs_mod_airtime_us(&sf7, 32U));

	/* 参数不合法：给 0 而不是算出一个瞎数 */
	CHECK(zlrs_mod_airtime_us(&no_bw, 31U) == 0U);
	CHECK(zlrs_mod_airtime_us(&bad_sf, 31U) == 0U);
	CHECK(zlrs_mod_airtime_us(NULL, 31U) == 0U);
}

/*
 * 10 Hz 的 LoRa 远距档要装得下"发 RC + 收遥测"：这就是 doc/07-architecture.md 第 3 节 里那句
 * "超帧周期由最慢的那条链路决定"的定量版本。
 */
static void test_mod_lora_control_budget(void)
{
	const zlrs_mod_cfg_t sf7 = zlrs_mod_lora(7U, 125000U, 1U, 8U);
	const uint32_t rc_us = zlrs_mod_airtime_us(&sf7, 31U);
	const uint32_t tlm_us = zlrs_mod_airtime_us(&sf7, 7U);
	const uint32_t period_us = 100000U; /* 10 Hz */

	printf("mod: lora control-link budget\n");

	/* 100 ms 的周期装得下 72 ms 的 RC 帧，但装不下"72 + 36" */
	CHECK(rc_us < period_us);
	CHECK(rc_us + tlm_us > period_us);
	/* 所以 10 Hz 档里遥测只能稀疏发：每 2 帧一次刚好放得下 */
	CHECK(rc_us + tlm_us / 2U < period_us);
}

void test_mod(void)
{
	test_mod_flrc_matches_region();
	test_mod_lora_reference();
	test_mod_lora_control_budget();
}
