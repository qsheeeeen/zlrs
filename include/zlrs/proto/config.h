/*
 * zlrs 协议核心的编译期配置。
 *
 * 工程分三层：
 *   - lib/proto：纯 C、无 RTOS 依赖的协议核心，可在主机上直接编译测试
 *   - drivers/：射频驱动（LR2021）
 *   - app/：Zephyr 侧粘合（线程、UART、SPI、GPIO）
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_PROTO_CONFIG_H_
#define ZLRS_PROTO_CONFIG_H_

/*
 * 空口协议版本。参与 UID/FHSS 种子派生与同步包内容，
 * 版本不一致的两端无法互通。
 */
#define ZLRS_PROTO_VERSION 0U

/* 设备 UID 长度 */
#define ZLRS_UID_LEN 6U

/* FHSS 序列表长度 */
#define ZLRS_FHSS_SEQUENCE_LEN 256U

/* 设备角色 */
#define ZLRS_ROLE_TX 0
#define ZLRS_ROLE_RX 1

/* 串口入口 FIFO 容量 */
#define ZLRS_SERIAL_BUF_LEN 64U

/*
 * TODO(帧格式)：帧头类型、payload 布局、CRC 位宽、透传每包字节数
 * 都还没定。等 docs/design.md 第 6 节的帧格式定稿后补到本文件。
 */

#endif /* ZLRS_PROTO_CONFIG_H_ */
