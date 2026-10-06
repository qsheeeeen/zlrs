/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/ztest.h>
#include <string.h>

ZTEST(rp2350_trng, test_successive_blocks)
{
	const struct device *const dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
	uint8_t first[32];
	uint8_t second[32];

	zassert_true(device_is_ready(dev), "entropy driver unavailable");
	zassert_ok(entropy_get_entropy(dev, first, sizeof(first)));
	zassert_ok(entropy_get_entropy(dev, second, sizeof(second)));
	zassert_not_equal(memcmp(first, second, sizeof(first)), 0,
			  "successive TRNG blocks repeat");
}

ZTEST_SUITE(rp2350_trng, NULL, NULL, NULL, NULL, NULL);
