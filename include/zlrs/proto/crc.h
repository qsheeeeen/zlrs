/*
 * 串口工具用的 CRC8（DVB-S2，多项式 0xD5）。CRSF 帧与配置帧都用它。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 标准检查值："123456789" 的 CRC 是 0xBC */
uint8_t zlrs_crc8(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

