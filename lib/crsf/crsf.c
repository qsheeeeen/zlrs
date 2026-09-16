/*
 * CRSF 帧编解码，见 include/zlrs/crsf/crsf.h。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/crsf/crsf.h"

#include <string.h>

#include "zlrs/proto/crc.h"

/* 解析阶段认识的地址字节；其它字节在空闲态直接丢掉 */
static bool valid_address(uint8_t byte)
{
	switch (byte) {
	case 0x00U: /* 广播 */
	case ZLRS_CRSF_ADDR_FC:
	case 0xECU: /* 接收机 */
	case 0xEEU: /* 模块 */
	case ZLRS_CRSF_ADDR_TX_MODULE:
	case 0xEFU: /* 配置口 */
		return true;
	default:
		return false;
	}
}

void zlrs_crsf_parser_init(zlrs_crsf_parser_t *p)
{
	memset(p, 0, sizeof(*p));
}

uint8_t zlrs_crsf_crc8(const uint8_t *data, size_t len)
{
	return zlrs_crc8(data, len);
}

enum zlrs_crsf_result zlrs_crsf_feed(zlrs_crsf_parser_t *p, uint8_t byte)
{
	if (p->len == 0U) {
		if (!valid_address(byte)) {
			p->frames_bad++;
			return ZLRS_CRSF_BAD_FRAME;
		}

		p->buf[0] = byte;
		p->len = 1U;
		return ZLRS_CRSF_NONE;
	}

	if (p->len == 1U) {
		const uint8_t len_field = byte;

		/* len 字段含 type 与 crc，最小 2 字节；整帧不超过 64 字节 */
		if (len_field < 2U || (uint16_t)len_field + 2U > ZLRS_CRSF_FRAME_MAX_LEN) {
			p->len = 0U;
			p->need = 0U;
			p->frames_bad++;
			return ZLRS_CRSF_BAD_FRAME;
		}

		p->buf[1] = len_field;
		p->len = 2U;
		p->need = (uint8_t)(len_field + 2U);
		return ZLRS_CRSF_NONE;
	}

	if (p->len >= p->need) {
		/* 上一次已经收完但没被取走，丢弃后重新同步 */
		p->len = 0U;
		p->need = 0U;
		return zlrs_crsf_feed(p, byte);
	}

	p->buf[p->len] = byte;
	p->len++;

	if (p->len < p->need) {
		return ZLRS_CRSF_NONE;
	}

	const uint8_t total = p->need;
	const uint8_t crc = zlrs_crsf_crc8(&p->buf[2], (size_t)total - 3U);

	p->type = p->buf[2];
	p->payload_len = (uint8_t)(total - 4U);
	p->len = 0U;
	p->need = 0U;

	if (crc != p->buf[total - 1U]) {
		p->frames_bad++;
		return ZLRS_CRSF_BAD_CRC;
	}

	p->frames_ok++;
	return ZLRS_CRSF_FRAME;
}

uint8_t zlrs_crsf_encode(uint8_t *out, uint8_t addr, uint8_t type, const uint8_t *payload,
			 uint8_t payload_len)
{
	if (payload_len > ZLRS_CRSF_PAYLOAD_MAX_LEN) {
		return 0U;
	}

	out[0] = addr;
	out[1] = (uint8_t)(payload_len + 2U);
	out[2] = type;

	if (payload_len > 0U) {
		memcpy(&out[3], payload, payload_len);
	}

	out[3U + payload_len] = zlrs_crsf_crc8(&out[2], (size_t)payload_len + 1U);

	return (uint8_t)(payload_len + 4U);
}

void zlrs_crsf_channels_pack(uint8_t out[ZLRS_CRSF_RC_PAYLOAD_LEN],
			     const uint16_t channels[ZLRS_CRSF_CHANNEL_COUNT])
{
	memset(out, 0, ZLRS_CRSF_RC_PAYLOAD_LEN);

	uint32_t acc = 0U;
	uint8_t bits = 0U;
	uint8_t pos = 0U;

	for (uint8_t i = 0U; i < ZLRS_CRSF_CHANNEL_COUNT; i++) {
		acc |= (uint32_t)(channels[i] & ZLRS_CRSF_CHANNEL_MASK) << bits;
		bits = (uint8_t)(bits + 11U);

		while (bits >= 8U) {
			out[pos] = (uint8_t)(acc & 0xFFU);
			pos++;
			acc >>= 8;
			bits = (uint8_t)(bits - 8U);
		}
	}
}

void zlrs_crsf_channels_unpack(const uint8_t in[ZLRS_CRSF_RC_PAYLOAD_LEN],
			       uint16_t channels[ZLRS_CRSF_CHANNEL_COUNT])
{
	uint32_t acc = 0U;
	uint8_t bits = 0U;
	uint8_t pos = 0U;

	for (uint8_t i = 0U; i < ZLRS_CRSF_CHANNEL_COUNT; i++) {
		while (bits < 11U) {
			acc |= (uint32_t)in[pos] << bits;
			pos++;
			bits = (uint8_t)(bits + 8U);
		}

		channels[i] = (uint16_t)(acc & ZLRS_CRSF_CHANNEL_MASK);
		acc >>= 11;
		bits = (uint8_t)(bits - 11U);
	}
}

void zlrs_crsf_channels_default(uint16_t channels[ZLRS_CRSF_CHANNEL_COUNT])
{
	for (uint8_t i = 0U; i < ZLRS_CRSF_CHANNEL_COUNT; i++) {
		channels[i] = (i == 0U) ? ZLRS_CRSF_CHANNEL_MIN : ZLRS_CRSF_CHANNEL_MID;
	}
}

uint8_t zlrs_crsf_rssi_from_dbm(int16_t dbm)
{
	if (dbm >= 0) {
		return 0U;
	}

	/* CRSF 用正数表示强度，且饱和在 8 bit */
	return (uint8_t)((-dbm > 255) ? 255 : -dbm);
}

uint8_t zlrs_crsf_tx_power_index(int8_t dbm)
{
	/* 索引 → mW，与 CRSF 的功率档一致 */
	static const uint16_t milliwatts[] = { 0U, 10U, 25U, 100U, 500U, 1000U, 2000U, 250U, 50U };
	uint32_t thousandths = 1000U; /* 1 mW，以 1/1000 mW 为单位 */
	uint16_t want_mw;
	uint8_t best = 0U;

	if (dbm <= 0) {
		return 0U;
	}

	/* dBm → mW：每 1 dBm 乘 10^0.1 ≈ 1.2589，只用整数运算 */
	for (int8_t step = 0; step < dbm && step < 36; step++) {
		thousandths = (thousandths * 1259U) / 1000U;
	}
	/* 每步都截断，最后四舍五入回整毫瓦，避免 10 dBm 掉到 9 mW */
	want_mw = (uint16_t)((thousandths + 500U) / 1000U);

	/* 取不超过实际功率的最大档 */
	for (uint8_t i = 0U; i < sizeof(milliwatts) / sizeof(milliwatts[0]); i++) {
		if (milliwatts[i] <= want_mw && milliwatts[i] >= milliwatts[best]) {
			best = i;
		}
	}

	return best;
}

uint8_t zlrs_crsf_rf_mode(uint32_t rate_bps)
{
	switch (rate_bps) {
	case 2600000U:
		return 0U;
	case 1300000U:
		return 1U;
	case 650000U:
		return 2U;
	case 325000U:
		return 3U;
	default:
		return 0xFFU; /* 未知档 */
	}
}

/*
 * LoRa 档没有"速率"这个说法，用 0x10 | SF 编码：手台侧只要求两端一致，
 * 至少要能看出"这是 LoRa、SF 多少"。见 doc/03-protocol.md 4.1.1。
 */
uint8_t zlrs_crsf_rf_mode_mod(const zlrs_mod_cfg_t *mod)
{
	if (mod == NULL) {
		return 0xFFU;
	}

	if (mod->modulation == (uint8_t)ZLRS_MOD_LORA) {
		return (uint8_t)(0x10U | (mod->sf & 0x0FU));
	}

	return zlrs_crsf_rf_mode(mod->rate_bps);
}

void zlrs_crsf_link_stats_encode(uint8_t out[ZLRS_CRSF_LINK_STATS_LEN],
				 const zlrs_crsf_link_stats_t *stats)
{
	out[0] = stats->uplink_rssi_ant1;
	out[1] = stats->uplink_rssi_ant2;
	out[2] = stats->uplink_lq;
	out[3] = (uint8_t)stats->uplink_snr_db;
	out[4] = stats->active_antenna;
	out[5] = stats->rf_mode;
	out[6] = stats->uplink_tx_power;
	out[7] = stats->downlink_rssi;
	out[8] = stats->downlink_lq;
	out[9] = (uint8_t)stats->downlink_snr_db;
}

void zlrs_crsf_link_stats_decode(const uint8_t in[ZLRS_CRSF_LINK_STATS_LEN],
				 zlrs_crsf_link_stats_t *stats)
{
	stats->uplink_rssi_ant1 = in[0];
	stats->uplink_rssi_ant2 = in[1];
	stats->uplink_lq = in[2];
	stats->uplink_snr_db = (int8_t)in[3];
	stats->active_antenna = in[4];
	stats->rf_mode = in[5];
	stats->uplink_tx_power = in[6];
	stats->downlink_rssi = in[7];
	stats->downlink_lq = in[8];
	stats->downlink_snr_db = (int8_t)in[9];
}
