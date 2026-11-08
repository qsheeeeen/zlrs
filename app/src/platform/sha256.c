/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SHA-256 through the Zephyr Crypto API. This file owns device access, session
 * serialization and fatal driver errors; protocol algorithms live in app/src/proto.
 */

#include <zephyr/crypto/crypto.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "platform/sha256.h"
#include "trace.h"

BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_NODELABEL(sha256), okay),
	     "ZLRS needs an enabled SHA-256 peripheral");
BUILD_ASSERT(IS_ENABLED(CONFIG_ZLRS_RP2350_SHA256), "ZLRS needs the RP2350 SHA-256 driver");

LOG_MODULE_REGISTER(zlrs_sha256, CONFIG_APP_LOG_LEVEL);

static const struct device *const hash_dev = DEVICE_DT_GET(DT_NODELABEL(sha256));

/* Priority inheritance keeps a low-priority caller from delaying the RF owner. */
K_MUTEX_DEFINE(hash_mutex);

static struct hash_ctx hash;

/* Both bounded reservations and ordinary hashes use this one driver session.
 * The caller holds hash_mutex, including throughout a reserved sequence. */
static int begin_hash(uint32_t started, uint32_t budget_us)
{
	__ASSERT_NO_MSG(hash.drv_sessn_state == NULL);
	if (!device_is_ready(hash_dev)) {
		return -ENODEV;
	}
	hash = (struct hash_ctx){.flags = CAP_SEPARATE_IO_BUFS | CAP_SYNC_OPS};
	while (k_cyc_to_us_floor32(k_cycle_get_32() - started) < budget_us) {
		const int rc = hash_begin_session(hash_dev, &hash, CRYPTO_HASH_ALGO_SHA256);
		if (rc != -EBUSY) {
			return rc;
		}
		const uint32_t elapsed = k_cyc_to_us_floor32(k_cycle_get_32() - started);
		if (elapsed >= budget_us) {
			break;
		}
		k_usleep(MIN(50U, budget_us - elapsed));
	}
	return -ETIMEDOUT;
}

bool zlrs_hash_acquire(uint32_t budget_us)
{
	const uint32_t started = k_cycle_get_32();
	if (budget_us == 0U || k_mutex_lock(&hash_mutex, K_USEC(budget_us)) != 0) {
		return false;
	}
	const int rc = begin_hash(started, budget_us);
	if (rc != 0) {
		k_mutex_unlock(&hash_mutex);
		if (rc != -ETIMEDOUT) {
			LOG_ERR("SHA-256 reservation failed: %d", rc);
			k_panic();
		}
	}
	return rc == 0;
}

void zlrs_hash_release(void)
{
	__ASSERT_NO_MSG(hash.drv_sessn_state != NULL);
	const int rc = hash_free_session(hash_dev, &hash);
	k_mutex_unlock(&hash_mutex);
	if (rc != 0) {
		LOG_ERR("SHA-256 release failed: %d", rc);
		k_panic();
	}
}

/* Feed both spans in order without copying. The driver retains partial words and
 * SHA state across update/compute; freeing releases it for other driver/ROM users. */
void zlrs_sha256(const uint8_t *first, size_t first_len, const uint8_t *second, size_t second_len,
		 uint8_t out[ZLRS_SHA256_LEN])
{
	struct hash_pkt pkt = {.in_buf = first, .in_len = first_len};
	int rc;

	ZLRS_TRACE_EVENT("hash_begin", first_len, second_len);
	if (k_mutex_lock(&hash_mutex, K_FOREVER) != 0) {
		k_panic();
	}
	const bool local = hash.drv_sessn_state == NULL;
	rc = local ? begin_hash(k_cycle_get_32(), 100000U) : 0;
	if (rc == 0) {
		rc = hash_update(&hash, &pkt);
		if (rc == 0) {
			pkt.in_buf = second;
			pkt.in_len = second_len;
			pkt.out_buf = out;
			rc = hash_compute(&hash, &pkt);
		}
		if (local) {
			const int free_rc = hash_free_session(hash_dev, &hash);
			if (rc == 0) {
				rc = free_rc;
			}
		}
	}
	k_mutex_unlock(&hash_mutex);
	ZLRS_TRACE_EVENT("hash_end", first_len, second_len);
	if (rc != 0) {
		/* Never turn a failed driver operation into an authentication tag. */
		LOG_ERR("SHA-256 driver failed: %d", rc);
		k_panic();
	}
}
