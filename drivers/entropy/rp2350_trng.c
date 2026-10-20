/*
 * RP2350 hardware TRNG entropy source (see doc/08-security.md#entropy-contract).
 *
 * Why not Zephyr's entropy_rpi_pico: that path calls pico-sdk's get_rand_64(), which takes
 * a spinlock and disables interrupts, and only returns once 192 ROSC samples have filled the
 * EHR. USB interrupts and workqueues get no service meanwhile: with it on, USB never enumerates.
 *
 * Reads follow the ARM TRNG usage described in RP2350 datasheet 12.12. (bootrom and pico-sdk
 * take another route: set TRNG_DEBUG_CONTROL to bypass the entropy checks and decorrelator and
 * hand raw ROSC samples to SHA-256 / xoroshiro):
 *
 *   - All three entropy checks are on after reset, nothing to enable. TRNG_CONFIG picks the
 *     ROSC chain length and SAMPLE_CNT1 the sampling interval. Datasheet 12.12.2 suggests
 *     chain length 0/1 with 20-25 samples (about 2 ms per block), but that fails the Von
 *     Neumann check on this silicon (measured: runs of 32 zeroes or ones, sometimes autocorrelation
 *     giving up entirely), and 12.12.4 admits the official driver does not use it. We take the
 *     100 samples it names, with the hardware checks still enabled. Real boards can reject
 *     several consecutive blocks, so recovery is bounded by elapsed time rather than three draws.
 *   - A block is 6 EHR_DATA words; reading the last one clears the result registers and starts
 *     the next block (12.12.3).
 *   - Generation time is unpredictable and occasionally exceeds 100x the average (12.12.4), so the
 *     wait per block is bounded by a timeout and a block that fails a check is discarded and a new
 *     one started. Delayed work handles deadlines/retries without waiting on hardware.
 *
 * No path waits for hardware while holding a lock. A short lock serializes bounded pool
 * copies and register operations shared by readers, the IRQ and delayed work.
 * **No software fallback**: a failed read or a failed entropy check is a failure, reported
 * through Zephyr's cryptographic random API. Random numbers that merely look fine are
 * not acceptable.
 *
 * The IRQ publishes complete, validated blocks into a Zephyr ring buffer. Reads trigger
 * collection at the low watermark. An earlier cache returned cleared EHR data as zeros;
 * publication here happens only after EHR_VALID and the health checks, using a software copy.
 * The ordinary entropy API waits with a deadline; the ISR-safe API only consumes ready bytes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zlrs_rp2350_trng

#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <hardware/structs/trng.h>

LOG_MODULE_REGISTER(zlrs_trng, CONFIG_ZLRS_TRNG_LOG_LEVEL);

#define TRNG_ROSC_CHAIN       0U
#define TRNG_SAMPLE_TICKS     100U
#define TRNG_BLOCK_WORDS      6U
#define TRNG_BLOCK_BYTES      (TRNG_BLOCK_WORDS * sizeof(uint32_t))
#define TRNG_POOL_BYTES       (2U * TRNG_BLOCK_BYTES)
#define TRNG_BLOCK_TIMEOUT_MS 3000U
#define TRNG_RETRY_DELAY_MS   1U
#define TRNG_ISR_CLEARABLE                                                                         \
	(TRNG_RNG_ISR_EHR_VALID_BITS | TRNG_RNG_ISR_VN_ERR_BITS | TRNG_RNG_ISR_CRNGT_ERR_BITS)
#define TRNG_IRQ_MASK_ALL                                                                          \
	(TRNG_RNG_IMR_EHR_VALID_INT_MASK_BITS | TRNG_RNG_IMR_VN_ERR_INT_MASK_BITS |                \
	 TRNG_RNG_IMR_CRNGT_ERR_INT_MASK_BITS | TRNG_RNG_IMR_AUTOCORR_ERR_INT_MASK_BITS)

struct trng_config {
	trng_hw_t *regs;
	void (*irq_config)(const struct device *dev);
};

struct trng_data {
	struct k_spinlock lock;
	struct ring_buf ring;
	uint8_t pool[TRNG_POOL_BYTES];
	struct k_sem available;
	struct k_work_delayable service;
	const struct device *dev;
	k_timepoint_t deadline;
	bool running;
	bool collecting;
	bool retry;
};

/* Register operations below are bounded; callers hold data->lock after init. */
static void trng_reset(const struct trng_config *cfg, struct trng_data *data)
{
	trng_hw_t *trng = cfg->regs;
	trng->rnd_source_enable = 0U;
	trng->trng_sw_reset = 1U;
	/* Fixed reset delay used by the bootrom, without a software wait loop. */
	(void)trng->trng_sw_reset;
	(void)trng->trng_sw_reset;
	trng->trng_config = TRNG_ROSC_CHAIN;
	trng->sample_cnt1 = TRNG_SAMPLE_TICKS;
	trng->rng_imr = TRNG_IRQ_MASK_ALL;
	trng->rng_icr = TRNG_ISR_CLEARABLE;
	data->running = false;
}

static void trng_start(const struct trng_config *cfg, struct trng_data *data)
{
	if (!data->running) {
		cfg->regs->rng_icr = TRNG_ISR_CLEARABLE;
		cfg->regs->rst_bits_counter = 1U;
		cfg->regs->rnd_source_enable = 1U;
		data->running = true;
	}
}

static void trng_stop(const struct trng_config *cfg, struct trng_data *data)
{
	cfg->regs->rng_imr = TRNG_IRQ_MASK_ALL;
	cfg->regs->rnd_source_enable = 0U;
	cfg->regs->rst_bits_counter = 1U;
	cfg->regs->rng_icr = TRNG_ISR_CLEARABLE;
	data->running = false;
}

static void trng_begin(const struct trng_config *cfg, struct trng_data *data)
{
	if (data->collecting) {
		return;
	}
	data->collecting = true;
	data->retry = false;
	data->deadline = sys_timepoint_calc(K_MSEC(TRNG_BLOCK_TIMEOUT_MS));
	trng_start(cfg, data);
	cfg->regs->rng_imr = 0U;
	k_work_reschedule(&data->service, sys_timepoint_timeout(data->deadline));
}

/* Runs on the system workqueue. It never waits on hardware or on a consumer.
 * Retries keep the original deadline; an obsolete queued timeout rechecks state.
 */
static void trng_service(struct k_work *work)
{
	struct trng_data *data =
		CONTAINER_OF(k_work_delayable_from_work(work), struct trng_data, service);
	const struct trng_config *cfg = data->dev->config;
	bool timed_out = false;
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	if (data->collecting) {
		if (sys_timepoint_expired(data->deadline)) {
			trng_stop(cfg, data);
			data->collecting = false;
			data->retry = false;
			timed_out = true;
		} else {
			if (data->retry) {
				trng_start(cfg, data);
				data->retry = false;
				cfg->regs->rng_imr = 0U;
			}
			k_work_reschedule(&data->service, sys_timepoint_timeout(data->deadline));
		}
	}
	k_spin_unlock(&data->lock, key);
	if (timed_out) {
		LOG_ERR("TRNG block read failed (%d)", -ETIMEDOUT);
		k_sem_give(&data->available);
	}
}

static void trng_irq(const struct device *dev)
{
	const struct trng_config *cfg = dev->config;
	struct trng_data *data = dev->data;
	trng_hw_t *trng = cfg->regs;
	bool published = false;
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	trng->rng_imr = TRNG_IRQ_MASK_ALL;
	const uint32_t status = trng->rng_isr;
	const uint32_t errors = TRNG_RNG_ISR_AUTOCORR_ERR_BITS | TRNG_RNG_ISR_VN_ERR_BITS |
				TRNG_RNG_ISR_CRNGT_ERR_BITS;
	if (!data->collecting || sys_timepoint_expired(data->deadline)) {
		trng_stop(cfg, data);
		data->collecting = false;
		data->retry = false;
		(void)k_work_cancel_delayable(&data->service);
	} else if ((status & errors) != 0U ||
		   ((status & TRNG_RNG_ISR_EHR_VALID_BITS) != 0U &&
		    (trng->trng_valid & TRNG_TRNG_VALID_EHR_VALID_BITS) == 0U)) {
		/* Check errors before EHR_VALID: rejected blocks also set EHR_VALID.
		 * Autocorrelation failure stops the generator until a soft reset.
		 */
		if ((status & TRNG_RNG_ISR_AUTOCORR_ERR_BITS) != 0U) {
			trng_reset(cfg, data);
		} else {
			trng_stop(cfg, data);
		}
		data->retry = true;
		k_work_reschedule(&data->service, K_MSEC(TRNG_RETRY_DELAY_MS));
	} else if ((status & TRNG_RNG_ISR_EHR_VALID_BITS) != 0U) {
		uint8_t block[TRNG_BLOCK_BYTES];
		/* RNG_ISR is latched; reading EHR_DATA5 clears the data, not that
		 * interrupt flag. Acknowledge before DATA5 starts the next block,
		 * so a pending old IRQ cannot publish cleared registers a second time.
		 */
		trng->rng_icr = TRNG_RNG_ICR_EHR_VALID_BITS;
		/* Only this handler reads EHR, exactly once after validation. Reading
		 * the last word clears it and begins the next hardware block.
		 */
		for (uint8_t i = 0U; i < TRNG_BLOCK_WORDS; i++) {
			sys_put_le32(trng->ehr_data[i], block + i * sizeof(uint32_t));
		}
		const uint32_t copied = ring_buf_put(&data->ring, block, sizeof(block));
		__ASSERT_NO_MSG(copied == sizeof(block));
		ARG_UNUSED(copied);
		published = true;
		if (ring_buf_space_get(&data->ring) >= TRNG_BLOCK_BYTES) {
			data->deadline = sys_timepoint_calc(K_MSEC(TRNG_BLOCK_TIMEOUT_MS));
			k_work_reschedule(&data->service, sys_timepoint_timeout(data->deadline));
			trng->rng_imr = 0U;
		} else {
			trng_stop(cfg, data);
			data->collecting = false;
			(void)k_work_cancel_delayable(&data->service);
		}
	} else {
		trng->rng_imr = 0U;
	}
	k_spin_unlock(&data->lock, key);
	if (published) {
		k_sem_give(&data->available);
	}
}

/* ISR reads permit short results. Blocking readers reserve a complete chunk;
 * all ring operations and hardware state transitions share this short lock.
 */
static int trng_take(const struct device *dev, uint8_t *buf, uint16_t len, bool partial)
{
	const struct trng_config *cfg = dev->config;
	struct trng_data *data = dev->data;
	if (len == 0U) {
		return 0;
	}
	if (k_is_pre_kernel()) {
		return -ENOTSUP;
	}
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	const int copied = partial || ring_buf_size_get(&data->ring) >= len
				   ? (int)ring_buf_get(&data->ring, buf, len)
				   : 0;
	const uint32_t remaining = ring_buf_size_get(&data->ring);
	if (remaining <= TRNG_BLOCK_BYTES) {
		trng_begin(cfg, data);
	}
	k_spin_unlock(&data->lock, key);
	if (copied > 0 && remaining > 0U) {
		/* A ready pool can serve several blocking readers from one IRQ. */
		k_sem_give(&data->available);
	}
	return copied;
}

static int trng_get_entropy(const struct device *dev, uint8_t *buf, uint16_t len)
{
	struct trng_data *data = dev->data;
	if (len == 0U) {
		return 0;
	}
	if (k_is_in_isr() || k_is_pre_kernel()) {
		return -ENOTSUP;
	}
	struct k_poll_event event = K_POLL_EVENT_INITIALIZER(
		K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &data->available);
	for (uint16_t off = 0U; off < len;) {
		const uint16_t take = MIN((uint16_t)TRNG_BLOCK_BYTES, (uint16_t)(len - off));
		const k_timepoint_t deadline = sys_timepoint_calc(K_MSEC(TRNG_BLOCK_TIMEOUT_MS));
		while (trng_take(dev, buf + off, take, false) != take) {
			event.state = K_POLL_STATE_NOT_READY;
			if (k_poll(&event, 1, sys_timepoint_timeout(deadline)) != 0) {
				return -ETIMEDOUT;
			}
			(void)k_sem_take(&data->available, K_NO_WAIT);
		}
		off += take;
	}
	return 0;
}

static int trng_get_entropy_isr(const struct device *dev, uint8_t *buf, uint16_t len,
				uint32_t flags)
{
	if (flags != 0U) {
		return -ENOTSUP;
	}
	return trng_take(dev, buf, len, true);
}

static DEVICE_API(entropy, trng_api) = {
	.get_entropy = trng_get_entropy,
	.get_entropy_isr = trng_get_entropy_isr,
};

static int trng_init(const struct device *dev)
{
	const struct trng_config *cfg = dev->config;
	struct trng_data *data = dev->data;
	if ((cfg->regs->rng_version & TRNG_RNG_VERSION_EHR_WIDTH_192_BITS) == 0U) {
		LOG_ERR("TRNG reports a 128-bit EHR (RNG_VERSION 0x%x)", cfg->regs->rng_version);
		return -ENOTSUP;
	}
	data->dev = dev;
	k_sem_init(&data->available, 0U, 1U);
	ring_buf_init(&data->ring, sizeof(data->pool), data->pool);
	k_work_init_delayable(&data->service, trng_service);
	trng_reset(cfg, data);
	cfg->irq_config(dev);
	return 0;
}

#define TRNG_DEFINE(inst)                                                                          \
	static void trng_irq_config_##inst(const struct device *dev)                               \
	{                                                                                          \
		ARG_UNUSED(dev);                                                                   \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority), trng_irq,             \
			    DEVICE_DT_INST_GET(inst), 0);                                          \
		irq_enable(DT_INST_IRQN(inst));                                                    \
	}                                                                                          \
	static struct trng_data trng_data_##inst;                                                  \
	static const struct trng_config trng_config_##inst = {                                     \
		.regs = (trng_hw_t *)(uintptr_t)(DT_INST_REG_ADDR(inst) + TRNG_RNG_IMR_OFFSET),    \
		.irq_config = trng_irq_config_##inst,                                              \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, trng_init, NULL, &trng_data_##inst, &trng_config_##inst,       \
			      PRE_KERNEL_1, CONFIG_ENTROPY_INIT_PRIORITY, &trng_api);

DT_INST_FOREACH_STATUS_OKAY(TRNG_DEFINE)
