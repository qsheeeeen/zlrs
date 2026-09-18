/*
 * 射频抽象：链路层只说"发什么/收多长"，落到芯片由移植层实现
 * （设备上是 usp_zephyr 的 RAC，主机测试上是模拟射频）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/proto/config.h"
#include "zlrs/proto/mod.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t freq_hz;   /* 载波频率 */
	/*
	 * 调制档：FLRC 的速率或 LoRa 的 SF/带宽/编码率/前导码，见
	 * doc/04-protocol.md 4.1.1。移植层按 modulation 分派到 RAC 的 flrc /
	 * lora 参数组。
	 */
	zlrs_mod_cfg_t mod;
	uint8_t sync_slot;  /* 同步字槽：1 控制、2 数据、3 配对 */
	uint8_t sync_word[ZLRS_SYNC_WORD_LEN];
	bool is_tx;
	int8_t tx_power_dbm;
	uint16_t size;        /* 发送字节数 */
	uint16_t max_rx_size; /* 接收缓冲上限 */
	uint16_t timeout_ms;  /* 接收窗口 */
	uint8_t *buf;         /* 收发共用缓冲，接收结果写回这里 */
} zlrs_radio_req_t;

typedef struct {
	/* 提交一次事务，成功返回 0；结果通过 zlrs_link_complete() 回报 */
	int (*submit)(void *ctx, const zlrs_radio_req_t *req);
	/* 32 位随机数，用于会话与配对 nonce；移植层接 TRNG */
	uint32_t (*random)(void *ctx);
	void *ctx;
} zlrs_radio_ops_t;

#ifdef __cplusplus
}
#endif

