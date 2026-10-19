/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SHA-256 through the Zephyr Crypto API. This file owns device access, session
 * serialization and fatal driver errors; protocol algorithms live in core/proto.
 */

#include <zephyr/crypto/crypto.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "sha256.h"

BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_NODELABEL(sha256), okay),
	     "ZLRS needs an enabled SHA-256 peripheral");
BUILD_ASSERT(IS_ENABLED(CONFIG_ZLRS_RP2350_SHA256), "ZLRS needs the RP2350 SHA-256 driver");

LOG_MODULE_REGISTER(zlrs_sha256, CONFIG_APP_LOG_LEVEL);

static const struct device *const hash_dev = DEVICE_DT_GET(DT_NODELABEL(sha256));

/* Priority inheritance keeps a low-priority caller from delaying the RF owner. */
K_MUTEX_DEFINE(hash_mutex);

/* Feed both spans in order without copying. The driver retains partial words and
 * SHA state across update/compute; freeing releases it for other driver/ROM users. */
void zlrs_sha256(const uint8_t *first, size_t first_len, const uint8_t *second, size_t second_len,
		 uint8_t out[ZLRS_SHA256_LEN])
{
	struct hash_ctx ctx = {.flags = CAP_SEPARATE_IO_BUFS | CAP_SYNC_OPS};
	struct hash_pkt pkt = {.in_buf = first, .in_len = first_len};
	int rc;

	if (!device_is_ready(hash_dev)) {
		LOG_ERR("SHA-256 device unavailable");
		k_panic();
	}
	if (k_mutex_lock(&hash_mutex, K_FOREVER) != 0) {
		k_panic();
	}
	const int64_t lock_started = k_uptime_get();

	do {
		rc = hash_begin_session(hash_dev, &ctx, CRYPTO_HASH_ALGO_SHA256);
		if (rc != -EBUSY || k_uptime_get() - lock_started >= 100) {
			break;
		}
		k_usleep(50);
	} while (true);
	if (rc == 0) {
		rc = hash_update(&ctx, &pkt);
		if (rc == 0) {
			pkt.in_buf = second;
			pkt.in_len = second_len;
			pkt.out_buf = out;
			rc = hash_compute(&ctx, &pkt);
		}
		const int free_rc = hash_free_session(hash_dev, &ctx);

		if (rc == 0) {
			rc = free_rc;
		}
	}
	k_mutex_unlock(&hash_mutex);
	if (rc != 0) {
		/* Never turn a failed driver operation into an authentication tag. */
		LOG_ERR("SHA-256 driver failed: %d", rc);
		k_panic();
	}
}
