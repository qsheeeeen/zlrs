/*
 * 持久化：根密钥、设备标识与配对记录放 NVS，见 doc/10-security.md 第 2 节。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/proto/config.h"

int zlrs_store_init(void);

/*
 * 配对记录：对端身份 + 配对时的两个 nonce（见 doc/10-security.md 第 2、3 节）。
 * 一次配对就是一条记录，没有别的形态。
 */
typedef struct {
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	uint8_t pair_salt[ZLRS_PAIR_SALT_LEN];
} zlrs_store_pairing_t;

/* 返回读到的字节数，没有记录或出错返回负值 */
int zlrs_store_load_root_key(uint8_t out[ZLRS_KEY_LEN]);
int zlrs_store_load_dev_id(uint8_t out[ZLRS_DEV_ID_LEN]);
int zlrs_store_load_pairing(zlrs_store_pairing_t *out);
int zlrs_store_load_failsafe(uint8_t out[ZLRS_CFG_FAILSAFE_LEN]);
int zlrs_store_load_baud(uint32_t *out);

int zlrs_store_save_root_key(const uint8_t key[ZLRS_KEY_LEN]);
int zlrs_store_save_dev_id(const uint8_t id[ZLRS_DEV_ID_LEN]);
int zlrs_store_save_pairing(const zlrs_store_pairing_t *pairing);
int zlrs_store_save_failsafe(const uint8_t channels[ZLRS_CFG_FAILSAFE_LEN]);
int zlrs_store_save_baud(uint32_t baud);

/* 把 64 个十六进制字符解析成根密钥，成功返回 0 */
int zlrs_store_parse_hex_key(const char *hex, uint8_t out[ZLRS_KEY_LEN]);

