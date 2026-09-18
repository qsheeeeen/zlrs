/*
 * 空口遥测载荷：RX 回报本侧测到的链路质量，TX 再组装成 CRSF 统计帧（0x14）。
 * 不复用 CRSF 帧当载荷：那是地面侧格式（含地址与 CRC），空口上多带没意义。
 * 载荷数字是 uplink 方向（RX 测 TX→RX），downlink 由 TX 自己测，见
 * doc/01-system.md 第 3 节。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZLRS_TLM_LEN 8U

/* flags 位定义 */
#define ZLRS_TLM_FLAG_PAIRED 0x0001U
#define ZLRS_TLM_FLAG_SESSION 0x0002U

typedef struct {
	uint8_t lq_percent;   /* 上行链路质量 0–100 */
	uint8_t rssi_neg_dbm; /* 上行 RSSI，dBm 的相反数（CRSF 口径） */
	int8_t snr_db;        /* 上行 SNR；FLRC 下射频栈给不出，填 0 */
	uint8_t rf_mode;      /* 接收端当前速率档编号（见 crsf.h 的 rf_mode） */
	uint8_t tx_power;     /* 接收端发射功率档编号（见 crsf.h 的功率档） */
	uint8_t state;        /* 接收端链路状态（ZLRS_STATE_*），诊断用 */
	uint16_t flags;       /* ZLRS_TLM_FLAG_* */
} zlrs_tlm_t;

void zlrs_tlm_encode(uint8_t out[ZLRS_TLM_LEN], const zlrs_tlm_t *tlm);

/* 载荷长度不对时返回 false，不改动出参 */
bool zlrs_tlm_decode(const uint8_t *in, uint16_t len, zlrs_tlm_t *tlm);

#ifdef __cplusplus
}
#endif

