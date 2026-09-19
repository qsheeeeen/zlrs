/*
 * RP2350 hardware TRNG entropy source (zlrs driver, see doc/13-roadmap.md).
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
 *     100 samples it names: 10 blocks in a row came out clean, at a proportional time cost.
 *   - A block is 6 EHR_DATA words; reading the last one clears the result registers and starts
 *     the next block (12.12.3).
 *   - Generation time is unpredictable and occasionally exceeds 100x the average, so 12.12.4
 *     suggests a small pool and kicking off the next generation whenever a slot frees up. Probe
 *     starts the first block (no scheduler yet, so it cannot wait), so reads never start from zero.
 *
 * Two hard rules: no interrupts disabled, no spinlocks; and bounded waits (timeout = error).
 * **No software fallback**: a failed read or a failed entropy check is a failure, reported
 * upward (CSPRNG / PSA / boot self-test). Random numbers that merely look fine are not acceptable.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zlrs_rp2350_trng

#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <hardware/structs/trng.h>

LOG_MODULE_REGISTER(zlrs_trng, CONFIG_ZLRS_TRNG_LOG_LEVEL);

/* Chain length 0 (fastest ROSC) with 100 samples; 20-25 fails the VN check, see the header */
#define TRNG_ROSC_CHAIN   0U
#define TRNG_SAMPLE_TICKS 100U

/* one block = 192 bit = 6 EHR_DATA words */
#define TRNG_BLOCK_WORDS 6U
#define TRNG_BLOCK_BYTES (TRNG_BLOCK_WORDS * sizeof(uint32_t))

/*
 * The pool holds two blocks: one for the read in progress, one in reserve while the hardware
 * keeps generating: the "small pool" of datasheet 12.12.4.
 */
#define TRNG_POOL_BLOCKS 2U
#define TRNG_POOL_BYTES  (TRNG_POOL_BLOCKS * TRNG_BLOCK_BYTES)

/* Generation occasionally exceeds 100x the average (12.12.4), so the timeout is that order */
#define TRNG_BLOCK_TIMEOUT_MS 1000U
#define TRNG_POLL_INTERVAL_MS 1U

/* A block that failed a check is not data; starting a new one is the normal path, not a fallback */
#define TRNG_BLOCK_ATTEMPTS 3U

/* Bits software can clear in RNG_ISR; AUTOCORR_ERR needs a soft reset (12.12.5) */
#define TRNG_ISR_CLEARABLE                                                                         \
	(TRNG_RNG_ISR_EHR_VALID_BITS | TRNG_RNG_ISR_VN_ERR_BITS | TRNG_RNG_ISR_CRNGT_ERR_BITS)

/*
 * The HAL's trng_hw_t starts at RNG_IMR (0x100) while the DT reg gives the TRNG block base
 * (0x400f0000, datasheet 12.12.5).
 */
#define TRNG_CONFIG_INIT(inst)                                                                     \
	{                                                                                          \
		.regs = (trng_hw_t *)(uintptr_t)(DT_INST_REG_ADDR(inst) + TRNG_RNG_IMR_OFFSET)     \
	}

struct trng_config {
	trng_hw_t *regs;
};

struct trng_data {
	/* The pool: reads come from here, the hardware is only waited on when it runs dry */
	uint8_t pool[TRNG_POOL_BYTES];
	uint16_t pool_head; /* next byte to hand out */
	uint16_t pool_len;  /* bytes left after pool_head */
	/* RND_SRC_EN is set: a block is being generated or is ready to collect */
	bool running;
	/* Reads wait on hardware and the entropy API has to be thread safe */
	struct k_mutex lock;
};

/* Soft reset plus the 12.12.2 configuration, leaving the source disabled */
static void trng_reset(const struct trng_config *cfg, struct trng_data *data)
{
	trng_hw_t *const trng = cfg->regs;

	trng->rnd_source_enable = 0U;
	trng->trng_sw_reset = 1U;

	/* A soft reset needs a fixed delay (bootrom reads the same register twice) */
	(void)trng->trng_sw_reset;
	(void)trng->trng_sw_reset;

	trng->trng_config = TRNG_ROSC_CHAIN;
	trng->sample_cnt1 = TRNG_SAMPLE_TICKS;
	/* Poll instead of using interrupts. The reset value already masks everything; writing says so */
	trng->rng_imr = TRNG_RNG_IMR_EHR_VALID_INT_MASK_BITS | TRNG_RNG_IMR_VN_ERR_INT_MASK_BITS |
			TRNG_RNG_IMR_CRNGT_ERR_INT_MASK_BITS |
			TRNG_RNG_IMR_AUTOCORR_ERR_INT_MASK_BITS;
	trng->rng_icr = TRNG_ISR_CLEARABLE;
	data->running = false;
}

/* Start a block. Leave it alone if one is running: rewriting RND_SRC_EN would abort it */
static void trng_start(const struct trng_config *cfg, struct trng_data *data)
{
	trng_hw_t *const trng = cfg->regs;

	if (data->running) {
		return;
	}

	/* RST_BITS_COUNTER requires the source to be disabled (12.12.5) */
	trng->rng_icr = TRNG_ISR_CLEARABLE;
	trng->rst_bits_counter = 1U;
	trng->rnd_source_enable = 1U;
	data->running = true;
}

/* Disable the source when idle to save power (12.12.3) and drop the half-collected block */
static void trng_stop(const struct trng_config *cfg, struct trng_data *data)
{
	trng_hw_t *const trng = cfg->regs;

	if (!data->running) {
		return;
	}

	trng->rnd_source_enable = 0U;
	trng->rst_bits_counter = 1U;
	trng->rng_icr = TRNG_ISR_CLEARABLE;
	data->running = false;
}

/*
 * Starting over after a bad block: Von Neumann and CRNGT failures just need the state cleared,
 * but repeated autocorrelation failures stop the RNG entirely and only a soft reset clears that.
 */
static void trng_recover(const struct trng_config *cfg, struct trng_data *data)
{
	LOG_DBG("discarding failed block, isr 0x%x", cfg->regs->rng_isr);

	if ((cfg->regs->rng_isr & TRNG_RNG_ISR_AUTOCORR_ERR_BITS) != 0U) {
		trng_reset(cfg, data);
	} else {
		trng_stop(cfg, data);
	}

	trng_start(cfg, data);
}

/*
 * Wait for a block: 0 = the EHR holds readable data, anything else is an errno. start is when
 * this wait began; retries share the deadline, so a block gets TRNG_BLOCK_TIMEOUT_MS in total.
 *
 * Trust EHR_VALID, not trng_busy: busy is not up yet in the first cycles of a generation, so
 * going by busy reads all-zero result registers.
 *
 * Check the error bits before EHR_VALID: a failed block sets EHR_VALID too (measured), and
 * 12.12.3 says a block that failed a check is not data, so drop it and start over.
 */
static int trng_wait_block(const struct trng_config *cfg, uint32_t start)
{
	const trng_hw_t *trng = cfg->regs;

	for (;;) {
		const uint32_t isr = trng->rng_isr;

		if ((isr & TRNG_RNG_ISR_AUTOCORR_ERR_BITS) != 0U) {
			return -EIO;
		}
		if ((isr & (TRNG_RNG_ISR_VN_ERR_BITS | TRNG_RNG_ISR_CRNGT_ERR_BITS)) != 0U) {
			return -EAGAIN;
		}
		if ((isr & TRNG_RNG_ISR_EHR_VALID_BITS) != 0U) {
			return 0;
		}
		if ((k_uptime_get_32() - start) >= TRNG_BLOCK_TIMEOUT_MS) {
			return -ETIMEDOUT;
		}

		k_msleep(TRNG_POLL_INTERVAL_MS);
	}
}

/* Move the leftover bytes to the front so the next block fits in whole */
static void trng_pool_compact(struct trng_data *data)
{
	if (data->pool_head == 0U) {
		return;
	}

	if (data->pool_len > 0U) {
		memmove(data->pool, &data->pool[data->pool_head], data->pool_len);
	}

	data->pool_head = 0U;
}

/* How many blocks the pool can still take */
static size_t trng_pool_room(struct trng_data *data)
{
	trng_pool_compact(data);

	return (sizeof(data->pool) - data->pool_len) / TRNG_BLOCK_BYTES;
}

/* Read all 6 words into the pool: the last read clears the registers and starts the next block */
static void trng_pool_put_block(const struct trng_config *cfg, struct trng_data *data)
{
	const trng_hw_t *trng = cfg->regs;
	const size_t at = (size_t)data->pool_head + data->pool_len;

	__ASSERT_NO_MSG((sizeof(data->pool) - at) >= TRNG_BLOCK_BYTES);

	for (uint8_t i = 0U; i < TRNG_BLOCK_WORDS; i++) {
		sys_put_le32(trng->ehr_data[i], &data->pool[at + (i * sizeof(uint32_t))]);
	}

	data->pool_len += TRNG_BLOCK_BYTES;
}

/* Wait for one block to land in the pool. 0 = the pool grew by one */
static int trng_pool_refill(const struct trng_config *cfg, struct trng_data *data)
{
	const uint32_t start = k_uptime_get_32();

	for (uint8_t attempt = 0U; attempt < TRNG_BLOCK_ATTEMPTS; attempt++) {
		int rc;

		if (trng_pool_room(data) == 0U) {
			return -ENOSPC;
		}

		trng_start(cfg, data);

		rc = trng_wait_block(cfg, start);
		if (rc == 0) {
			trng_pool_put_block(cfg, data);
			return 0;
		}
		if (rc == -ETIMEDOUT) {
			/* It may simply be slow (12.12.4): leave the machine state alone, the next read keeps waiting */
			return rc;
		}

		trng_recover(cfg, data);
	}

	return -EIO;
}

/* Pool full: disable the source. Room left: collect the finished block and keep generating */
static void trng_pool_top_up(const struct trng_config *cfg, struct trng_data *data)
{
	for (;;) {
		uint32_t isr;

		if (trng_pool_room(data) == 0U) {
			trng_stop(cfg, data);
			return;
		}

		isr = cfg->regs->rng_isr;
		if ((isr & TRNG_RNG_ISR_EHR_VALID_BITS) != 0U) {
			trng_pool_put_block(cfg, data);
			continue;
		}

		/* Not ready yet: restart only after an idle stop or a bad block, otherwise let it run */
		if (!data->running ||
		    (isr & (TRNG_RNG_ISR_VN_ERR_BITS | TRNG_RNG_ISR_CRNGT_ERR_BITS |
			    TRNG_RNG_ISR_AUTOCORR_ERR_BITS)) != 0U) {
			trng_recover(cfg, data);
		}

		return;
	}
}

static int trng_get_entropy(const struct device *dev, uint8_t *buf, uint16_t len)
{
	const struct trng_config *cfg = dev->config;
	struct trng_data *data = dev->data;
	uint16_t off = 0U;
	int rc = 0;

	if (len == 0U) {
		return 0;
	}

	/* Reads wait on hardware (hundreds of ms), so they stay on the calling thread */
	if (k_is_in_isr()) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	while (off < len) {
		size_t n;

		if (data->pool_len == 0U) {
			rc = trng_pool_refill(cfg, data);
			if (rc != 0) {
				LOG_ERR("TRNG block read failed (%d)", rc);
				break;
			}
		}

		n = MIN((size_t)data->pool_len, (size_t)(len - off));
		memcpy(&buf[off], &data->pool[data->pool_head], n);
		data->pool_head += (uint16_t)n;
		data->pool_len -= (uint16_t)n;
		off += (uint16_t)n;
	}

	if (rc == 0) {
		trng_pool_top_up(cfg, data);
	}

	k_mutex_unlock(&data->lock);

	return rc;
}

static DEVICE_API(entropy, trng_api) = {
	.get_entropy = trng_get_entropy,
};

static int trng_init(const struct device *dev)
{
	const struct trng_config *cfg = dev->config;
	struct trng_data *data = dev->data;

	/*
	 * Register self-test: the driver reads 192 bit (6 EHR words), so first confirm this IP really
	 * has a 192-bit EHR. If it does not, this is not the TRNG we know and it must not feed entropy.
	 */
	if ((cfg->regs->rng_version & TRNG_RNG_VERSION_EHR_WIDTH_192_BITS) == 0U) {
		LOG_ERR("TRNG reports a 128-bit EHR (RNG_VERSION 0x%x)", cfg->regs->rng_version);
		return -ENOTSUP;
	}

	k_mutex_init(&data->lock);
	trng_reset(cfg, data);

	/*
	 * Pre-fetch the first block: enable the source and do not wait — probe runs at PRE_KERNEL_1,
	 * before the scheduler exists, so no waiting can happen here.
	 */
	trng_start(cfg, data);

	return 0;
}

#define TRNG_DEFINE(inst)                                                                          \
	static struct trng_data trng_data_##inst;                                                  \
	static const struct trng_config trng_config_##inst = TRNG_CONFIG_INIT(inst);               \
	DEVICE_DT_INST_DEFINE(inst, trng_init, NULL, &trng_data_##inst, &trng_config_##inst,       \
			      PRE_KERNEL_1, CONFIG_ENTROPY_INIT_PRIORITY, &trng_api);

DT_INST_FOREACH_STATUS_OKAY(TRNG_DEFINE)
