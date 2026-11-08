/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/random/random.h>
#include <zephyr/ztest.h>
#include <zephyr/irq_offload.h>
#include <zephyr/init.h>
#include <errno.h>
#include <hardware/structs/trng.h>
#include <stdlib.h>
#include <string.h>

static const struct device *const entropy_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
static uint32_t sampling_baseline;

static struct {
	uint8_t bytes[48];
	int count;
	bool ready;
	bool source_stopped;
	uint32_t read_us;
} boot_pool;

static int capture_boot_pool(void)
{
	/* Run immediately after driver initialization, before any test requests entropy.
	 * This read must consume two already-validated blocks without waiting or warming up.
	 */
	boot_pool.ready = device_is_ready(entropy_dev);
	boot_pool.source_stopped = trng_hw->rnd_source_enable == 0U;
	const uint64_t before = k_cycle_get_64();
	boot_pool.count =
		entropy_get_entropy_isr(entropy_dev, boot_pool.bytes, sizeof(boot_pool.bytes), 0U);
	boot_pool.read_us = k_cyc_to_us_floor64(k_cycle_get_64() - before);
	return 0;
}

SYS_INIT(capture_boot_pool, POST_KERNEL, UTIL_INC(CONFIG_ENTROPY_INIT_PRIORITY));

ZTEST(rp2350_trng, test_boot_pool_is_full)
{
	printk("TRNG_BOOT ready=%u stopped=%u bytes=%d read_us=%u\n", boot_pool.ready,
	       boot_pool.source_stopped, boot_pool.count, boot_pool.read_us);
	zassert_true(boot_pool.ready, "entropy device not ready after initialization");
	zassert_true(boot_pool.source_stopped, "initial pool was still collecting");
	zassert_equal(boot_pool.count, sizeof(boot_pool.bytes), "startup did not fill the pool");
	zassert_true(boot_pool.read_us < 1000U, "first read waited for generation");
	zassert_not_equal(memcmp(boot_pool.bytes, boot_pool.bytes + 24U, 24U), 0,
			  "startup reissued the first block");
}

static uint32_t stall_source(void)
{
	/* Wait until a completed refill has stopped the source, then drain every
	 * cached byte. Emptying the pool starts a new generation with the fault.
	 */
	const int64_t deadline = k_uptime_get() + 4000;
	while (trng_hw->rnd_source_enable != 0U && k_uptime_get() < deadline) {
		k_msleep(1);
	}
	zassert_equal(trng_hw->rnd_source_enable, 0U, "TRNG did not stop with a full pool");
	const uint32_t sampling = trng_hw->sample_cnt1;
	trng_hw->sample_cnt1 = UINT32_MAX;
	uint8_t discard[64];
	while (entropy_get_entropy_isr(entropy_dev, discard, sizeof(discard), 0U) > 0) {
	}
	return sampling;
}

static void nonblocking_read(uint8_t *out, uint16_t len)
{
	const int64_t deadline = k_uptime_get() + 4000;
	for (uint16_t off = 0U; off < len;) {
		const int rc = entropy_get_entropy_isr(entropy_dev, out + off, len - off, 0U);
		zassert_true(rc >= 0 && rc <= len - off, "bad entropy count %d", rc);
		if (rc == 0) {
			zassert_true(k_uptime_get() < deadline, "pool failed to refill");
			k_msleep(1);
		} else {
			off += rc;
		}
	}
}

static void assert_recovered(void)
{
	/* Restoring sampling need not cancel a generation already in flight.
	 * Its existing deadline can expire before the next healthy block arrives.
	 */
	uint32_t nonce;
	int rc = sys_csrand_get(&nonce, sizeof(nonce));
	if (rc != 0) {
		rc = sys_csrand_get(&nonce, sizeof(nonce));
	}
	zassert_ok(rc, "TRNG did not recover");
}

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

static int compare_duration(const void *lhs, const void *rhs)
{
	const uint32_t a = *(const uint32_t *)lhs;
	const uint32_t b = *(const uint32_t *)rhs;
	return (a > b) - (a < b);
}

ZTEST(rp2350_trng, test_link_nonce_latency)
{
	/* Match rf.c's four-byte nonblocking read, without timing printk/sorting/retries.
	 * Keep samples off the test thread's stack. These measure isolated API latency;
	 * they do not measure complete RF handshakes or concurrent DATA goodput.
	 */
	static uint32_t duration_us[1024];
	uint64_t total_us = 0U;
	unsigned int misses = 0U;
	uint32_t empty_max_us = 0U;
	for (size_t i = 0U; i < ARRAY_SIZE(duration_us); i++) {
		const int64_t deadline = k_uptime_get() + 4000;
		for (;;) {
			uint32_t nonce = 0xa5a5a5a5U;
			const uint64_t before = k_cycle_get_64();
			const int rc = entropy_get_entropy_isr(entropy_dev, (uint8_t *)&nonce,
							       sizeof(nonce), 0U);
			const uint32_t elapsed = k_cyc_to_us_floor64(k_cycle_get_64() - before);
			zassert_true(rc >= 0 && rc <= sizeof(nonce));
			if (rc == sizeof(nonce)) {
				duration_us[i] = elapsed;
				total_us += elapsed;
				break;
			}
			misses++;
			empty_max_us = MAX(empty_max_us, elapsed);
			if (rc == 0) {
				zassert_equal(nonce, 0xa5a5a5a5U, "empty pool changed output");
			}
			zassert_true(k_uptime_get() < deadline);
			k_msleep(1);
		}
	}
	qsort(duration_us, ARRAY_SIZE(duration_us), sizeof(duration_us[0]), compare_duration);
	printk("TRNG_NONCE requests=1024 bytes=4 mode=nonblocking min_us=%u mean_us=%u "
	       "p50_us=%u p95_us=%u p99_us=%u max_us=%u misses=%u empty_max_us=%u\n",
	       duration_us[0], (uint32_t)(total_us / ARRAY_SIZE(duration_us)), duration_us[511],
	       duration_us[972], duration_us[1013], duration_us[1023], misses, empty_max_us);
	zassert_true(duration_us[1023] < 1000U && empty_max_us < 1000U,
		     "nonblocking call waited for hardware");
}

ZTEST(rp2350_trng, test_repeated_secure_reads)
{
	const uint32_t bypass_mask = TRNG_TRNG_DEBUG_CONTROL_AUTO_CORRELATE_BYPASS_BITS |
				     TRNG_TRNG_DEBUG_CONTROL_TRNG_CRNGT_BYPASS_BITS |
				     TRNG_TRNG_DEBUG_CONTROL_VNC_BYPASS_BITS;
	zassert_equal(trng_hw->trng_debug_control & bypass_mask, 0U);
	uint8_t block[32], previous[32];
	unsigned int failures = 0, repeats = 0;
	uint32_t max_ms = 0;
	bool have_previous = false;
	const int64_t started = k_uptime_get();
	for (unsigned int request = 0; request < 256; request++) {
		const int64_t before = k_uptime_get();
		const int rc = sys_csrand_get(block, sizeof(block));
		max_ms = MAX(max_ms, (uint32_t)(k_uptime_get() - before));
		if (rc != 0) {
			failures++;
			continue;
		}
		if (have_previous && memcmp(previous, block, sizeof(block)) == 0) {
			repeats++;
		}
		memcpy(previous, block, sizeof(previous));
		have_previous = true;
	}
	printk("TRNG_STRESS requests=256 bytes=32 failures=%u repeats=%u max_ms=%u "
	       "duration_ms=%lld\n",
	       failures, repeats, max_ms, (long long)(k_uptime_get() - started));
	zassert_equal(failures, 0U, "healthy source failed during repeated secure reads");
	zassert_equal(repeats, 0U);
	zassert_equal(trng_hw->trng_debug_control & bypass_mask, 0U);
}

ZTEST(rp2350_trng, test_timeout_and_recovery)
{
	const struct device *const dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
	uint8_t nonce[4];
	/* One sample now needs 2^32 RNG clocks, beyond the driver's bounded wait.
	 * Leave health checks enabled and restore the sampling interval before assertions. */
	const uint32_t sampling = stall_source();
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
	assert_recovered();
}

ZTEST(rp2350_trng, test_nonblocking_sizes_and_wrap)
{
	static const uint16_t sizes[] = {0U, 1U, 3U, 4U, 23U, 24U, 25U, 47U, 48U, 73U};
	uint8_t guarded[75];
	for (unsigned int round = 0U; round < 16U; round++) {
		for (size_t i = 0U; i < ARRAY_SIZE(sizes); i++) {
			memset(guarded, 0xa5, sizeof(guarded));
			nonblocking_read(guarded + 1U, sizes[i]);
			zassert_equal(guarded[0], 0xa5);
			for (size_t j = sizes[i] + 1U; j < sizeof(guarded); j++) {
				zassert_equal(guarded[j], 0xa5, "request %u overflow at %zu",
					      sizes[i], j);
			}
		}
	}
	/* Busy-wait is deliberately unsupported, even if a pool is ready. */
	memset(guarded, 0xa5, sizeof(guarded));
	zassert_equal(entropy_get_entropy_isr(entropy_dev, guarded, 4U, ENTROPY_BUSYWAIT),
		      -ENOTSUP);
	zassert_equal(guarded[0], 0xa5);
}

struct irq_read {
	uint8_t bytes[4];
	int rc;
	int blocking_rc;
};

static void read_in_irq(const void *arg)
{
	struct irq_read *result = (struct irq_read *)arg;
	result->rc = entropy_get_entropy_isr(entropy_dev, result->bytes, sizeof(result->bytes), 0U);
	result->blocking_rc =
		entropy_get_entropy(entropy_dev, result->bytes, sizeof(result->bytes));
}

ZTEST(rp2350_trng, test_isr_reads_ready_and_empty)
{
	uint8_t ready[4];
	zassert_ok(entropy_get_entropy(entropy_dev, ready, sizeof(ready)));
	/* Let the source finish refilling, so this ISR consumes a ready nonce. */
	const int64_t deadline = k_uptime_get() + 4000;
	while (trng_hw->rnd_source_enable != 0U && k_uptime_get() < deadline) {
		k_msleep(1);
	}
	struct irq_read result = {0};
	irq_offload(read_in_irq, &result);
	zassert_equal(result.rc, sizeof(result.bytes));
	zassert_equal(result.blocking_rc, -ENOTSUP);
	const uint32_t sampling = stall_source();
	memset(result.bytes, 0xa5, sizeof(result.bytes));
	irq_offload(read_in_irq, &result);
	trng_hw->sample_cnt1 = sampling;
	zassert_equal(result.rc, 0);
	zassert_equal(result.blocking_rc, -ENOTSUP);
	for (size_t i = 0U; i < sizeof(result.bytes); i++) {
		zassert_equal(result.bytes[i], 0xa5);
	}
	assert_recovered();
}

static int64_t probe_started, probe_elapsed;
static K_SEM_DEFINE(probe_done, 0, 1);

static void workqueue_probe(struct k_work *work)
{
	ARG_UNUSED(work);
	probe_elapsed = k_uptime_get() - probe_started;
	k_sem_give(&probe_done);
}

K_WORK_DELAYABLE_DEFINE(probe_work, workqueue_probe);

ZTEST(rp2350_trng, test_nonblocking_during_hardware_stall)
{
	const uint32_t sampling = stall_source();
	probe_started = k_uptime_get();
	probe_elapsed = -1;
	k_sem_reset(&probe_done);
	k_work_schedule(&probe_work, K_MSEC(50));
	uint32_t max_us = 0U, samples = 0U;
	bool changed = false;
	int unexpected = 0;
	while (k_uptime_get() - probe_started < 3300) {
		uint32_t nonce = 0xa5a5a5a5U;
		const uint64_t before = k_cycle_get_64();
		const int rc =
			entropy_get_entropy_isr(entropy_dev, (uint8_t *)&nonce, sizeof(nonce), 0U);
		max_us = MAX(max_us, (uint32_t)k_cyc_to_us_floor64(k_cycle_get_64() - before));
		changed |= nonce != 0xa5a5a5a5U;
		unexpected += rc != 0;
		samples++;
		k_msleep(1);
	}
	trng_hw->sample_cnt1 = sampling;
	const bool probe_completed = k_sem_take(&probe_done, K_NO_WAIT) == 0;
	printk("TRNG_STALLED samples=%u duration_ms=3300 max_us=%u workqueue_probe_ms=%lld\n",
	       samples, max_us, (long long)probe_elapsed);
	zassert_equal(unexpected, 0);
	zassert_false(changed, "stalled source returned substituted bytes");
	zassert_true(max_us < 1000U, "nonblocking call waited on hardware");
	zassert_true(probe_completed && probe_elapsed >= 50 && probe_elapsed < 100,
		     "TRNG work blocked the system workqueue");
	assert_recovered();
}

#define READER_SAMPLES 128U
static uint8_t concurrent_samples[3][READER_SAMPLES][24];
static int reader_errors[2];
static struct k_thread readers[2];
static K_THREAD_STACK_ARRAY_DEFINE(reader_stacks, 2, 1536);
static K_SEM_DEFINE(readers_done, 0, 2);

static void blocking_reader(void *arg, void *unused1, void *unused2)
{
	const size_t index = (size_t)(uintptr_t)arg;
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	for (size_t i = 0U; i < READER_SAMPLES; i++) {
		const int rc = entropy_get_entropy(entropy_dev, concurrent_samples[index][i], 24U);
		if (rc != 0) {
			reader_errors[index] = rc;
			break;
		}
	}
	k_sem_give(&readers_done);
}

ZTEST(rp2350_trng, test_concurrent_consumers)
{
	memset(reader_errors, 0, sizeof(reader_errors));
	for (size_t i = 0U; i < ARRAY_SIZE(readers); i++) {
		k_thread_create(&readers[i], reader_stacks[i],
				K_THREAD_STACK_SIZEOF(reader_stacks[i]), blocking_reader,
				(void *)(uintptr_t)i, NULL, NULL, 1, 0, K_NO_WAIT);
	}
	for (size_t i = 0U; i < READER_SAMPLES; i++) {
		nonblocking_read(concurrent_samples[2][i], 24U);
	}
	for (size_t i = 0U; i < ARRAY_SIZE(readers); i++) {
		zassert_ok(k_sem_take(&readers_done, K_SECONDS(5)));
		zassert_ok(k_thread_join(&readers[i], K_SECONDS(1)));
		zassert_ok(reader_errors[i], "reader %zu failed", i);
	}
	/* Full 192-bit samples make stale/reissued cache contents detectable without
	 * imposing a uniqueness requirement on legitimate four-byte random nonces.
	 */
	const uint8_t *samples = &concurrent_samples[0][0][0];
	for (size_t i = 0U; i < 3U * READER_SAMPLES; i++) {
		for (size_t j = 0U; j < i; j++) {
			zassert_not_equal(memcmp(samples + i * 24U, samples + j * 24U, 24U), 0,
					  "pool reissued sample %zu as %zu", j, i);
		}
	}
}

static void *trng_setup(void)
{
	sampling_baseline = trng_hw->sample_cnt1;
	return NULL;
}

static void trng_after(void *unused)
{
	ARG_UNUSED(unused);
	trng_hw->sample_cnt1 = sampling_baseline;
}

ZTEST_SUITE(rp2350_trng, NULL, trng_setup, NULL, trng_after, NULL);
