/*
 * 持久化：根密钥、设备标识与配对记录放 NVS，见 doc/02-security.md 第 2 节。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_APP_STORE_H_
#define ZLRS_APP_STORE_H_

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/proto/config.h"

int zlrs_store_init(void);

/* 返回读到的字节数，没有记录或出错返回负值 */
int zlrs_store_load_root_key(uint8_t out[ZLRS_KEY_LEN]);
int zlrs_store_load_dev_id(uint8_t out[ZLRS_DEV_ID_LEN]);
int zlrs_store_load_peer(uint8_t out[ZLRS_DEV_ID_LEN]);

int zlrs_store_save_dev_id(const uint8_t id[ZLRS_DEV_ID_LEN]);
int zlrs_store_save_peer(const uint8_t id[ZLRS_DEV_ID_LEN]);
int zlrs_store_clear_peer(void);

/* 把 64 个十六进制字符解析成根密钥，成功返回 0 */
int zlrs_store_parse_hex_key(const char *hex, uint8_t out[ZLRS_KEY_LEN]);

#endif /* ZLRS_APP_STORE_H_ */
