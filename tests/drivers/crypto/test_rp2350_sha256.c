/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/crypto/crypto.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <stdalign.h>
#include <stdint.h>
#include <string.h>

#include "test_support.h"
#include "sha256_vectors.h"

#define SHA256_LEN 32U
#define HASH_FLAGS (CAP_SEPARATE_IO_BUFS | CAP_SYNC_OPS)

static const struct device *const hash_dev = DEVICE_DT_GET(DT_NODELABEL(sha256));
static struct hash_ctx session;
static bool session_open;
static alignas(uint32_t) uint8_t data[511];

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_true(device_is_ready(hash_dev), "SHA-256 driver unavailable");
	for (size_t i = 0U; i < sizeof(data); i++) {
		data[i] = (uint8_t)i;
	}

	session = (struct hash_ctx){.flags = HASH_FLAGS};
}

static void after(void *fixture)
{
	ARG_UNUSED(fixture);
	if (session_open) {
		const int rc = hash_free_session(hash_dev, &session);

		session_open = false;
		zassert_ok(rc, "release hash session");
	}
}

static void begin(void)
{
	zassert_ok(hash_begin_session(hash_dev, &session, CRYPTO_HASH_ALGO_SHA256));
	session_open = true;
}

ZTEST(rp2350_sha256, test_sha_multipart_and_session_reuse)
{
	uint8_t out[SHA256_LEN];
	struct hash_pkt pkt = {.out_buf = out};
	unsigned int count = 0U;

	begin();
	for (size_t v = 0U; v < ARRAY_SIZE(sha_vectors); v++) {
		const size_t len = sha_vectors[v].len;
		const uint8_t *input = len == 3U ? (const uint8_t *)"abc" : data;
		const size_t chunks[] = {1U, 3U, 64U};

		/* One-shot and multiple chunks reuse the same session for fresh digests. */
		pkt.in_buf = input;
		pkt.in_len = len;
		zassert_ok(hash_compute(&session, &pkt));
		zlrs_test_check_hex(out, sizeof(out), sha_vectors[v].want);
		count++;
		for (size_t c = 0U; c < ARRAY_SIZE(chunks); c++) {
			for (size_t offset = 0U; offset < len; offset += chunks[c]) {
				pkt.in_buf = input + offset;
				pkt.in_len = MIN(chunks[c], len - offset);
				zassert_ok(hash_update(&session, &pkt));
			}
			pkt.in_buf = NULL;
			pkt.in_len = 0U;
			zassert_ok(hash_compute(&session, &pkt));
			zlrs_test_check_hex(out, sizeof(out), sha_vectors[v].want);
			count++;
		}
	}
	printk("SHA-256: %u one-shot/multipart/reused-session vectors passed\n", count);
}

ZTEST(rp2350_sha256, test_session_errors_and_abandon)
{
	struct hash_ctx other = {.flags = HASH_FLAGS};
	struct hash_ctx unsupported = {.flags = CAP_SEPARATE_IO_BUFS | CAP_ASYNC_OPS};
	uint8_t out[SHA256_LEN];
	struct hash_pkt pkt = {.in_buf = data, .in_len = 3U, .out_buf = out};

	zassert_equal(hash_begin_session(hash_dev, &other, CRYPTO_HASH_ALGO_SHA512), -ENOTSUP);
	zassert_equal(hash_begin_session(hash_dev, &unsupported, CRYPTO_HASH_ALGO_SHA256),
		      -ENOTSUP);
	begin();
	zassert_equal(hash_begin_session(hash_dev, &other, CRYPTO_HASH_ALGO_SHA256), -EBUSY);
	zassert_equal(hash_free_session(hash_dev, &other), -EINVAL);
	pkt.in_buf = NULL;
	zassert_equal(hash_update(&session, &pkt), -EINVAL);
	pkt.in_buf = data;
	pkt.out_buf = NULL;
	zassert_equal(hash_compute(&session, &pkt), -EINVAL);
	pkt.out_buf = out;
	pkt.in_len = SIZE_MAX;
	zassert_equal(hash_update(&session, &pkt), -EOVERFLOW);
	pkt.in_len = 3U;
	zassert_ok(hash_update(&session, &pkt));
	zassert_ok(hash_free_session(hash_dev, &session));
	session_open = false;
	zassert_equal(hash_free_session(hash_dev, &session), -EINVAL);
	begin();
	pkt.in_buf = (const uint8_t *)"abc";
	zassert_ok(hash_compute(&session, &pkt));
	zlrs_test_check_hex(out, sizeof(out), sha_vectors[1].want);
}

ZTEST_SUITE(rp2350_sha256, NULL, NULL, before, after, NULL);
