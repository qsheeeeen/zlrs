/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/random/random.h>
#include <zephyr/ztest.h>
#include <errno.h>
#include <hardware/structs/trng.h>
#include <string.h>

ZTEST(rp2350_trng, test_successive_blocks)
{
	const struct device *const dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
	uint8_t first[32];
	uint8_t second[32];

	zassert_true(device_is_ready(dev), "entropy driver unavailable");
	zassert_ok(entropy_get_entropy(dev, first, sizeof(first)));
	zassert_ok(entropy_get_entropy(dev, second, sizeof(second)));
	zassert_not_equal(memcmp(first, second, sizeof(first)), 0, "successive TRNG blocks repeat");
}

ZTEST(rp2350_trng, test_secure_request_sizes)
{
	static const size_t sizes[] = {0U, 1U, 4U, 23U, 24U, 25U, 32U, 48U};
	uint8_t buffer[50];
	for (size_t i = 0U; i < ARRAY_SIZE(sizes); i++) {
		memset(buffer, 0xa5, sizeof(buffer));
		zassert_ok(sys_csrand_get(buffer + 1U, sizes[i]), "secure request %zu", sizes[i]);
		zassert_equal(buffer[0], 0xa5);
		for (size_t j = 1U + sizes[i]; j < sizeof(buffer); j++) {
			zassert_equal(buffer[j], 0xa5, "request %zu overwrote byte %zu", sizes[i],
				      j);
		}
	}
}

ZTEST(rp2350_trng, test_secure_successive_blocks)
{
	uint8_t first[32], second[32];
	zassert_ok(sys_csrand_get(first, sizeof(first)));
	zassert_ok(sys_csrand_get(second, sizeof(second)));
	zassert_not_equal(memcmp(first, second, sizeof(first)), 0, "secure blocks repeat");
}

ZTEST(rp2350_trng, test_timeout_and_recovery)
{
	const struct device *const dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
	uint8_t nonce[4];
	zassert_ok(sys_csrand_get(nonce, sizeof(nonce))); /* Consume any prefetched block. */

	/* One sample now needs 2^32 RNG clocks, beyond the driver's bounded wait.
	 * Leave health checks enabled and restore the sampling interval before assertions. */
	const uint32_t sampling = trng_hw->sample_cnt1;
	trng_hw->sample_cnt1 = UINT32_MAX;
	memset(nonce, 0xa5, sizeof(nonce));
	int64_t start = k_uptime_get();
	const int entropy_rc = entropy_get_entropy(dev, nonce, sizeof(nonce));
	const int64_t entropy_ms = k_uptime_get() - start;
	start = k_uptime_get();
	const int secure_rc = sys_csrand_get(nonce, sizeof(nonce));
	const int64_t secure_ms = k_uptime_get() - start;
	trng_hw->sample_cnt1 = sampling;

	printk("TRNG timeout: entropy=%d (%lld ms), secure=%d (%lld ms)\n", entropy_rc,
	       (long long)entropy_ms, secure_rc, (long long)secure_ms);
	zassert_equal(entropy_rc, -ETIMEDOUT);
	zassert_equal(secure_rc, -EIO);
	zassert_true(entropy_ms >= 1000 && entropy_ms < 5000);
	zassert_true(secure_ms >= 1000 && secure_ms < 5000);
	for (size_t i = 0U; i < sizeof(nonce); i++) {
		zassert_equal(nonce[i], 0xa5, "failed request substituted bytes");
	}
	zassert_ok(sys_csrand_get(nonce, sizeof(nonce)), "TRNG did not recover");
}

ZTEST_SUITE(rp2350_trng, NULL, NULL, NULL, NULL, NULL);
