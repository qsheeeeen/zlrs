/*
 * 配置协议：设备在配置模式下跑在数据通路上，规则见 doc/02-security.md 第 10 节
 * 与 doc/01-system.md 2.1。
 *
 * 帧形状：
 *
 *   [0xA5][0x5A][ver][cmd][len][payload ...][crc8]
 *     ver = 1，len = 载荷长度，crc8 覆盖 ver..payload（与 CRSF 同一个多项式）
 *
 * 这个库是纯 C 的"协议 + 命令分发"，落盘与链路动作通过 backend 回调交给应用：
 * 主机侧拿一个假 backend 就能把每条命令跑一遍（见 tests/host）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_CONFIG_CONFIG_H_
#define ZLRS_CONFIG_CONFIG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zlrs/proto/config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZLRS_CFG_SOF0 0xA5U
#define ZLRS_CFG_SOF1 0x5AU
#define ZLRS_CFG_VERSION 1U
#define ZLRS_CFG_FRAME_MAX_LEN 96U
#define ZLRS_CFG_PAYLOAD_MAX_LEN (ZLRS_CFG_FRAME_MAX_LEN - 6U)

/* 主机 → 设备 */
#define ZLRS_CFG_CMD_STATUS 0x01U      /* 读状态 */
#define ZLRS_CFG_CMD_PAIR 0x02U        /* 触发配对 */
#define ZLRS_CFG_CMD_SET_KEY_HEX 0x10U /* 64 个十六进制字符的根密钥 */
#define ZLRS_CFG_CMD_SET_PHRASE 0x11U  /* 短语，设备侧派生根密钥 */
#define ZLRS_CFG_CMD_SET_FAILSAFE 0x20U /* 22 字节 CRSF 通道载荷 */
#define ZLRS_CFG_CMD_SET_BAUD 0x21U    /* 4 字节小端波特率 */
#define ZLRS_CFG_CMD_EXIT 0x30U        /* 退出配置模式 */

/* 设备 → 主机 */
#define ZLRS_CFG_CMD_ACK 0x80U         /* [cmd][status] */
#define ZLRS_CFG_CMD_STATUS_RESP 0x81U /* 见 zlrs_cfg_status_t */

/* 状态码 */
#define ZLRS_CFG_OK 0U
#define ZLRS_CFG_E_PARAM 1U
#define ZLRS_CFG_E_STATE 2U
#define ZLRS_CFG_E_STORE 3U
#define ZLRS_CFG_E_UNKNOWN 4U

/* 状态响应：固定的 16 字节，字段顺序变了就改版本号 */
#define ZLRS_CFG_STATUS_LEN 16U
#define ZLRS_CFG_PHRASE_MAX_LEN 32U

typedef struct {
	uint8_t version;    /* 协议版本 */
	uint8_t role;       /* 0 = TX，1 = RX */
	uint8_t state;      /* ZLRS_STATE_* */
	uint8_t flags;      /* bit0 已配置根密钥，bit1 已配对，bit2 会话有效 */
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	uint8_t lq_percent;
	uint8_t reserved;
} zlrs_cfg_status_t;

#define ZLRS_CFG_FLAG_KEY 0x01U
#define ZLRS_CFG_FLAG_PAIRED 0x02U
#define ZLRS_CFG_FLAG_SESSION 0x04U

/*
 * 设备侧的动作。每个回调返回 ZLRS_CFG_OK 或 ZLRS_CFG_E_*；
 * ctx 由调用方给，回调可以实现成"写到 NVS"或主机测试里的假实现。
 */
typedef struct {
	int (*save_root_key)(void *ctx, const uint8_t key[ZLRS_KEY_LEN]);
	int (*read_status)(void *ctx, zlrs_cfg_status_t *out);
	int (*save_failsafe)(void *ctx, const uint8_t channels[22]);
	int (*save_baud)(void *ctx, uint32_t baud);
	int (*pair)(void *ctx);
	void *ctx;
} zlrs_cfg_backend_t;

/* 逐字节解析配置帧的状态机 */
typedef struct {
	uint8_t buf[ZLRS_CFG_FRAME_MAX_LEN];
	uint8_t len;
	uint8_t need;
	uint8_t cmd;
	uint8_t payload_len;
	uint32_t frames_ok;
	uint32_t frames_bad;
} zlrs_cfg_parser_t;

#define ZLRS_CFG_PAYLOAD(p) ((p)->buf + 5U)

void zlrs_cfg_parser_init(zlrs_cfg_parser_t *p);

/* 返回 1 表示凑够一帧（cmd / payload 可读），0 表示还没凑够，-1 表示这一片不合法 */
int zlrs_cfg_feed(zlrs_cfg_parser_t *p, uint8_t byte);

/* 组帧；返回整帧长度，参数不合法返回 0 */
uint8_t zlrs_cfg_encode(uint8_t *out, size_t out_len, uint8_t cmd, const uint8_t *payload,
			uint8_t payload_len);

/*
 * 处理一条命令并把响应帧写进 out（含 SOF 与 CRC）。
 * 返回响应帧长度，0 表示这条命令不需要响应。
 */
uint8_t zlrs_cfg_dispatch(const zlrs_cfg_backend_t *backend, uint8_t cmd,
			  const uint8_t *payload, uint8_t payload_len, uint8_t *out,
			  size_t out_len);

/* 状态结构体 ↔ 定长字节 */
void zlrs_cfg_status_encode(uint8_t out[ZLRS_CFG_STATUS_LEN],
			    const zlrs_cfg_status_t *status);
bool zlrs_cfg_status_decode(const uint8_t *in, uint8_t len, zlrs_cfg_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* ZLRS_CONFIG_CONFIG_H_ */
