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
 *   - Generation time is unpredictable and occasionally exceeds 100x the average (12.12.4), so the
 *     wait per block is bounded by a timeout and a block that fails a check is discarded and a new
 *     one started. Probe starts the first block (there is no scheduler yet, so it cannot wait).
 *
 * Two hard rules: no interrupts disabled, no spinlocks; and bounded waits (timeout = error).
 * **No software fallback**: a failed read or a failed entropy check is a failure, reported
 * upward (CSPRNG / PSA / boot self-test). Random numbers that merely look fine are not acceptable.
 *
 * A request reads fresh blocks straight into the caller's buffer and stops the source when it is
 * done. An earlier version kept a pool of ready blocks and handed out of it; the second read then
 * came back as zeros while the first looked fine, which is how a device ended up with no id at all.
 * Reading block by block leaves no bookkeeping that can drift out of step with the hardware.
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

/* One block, copied out of the EHR: reading the last word starts the next block */
static void trng_read_block(const struct trng_config *cfg, uint8_t *out)
{
	const trng_hw_t *trng = cfg->regs;

	for (uint8_t i = 0U; i < TRNG_BLOCK_WORDS; i++) {
		sys_put_le32(trng->ehr_data[i], &out[i * sizeof(uint32_t)]);
	}
}

/*
 * One block into out, retrying the machine state after a block that failed a check. Returns 0 or
 * the errno of the last attempt; a slow block keeps its place in the machine and is waited on
 * again, so an attempt only ends when the block is discarded or ready.
 */
static int trng_block(const struct trng_config *cfg, struct trng_data *data, uint8_t *out)
{
	int rc = -EIO;

	for (uint8_t attempt = 0U; attempt < TRNG_BLOCK_ATTEMPTS; attempt++) {
		trng_start(cfg, data);
		rc = trng_wait_block(cfg, k_uptime_get_32());
		if (rc == 0) {
			trng_read_block(cfg, out);
			return 0;
		}
		if (rc == -ETIMEDOUT) {
			continue;
		}

		trng_recover(cfg, data);
	}

	return rc;
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
		/*
		 * The EHR hands out whole blocks and reading it clears the block, so a request that is
		 * not a multiple of the block size takes a fresh block for its tail and keeps the first
		 * bytes of it. Callers ask for 32 bytes, which is the same cost either way.
		 */
		uint8_t block[TRNG_BLOCK_BYTES];
		const uint16_t take = MIN((uint16_t)TRNG_BLOCK_BYTES, (uint16_t)(len - off));

		rc = trng_block(cfg, data, block);
		if (rc != 0) {
			LOG_ERR("TRNG block read failed (%d)", rc);
			break;
		}

		memcpy(&buf[off], block, take);
		off += take;
	}

	trng_stop(cfg, data);
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
