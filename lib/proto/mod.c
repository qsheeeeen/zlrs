/*
 * 调制档与空中时间模型，见 include/zlrs/proto/mod.h 与 doc/03-protocol.md 4.1.1。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/proto/mod.h"

#include <string.h>

/*
 * FLRC 的净时间：载荷 + 4 字节开销（前导码/同步字/长度）按原始速率折算，
 * 与 lib/region 里的 zlrs_airtime_us() 是同一个模型——那边用于法规记账，
 * 这里用于超帧规划，两边必须是同一个数。
 */
static uint32_t flrc_airtime_us(uint32_t rate_bps, uint16_t frame_len)
{
	uint64_t bits;

	if (rate_bps == 0U) {
		return 0U;
	}

	bits = ((uint64_t)frame_len + 4U) * 8U;

	return (uint32_t)((bits * 1000000U + rate_bps - 1U) / rate_bps);
}

/*
 * LoRa 载荷符号数（Semtech 公式）：CRC=1（硬件生成）、IH=0（显式头）、DE 只在
 * SF11/SF12 配 125 kHz 时置 1、CR 用 1..4 表示 4/5..4/8。
 */
static uint32_t lora_payload_symbols(const zlrs_mod_cfg_t *mod, uint16_t frame_len)
{
	const uint32_t cr = (mod->cr >= 1U && mod->cr <= 4U) ? mod->cr : 1U;
	const uint32_t de = ((mod->sf >= 11U) && (mod->bw_hz == 125000U)) ? 1U : 0U;
	const int32_t numerator =
		(int32_t)(8U * frame_len) - (int32_t)(4U * mod->sf) + 28 + 16;
	const uint32_t denominator = 4U * (mod->sf - 2U * de);
	int32_t quotient;

	if (denominator == 0U) {
		return 8U;
	}

	/* 向上取整（分子可能为负：极短帧时按 0 处理） */
	quotient = (numerator > 0) ? (int32_t)(((uint32_t)numerator + denominator - 1U) /
					      denominator)
				   : 0;

	return (uint32_t)(8 + (uint32_t)quotient * (cr + 4U));
}

bool zlrs_mod_equal(const zlrs_mod_cfg_t *a, const zlrs_mod_cfg_t *b)
{
	if (a == NULL || b == NULL) {
		return false;
	}

	return a->modulation == b->modulation && a->sf == b->sf && a->cr == b->cr &&
	       a->preamble == b->preamble && a->sync_byte == b->sync_byte &&
	       a->rate_bps == b->rate_bps && a->bw_hz == b->bw_hz;
}

zlrs_mod_cfg_t zlrs_mod_flrc(uint32_t rate_bps)
{
	zlrs_mod_cfg_t mod;

	memset(&mod, 0, sizeof(mod));
	mod.modulation = (uint8_t)ZLRS_MOD_FLRC;
	mod.rate_bps = rate_bps;

	return mod;
}

zlrs_mod_cfg_t zlrs_mod_lora(uint8_t sf, uint32_t bw_hz, uint8_t cr, uint8_t preamble)
{
	zlrs_mod_cfg_t mod;

	memset(&mod, 0, sizeof(mod));
	mod.modulation = (uint8_t)ZLRS_MOD_LORA;
	mod.sf = sf;
	mod.bw_hz = bw_hz;
	mod.cr = cr;
	mod.preamble = preamble;

	return mod;
}

uint32_t zlrs_mod_airtime_us(const zlrs_mod_cfg_t *mod, uint16_t frame_len)
{
	uint64_t symbol_num;   /* 一个符号的时间 = symbol_num / bw_hz 秒 */
	uint64_t payload_us;
	uint64_t preamble_us;
	uint32_t symbols;

	if (mod == NULL) {
		return 0U;
	}

	if (mod->modulation == (uint8_t)ZLRS_MOD_FLRC) {
		return flrc_airtime_us(mod->rate_bps, frame_len);
	}

	if (mod->modulation != (uint8_t)ZLRS_MOD_LORA || mod->bw_hz == 0U ||
	    mod->sf < 5U || mod->sf > 12U) {
		return 0U;
	}

	/* Ts = 2^SF / BW 秒，用微秒×Hz 的整数表示，避免逐项取整带来的误差 */
	symbol_num = (uint64_t)1000000U << mod->sf;

	/* 前导码：(n + 4.25) 个符号 = n + 17/4 */
	preamble_us = (symbol_num * ((uint64_t)4U * mod->preamble + 17U) +
		       4U * mod->bw_hz - 1U) /
		      (4U * mod->bw_hz);

	symbols = lora_payload_symbols(mod, frame_len);
	payload_us = (symbol_num * symbols + mod->bw_hz - 1U) / mod->bw_hz;

	return (uint32_t)(preamble_us + payload_us);
}

uint32_t zlrs_mod_airtime_ms(const zlrs_mod_cfg_t *mod, uint16_t frame_len)
{
	const uint32_t us = zlrs_mod_airtime_us(mod, frame_len);

	return (us + 999U) / 1000U;
}
