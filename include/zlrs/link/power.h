/*
 * 动态发射功率：按对端遥测回报的上行 RSSI/LQ 升降本端功率；收不到遥测就直接回到
 * 上限（那一头多半掉线，先打满再谈省电）。与速率自适应同一套策略骨架：连续窗口
 * + 保持时间 + 滞回，阈值待实测标定，见 doc/03-protocol.md 4.6。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	/* 策略参数 */
	int8_t min_dbm;      /* 信号好时最低降到这 */
	int8_t max_dbm;      /* 上限：配置功率与地区上限取小 */
	int8_t step_db;      /* 每次调整的步长 */
	int8_t rssi_low_dbm; /* 低于它就升功率 */
	int8_t rssi_high_dbm;/* 高于它（且 LQ 够好）才降功率 */
	uint8_t lq_low;      /* LQ 低于它就升功率（%） */
	uint8_t lq_high;     /* LQ 高于它才降功率（%） */
	uint8_t windows;     /* 连续多少个样本才动一次 */
	uint32_t hold_ms;    /* 两次调整之间的最小间隔 */
	uint32_t tlm_timeout_ms; /* 多久没有遥测就回到上限 */

	/* 状态 */
	int8_t current_dbm;
	uint8_t good;
	uint8_t bad;
	uint32_t last_change_ms;
	uint32_t last_tlm_ms;
	bool tlm_seen;
	uint32_t raises;   /* 升功率次数 */
	uint32_t lowers;   /* 降功率次数 */
	uint32_t fallbacks;/* 因遥测超时回到上限的次数 */
} zlrs_power_ctl_t;

/*
 * configured_dbm：配置/地区允许的上限（动态功率不会超过它）
 * floor_dbm：最低功率（配置项）
 */
void zlrs_power_init(zlrs_power_ctl_t *ctl, int8_t configured_dbm, int8_t floor_dbm,
		     uint32_t now_ms);

/*
 * 每次收到遥测（valid = true，rssi 是本端信号在对端测到的 dBm）或定时轮询
 * （valid = false，用于遥测超时判定）时调用。
 * 需要改变时返回 true，并把新功率写进 *out_dbm。
 */
bool zlrs_power_update(zlrs_power_ctl_t *ctl, bool valid, int16_t rssi_dbm,
		       uint8_t lq_percent, uint32_t now_ms, int8_t *out_dbm);

int8_t zlrs_power_current_dbm(const zlrs_power_ctl_t *ctl);

#ifdef __cplusplus
}
#endif

