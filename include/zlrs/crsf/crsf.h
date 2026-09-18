/*
 * CRSF 帧编解码（地面侧接口，不走空口）：帧与 CRC 按 CRSF 惯例（DVB-S2 多项式
 * 0xD5），只实现 zlrs 用到的 RC_CHANNELS_PACKED 与 LINK_STATISTICS 两种。
 *
 * 取舍：CRSF 在我们的方案里只是端口格式（手台 ↔ 模块、接收机 ↔ 飞控），
 * 与 ExpressLRS 的空口协议无关；地址字段解析时不校验（手台与飞控用的地址
 * 不止一个），发送时由调用方给出，见 doc/01-system.md 2.1。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_CRSF_CRSF_H_
#define ZLRS_CRSF_CRSF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zlrs/proto/mod.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 发送地址：常规惯例，接收端不校验 */
#define ZLRS_CRSF_ADDR_FC 0xC8U          /* 接收机 → 飞控、手台 → 模块 */
#define ZLRS_CRSF_ADDR_TX_MODULE 0xEAU   /* 模块 → 手台 */

/* 帧类型 */
#define ZLRS_CRSF_TYPE_LINK_STATISTICS 0x14U
#define ZLRS_CRSF_TYPE_RC_CHANNELS 0x16U

/* 帧长限制来自 CRSF：整帧不超过 64 字节 */
#define ZLRS_CRSF_FRAME_MAX_LEN 64U
#define ZLRS_CRSF_PAYLOAD_MAX_LEN \
	(ZLRS_CRSF_FRAME_MAX_LEN - 4U) /* addr + len + type + crc */

/* 通道值：11 bit，1000–2000 µs 对应 172–1811 */
#define ZLRS_CRSF_CHANNEL_COUNT 16U
#define ZLRS_CRSF_CHANNEL_MIN 172U
#define ZLRS_CRSF_CHANNEL_MID 992U
#define ZLRS_CRSF_CHANNEL_MAX 1811U
#define ZLRS_CRSF_CHANNEL_MASK 0x07FFU
#define ZLRS_CRSF_RC_PAYLOAD_LEN 22U

#define ZLRS_CRSF_LINK_STATS_LEN 10U

/* 解析结果 */
enum zlrs_crsf_result {
	ZLRS_CRSF_NONE = 0,  /* 还没凑够一帧 */
	ZLRS_CRSF_FRAME,     /* 收到完整且 CRC 正确的一帧 */
	ZLRS_CRSF_BAD_FRAME, /* 形状不合法（地址、长度），已丢弃 */
	ZLRS_CRSF_BAD_CRC,   /* CRC 不匹配，已丢弃 */
};

/*
 * 逐字节解析状态机。凑够一帧后：type 在 p->type，payload 在
 * ZLRS_CRSF_PAYLOAD(p)，长度在 p->payload_len。
 */
typedef struct {
	uint8_t buf[ZLRS_CRSF_FRAME_MAX_LEN];
	uint8_t len;         /* 已收字节数 */
	uint8_t need;        /* 本帧总长；0 表示还在等地址字节 */
	uint8_t type;        /* 最近一帧的类型 */
	uint8_t payload_len; /* 最近一帧的载荷长度 */
	uint32_t frames_ok;
	uint32_t frames_bad;
} zlrs_crsf_parser_t;

#define ZLRS_CRSF_PAYLOAD(p) ((p)->buf + 3U)

void zlrs_crsf_parser_init(zlrs_crsf_parser_t *p);
enum zlrs_crsf_result zlrs_crsf_feed(zlrs_crsf_parser_t *p, uint8_t byte);

/* CRC8，多项式 0xD5；"123456789" 的校验值是 0xBC */
uint8_t zlrs_crsf_crc8(const uint8_t *data, size_t len);

/* 组帧；返回整帧长度，参数不合法时返回 0 */
uint8_t zlrs_crsf_encode(uint8_t *out, uint8_t addr, uint8_t type, const uint8_t *payload,
			 uint8_t payload_len);

/* 16 通道 ↔ 22 字节；pack 只取低 11 bit */
void zlrs_crsf_channels_pack(uint8_t out[ZLRS_CRSF_RC_PAYLOAD_LEN],
			     const uint16_t channels[ZLRS_CRSF_CHANNEL_COUNT]);
void zlrs_crsf_channels_unpack(const uint8_t in[ZLRS_CRSF_RC_PAYLOAD_LEN],
			       uint16_t channels[ZLRS_CRSF_CHANNEL_COUNT]);

/* 默认失控值：油门（通道 0）最低，其余中位——与固件起手的默认输出一致 */
void zlrs_crsf_channels_default(uint16_t channels[ZLRS_CRSF_CHANNEL_COUNT]);

/* RSSI：CRSF 用"dBm 的相反数"表示强度，-70 dBm → 70 */
uint8_t zlrs_crsf_rssi_from_dbm(int16_t dbm);

/* 发射功率档索引：0=0mW 1=10 2=25 3=100 4=500 5=1000 6=2000 7=250 8=50 */
uint8_t zlrs_crsf_tx_power_index(int8_t dbm);

/*
 * 速率档编号：zlrs 自己定，只要求两端一致
 * （FLRC 2.6M=0 / 1.3M=1 / 0.65M=2 / 0.325M=3，LoRa = 0x10 | SF）
 */
uint8_t zlrs_crsf_rf_mode(uint32_t rate_bps);

/* 调制感知版本：LoRa 档按 SF 编码，其余与上面一致 */
uint8_t zlrs_crsf_rf_mode_mod(const zlrs_mod_cfg_t *mod);

typedef struct {
	uint8_t uplink_rssi_ant1; /* dBm 的相反数，见 zlrs_crsf_rssi_from_dbm() */
	uint8_t uplink_rssi_ant2; /* 本设计单天线，填同值 */
	uint8_t uplink_lq;        /* 0–100 % */
	int8_t uplink_snr_db;
	uint8_t active_antenna;   /* 0 = 第一天线 */
	uint8_t rf_mode;          /* 见 zlrs_crsf_rf_mode() */
	uint8_t uplink_tx_power;  /* 见 zlrs_crsf_tx_power_index() */
	uint8_t downlink_rssi;
	uint8_t downlink_lq;
	int8_t downlink_snr_db;
} zlrs_crsf_link_stats_t;

void zlrs_crsf_link_stats_encode(uint8_t out[ZLRS_CRSF_LINK_STATS_LEN],
				 const zlrs_crsf_link_stats_t *stats);
void zlrs_crsf_link_stats_decode(const uint8_t in[ZLRS_CRSF_LINK_STATS_LEN],
				 zlrs_crsf_link_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* ZLRS_CRSF_CRSF_H_ */
