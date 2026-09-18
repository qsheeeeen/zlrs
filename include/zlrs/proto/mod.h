/*
 * 调制档与空中时间模型，见 doc/03-protocol.md 4.1.1。引入 LoRa 后"一帧占多久"
 * 不再是一个除法：由扩频因子/带宽/编码率/前导码决定。超帧、时隙预算、ARQ 超时
 * 与占空比记账都从这里取时间。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_PROTO_MOD_H_
#define ZLRS_PROTO_MOD_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	ZLRS_MOD_FLRC = 0,
	ZLRS_MOD_LORA = 1,
} zlrs_modulation_t;

/*
 * 一个档位的完整描述。结构体按"最大成员先摆"排，便于两端（设备与主机）用同样的
 * sizeof 比较，见 zlrs_mod_equal()。
 */
typedef struct {
	uint32_t rate_bps;  /* FLRC：原始速率档；LoRa 不用，置 0 */
	uint32_t bw_hz;     /* LoRa：带宽（125k / 250k / 500k）；FLRC 不用 */
	uint8_t modulation; /* ZLRS_MOD_* */
	uint8_t sf;         /* LoRa：扩频因子 5..12 */
	uint8_t cr;         /* LoRa：编码率 1..4 表示 4/5..4/8 */
	uint8_t preamble;   /* LoRa：前导码符号数 */
	uint8_t sync_byte;  /* LoRa：1 字节同步字（最高位是 private bit） */
	uint8_t reserved[3];
} zlrs_mod_cfg_t;

/* 两档是否完全一致（速率自适应用它判断"要不要换档"） */
bool zlrs_mod_equal(const zlrs_mod_cfg_t *a, const zlrs_mod_cfg_t *b);

/* FLRC 档：只有速率有意义 */
zlrs_mod_cfg_t zlrs_mod_flrc(uint32_t rate_bps);

/*
 * LoRa 档：SF / 带宽 / 编码率 / 前导码符号数。同步字由密钥材料派生，配对之后
 * 才填得进去，所以这里给 0，由链路层补。
 */
zlrs_mod_cfg_t zlrs_mod_lora(uint8_t sf, uint32_t bw_hz, uint8_t cr, uint8_t preamble);

/*
 * 一帧的空中时间（µs，向上取整）。
 *   FLRC：载荷 + 4 字节（前导/同步/长度）按原始速率折算
 *   LoRa：前导码 (n + 4.25) 个符号 + 载荷符号，按 Semtech 的 LoRa 时间公式
 * 参数不合法（速率为 0、带宽为 0、SF 越界）时返回 0。
 */
uint32_t zlrs_mod_airtime_us(const zlrs_mod_cfg_t *mod, uint16_t frame_len);

/* 同一笔账按毫秒给（向上取整），占空比记账用 */
uint32_t zlrs_mod_airtime_ms(const zlrs_mod_cfg_t *mod, uint16_t frame_len);

#ifdef __cplusplus
}
#endif

#endif /* ZLRS_PROTO_MOD_H_ */
