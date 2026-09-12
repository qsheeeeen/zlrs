/*
 * 数据串口：串口透传的落地端，中断收发 + 环形缓冲，不阻塞链路线程。
 * 见 docs/01-system.md 2.1（数据 UART 端口）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_APP_SERIAL_PORT_H_
#define ZLRS_APP_SERIAL_PORT_H_

#include <stdint.h>

int zlrs_serial_init(void);

/* 从接收环形缓冲取数据，返回实际取到的字节数 */
uint16_t zlrs_serial_read(uint8_t *buf, uint16_t len);

/* 放进发送环形缓冲并启动中断发送，返回接受的字节数 */
uint16_t zlrs_serial_write(const uint8_t *buf, uint16_t len);

/* 因为缓冲满而丢掉的字节数（best-effort 计数） */
uint32_t zlrs_serial_dropped(void);

#endif /* ZLRS_APP_SERIAL_PORT_H_ */
