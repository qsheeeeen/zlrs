/*
 * 通用查表 CRC（8..16 位任意位宽），用于空口帧校验。
 *
 * 算法：表项 crc = i << (bits - 8)，8 次移位，最高位为 1 时异或 poly；
 * 计算时 crc = (crc << 8) ^ tab[((crc >> (bits - 8)) ^ data) & 0xFF]，
 * 最后与 bitmask 相与只保留 bits 位。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_PROTO_CRC_H_
#define ZLRS_PROTO_CRC_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint8_t bits;     /* 8..16 */
	uint16_t poly;
	uint16_t bitmask; /* (1 << bits) - 1 */
	uint16_t tab[256];
} zlrs_crc_t;

/*
 * 初始化查表。可以对同一个对象反复调用；位宽与多项式相同时直接返回。
 */
void zlrs_crc_init(zlrs_crc_t *crc, uint8_t bits, uint16_t poly);

/*
 * 增量计算。首次调用时 seed 直接传种子即可，不需要先归零：
 * 计算过程中旧值只通过高字节参与。
 */
uint16_t zlrs_crc_calc(zlrs_crc_t *crc, const uint8_t *data, uint8_t len,
		       uint16_t seed);

#ifdef __cplusplus
}
#endif

#endif /* ZLRS_PROTO_CRC_H_ */
