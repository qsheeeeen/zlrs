/*
 * 协议核心的编译期配置。分层：lib/proto（协议核心）、lib/link（链路层）都不
 * 依赖 Zephyr、主机可直接测；Zephyr 粘合只在 app/。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_PROTO_CONFIG_H_
#define ZLRS_PROTO_CONFIG_H_

/*
 * 空口协议版本：不兼容的改动递增一次，写在 BEACON 里，RX 只认同版本。
 * 1：tag/会话密钥按双方 dev_id 派生（方向由顺序编码），pair_key 掺配对 nonce，
 *    会话密钥掺三次握手的两个 nonce，见 doc/02-security.md 第 3 节。
 */
#define ZLRS_PROTO_VERSION 1U

/* 帧：header(1) + seq(2) + payload + tag(0/4)，CRC 由射频硬件生成 */
#define ZLRS_FRAME_HDR_LEN 3U
#define ZLRS_TAG_LEN 4U
#define ZLRS_FRAME_MAX_LEN 255U
#define ZLRS_PAYLOAD_MAX_LEN \
	(ZLRS_FRAME_MAX_LEN - ZLRS_FRAME_HDR_LEN - ZLRS_TAG_LEN)

/*
 * 分片重组超时。ARQ 的重传周期是 30 ms，这里要留足余量，否则重传过程中
 * 半截分片会被重组器丢掉。
 */
#define ZLRS_FRAG_TIMEOUT_MS 100U
#define ZLRS_REASM_BUF_LEN 512U

/* 同步字：4 字节，槽 1 控制链路、槽 2 数据链路、槽 3 配对 */
#define ZLRS_SYNC_WORD_LEN 4U
#define ZLRS_SYNC_SLOT_CONTROL 1U
#define ZLRS_SYNC_SLOT_DATA 2U
#define ZLRS_SYNC_SLOT_PAIRING 3U

/* 跳频表长度，必须是频点数的整数倍 */
#define ZLRS_FHSS_SEQUENCE_LEN 256U

/* 密钥与设备标识 */
#define ZLRS_KEY_LEN 32U
#define ZLRS_DEV_ID_LEN 4U

/* 配对时的两个 nonce（发起方 || 响应方），参与 pair_key 派生并落盘 */
#define ZLRS_PAIR_SALT_LEN 8U

/* 设备角色 */
#define ZLRS_ROLE_TX 0U
#define ZLRS_ROLE_RX 1U

/* 控制链路默认帧率（Hz） */
#define ZLRS_FRAME_RATE_HZ 150U

/* 控制链路 RC 载荷：CRSF 的 22 字节通道数据原样搬运 */
#define ZLRS_RC_PAYLOAD_LEN 22U

/* 配置协议里的失控值：就是同一份 22 字节 CRSF 通道载荷 */
#define ZLRS_CFG_FAILSAFE_LEN ZLRS_RC_PAYLOAD_LEN

#endif /* ZLRS_PROTO_CONFIG_H_ */
