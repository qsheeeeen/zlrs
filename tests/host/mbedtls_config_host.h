/*
 * 主机侧测试用的最小 mbedTLS 配置：只开协议用到的模块。
 * 设备侧用 Zephyr 的 config-mbedtls.h，两边是同一份 mbedTLS 源码。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MBEDTLS_CONFIG_HOST_H_
#define MBEDTLS_CONFIG_HOST_H_

#define MBEDTLS_SHA256_C
#define MBEDTLS_MD_C
#define MBEDTLS_HKDF_C

#endif /* MBEDTLS_CONFIG_HOST_H_ */
