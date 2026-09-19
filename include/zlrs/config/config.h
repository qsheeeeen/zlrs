/*
 * 配置协议：配置模式下跑在配置 CDC 上，命令表见 doc/10-security.md 第 10 节。
 * 纯 C 的"协议 + 分发"，落盘与链路动作由 backend 回调交给应用——主机侧拿假
 * backend 就能把每条命令跑一遍（tests/host）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zlrs/proto/config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZLRS_CFG_SOF0 0xA5U
#define ZLRS_CFG_SOF1 0x5AU
#define ZLRS_CFG_VERSION 2U
#define ZLRS_CFG_FRAME_MAX_LEN 96U
#define ZLRS_CFG_PAYLOAD_MAX_LEN (ZLRS_CFG_FRAME_MAX_LEN - 6U)

/* 主机 → 设备 */
#define ZLRS_CFG_CMD_ENTER 0x00U       /* 进配置模式（正式入口） */
#define ZLRS_CFG_CMD_STATUS 0x01U      /* 读状态 */
#define ZLRS_CFG_CMD_PAIR 0x02U        /* 触发配对 */
#define ZLRS_CFG_CMD_ERASE_PAIR 0x03U  /* 擦除配对记录 */
#define ZLRS_CFG_CMD_SET_KEY_HEX 0x10U /* 64 个十六进制字符的根密钥 */
#define ZLRS_CFG_CMD_SET_PHRASE 0x11U  /* 短语，设备侧派生根密钥 */
#define ZLRS_CFG_CMD_BUILD 0x12U       /* 读固件版本串（升级后核对跑的是哪一版） */
#define ZLRS_CFG_CMD_IMAGE_HASH 0x13U  /* 读当前镜像的 MCUboot 哈希（升级/回滚判据） */
#define ZLRS_CFG_CMD_SET_FAILSAFE 0x20U /* 22 字节 CRSF 通道载荷 */
#define ZLRS_CFG_CMD_SET_BAUD 0x21U    /* 4 字节小端波特率 */
#define ZLRS_CFG_CMD_SET_REGION 0x22U  /* 1 字节地区号（0..3） */
#define ZLRS_CFG_CMD_SET_POWER 0x23U   /* 1 字节有符号 dBm */
#define ZLRS_CFG_CMD_EXIT 0x30U        /* 退出配置模式 */
#define ZLRS_CFG_CMD_FACTORY_RESET 0x40U /* 清 NVS */
#define ZLRS_CFG_CMD_RECOVERY 0x41U    /* 重启进 MCUboot 串口恢复 */
#define ZLRS_CFG_CMD_CONFIRM 0x42U     /* 上位机确认这一版升级成功（不确认就回滚） */

/* 设备 → 主机 */
#define ZLRS_CFG_CMD_ACK 0x80U         /* [cmd][status] */
#define ZLRS_CFG_CMD_STATUS_RESP 0x81U /* 见 zlrs_cfg_status_t */
#define ZLRS_CFG_CMD_BUILD_RESP 0x82U  /* 固件版本串，最长 ZLRS_CFG_BUILD_MAX_LEN */
#define ZLRS_CFG_CMD_IMAGE_HASH_RESP 0x83U /* ZLRS_CFG_IMAGE_HASH_LEN 字节镜像哈希 */

/* 状态码 */
#define ZLRS_CFG_OK 0U
#define ZLRS_CFG_E_PARAM 1U
#define ZLRS_CFG_E_STATE 2U
#define ZLRS_CFG_E_STORE 3U
#define ZLRS_CFG_E_UNKNOWN 4U

/* 状态响应：固定的 20 字节，字段顺序变了就改版本号 */
#define ZLRS_CFG_STATUS_LEN 20U
#define ZLRS_CFG_PHRASE_MAX_LEN 32U
#define ZLRS_CFG_BUILD_MAX_LEN 32U
#define ZLRS_CFG_IMAGE_HASH_LEN 32U

typedef struct {
	uint8_t version;    /* 协议版本 */
	uint8_t role;       /* 0 = TX，1 = RX */
	uint8_t state;      /* ZLRS_STATE_* */
	uint8_t flags;      /* bit0 已配置根密钥，bit1 已配对，bit2 会话有效 */
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	uint8_t lq_percent;
	uint8_t band;        /* 本机 sub-G 硬件：0 = LF（470/433），1 = HF（868/915） */
	uint8_t region;      /* 当前地区号（0..3） */
	int8_t tx_power_dbm; /* 当前发射功率上限 */
	uint32_t data_baud;  /* 数据口波特率（下次启动生效） */
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
	int (*save_region)(void *ctx, uint8_t region);
	int (*save_power)(void *ctx, int8_t dbm);
	int (*pair)(void *ctx);
	int (*erase_pair)(void *ctx);
	int (*factory_reset)(void *ctx);
	int (*enter_recovery)(void *ctx);
	/* 上位机确认这一版：返回 ZLRS_CFG_OK 表示已记下（升级流程靠它决定去留） */
	int (*confirm_image)(void *ctx);
	/* 固件版本串；没实现就回 E_STATE */
	int (*read_build)(void *ctx, char *out, size_t out_len);
	/* 当前镜像的 MCUboot 哈希（工具据此判断跑的是新版还是回滚后的旧版） */
	int (*read_image_hash)(void *ctx, uint8_t out[ZLRS_CFG_IMAGE_HASH_LEN]);
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
