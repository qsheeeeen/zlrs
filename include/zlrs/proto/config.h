/*
 * zlrs 协议核心的编译期配置。
 *
 * 工程分三层：
 *   - lib/proto：纯 C、无 RTOS 依赖的协议核心，主机上可直接编译测试
 *   - lib/link：链路层，把帧翻译成射频事务，同样不依赖 Zephyr
 *   - app/：Zephyr 侧粘合（线程、UART、USP/RAC）
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_PROTO_CONFIG_H_
#define ZLRS_PROTO_CONFIG_H_

/* 空口协议版本：参与标签派生，版本不同的两端不互通 */
#define ZLRS_PROTO_VERSION 0U

/* 帧：header(1) + seq(2) + payload + tag(0/4)，CRC 由射频硬件生成 */
#define ZLRS_FRAME_HDR_LEN 3U
#define ZLRS_TAG_LEN 4U
#define ZLRS_FRAME_MAX_LEN 255U
#define ZLRS_PAYLOAD_MAX_LEN \
	(ZLRS_FRAME_MAX_LEN - ZLRS_FRAME_HDR_LEN - ZLRS_TAG_LEN)

/* 分片重组 */
#define ZLRS_FRAG_TIMEOUT_MS 20U
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

/* 设备角色 */
#define ZLRS_ROLE_TX 0U
#define ZLRS_ROLE_RX 1U

/* 控制链路默认帧率（Hz） */
#define ZLRS_FRAME_RATE_HZ 150U

/* 控制链路 RC 载荷：CRSF 的 22 字节通道数据原样搬运 */
#define ZLRS_RC_PAYLOAD_LEN 22U

#endif /* ZLRS_PROTO_CONFIG_H_ */
