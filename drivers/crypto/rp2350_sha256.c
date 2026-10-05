/*
 * Copyright (c) 2025 TOKITA Hiroshi
 * Copyright (c) 2026 zlrs contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Based on Zephyr's crypto_rpi_pico_sha256 driver. Keep Pico SDK's input and
 * padding routines, support multipart hashing, and track the owning context.
 * The CPU feeds SHA words with interrupts enabled; no DMA channels are used.
 */

#define DT_DRV_COMPAT zlrs_rp2350_sha256

#include <zephyr/crypto/crypto.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <pico/bootrom/lock.h>
#include <pico/sha256.h>

#define SHA256_CAPS        (CAP_SEPARATE_IO_BUFS | CAP_SYNC_OPS)
#define SHA256_WORDS       8U
/* Pico SDK rounds total_data_size + 9 + 63 before appending the padding. */
#define SHA256_PAD_RESERVE 72U

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
	     "RP2350 has one SHA-256 accelerator");

struct sha256_data {
	pico_sha256_state_t state;
	struct k_mutex mutex;
	struct hash_ctx *owner;
};

static void sha256_start_hash(struct sha256_data *data)
{
	data->state.cache_used = 0U;
	data->state.cache.word = 0U;
	data->state.total_data_size = 0U;
	sha256_err_not_ready_clear();
	sha256_set_bswap(true);
	sha256_start();
}

static int sha256_hash_handler(struct hash_ctx *ctx, struct hash_pkt *pkt, bool finish)
{
	struct sha256_data *data = ctx->device->data;
	int rc = k_mutex_lock(&data->mutex, K_FOREVER);

	if (rc != 0) {
		return rc;
	}
	if (data->owner != ctx || ctx->drv_sessn_state != data || !data->state.locked) {
		rc = -EINVAL;
		goto out;
	}
	if ((pkt->in_len > 0U && pkt->in_buf == NULL) || (finish && pkt->out_buf == NULL)) {
		rc = -EINVAL;
		goto out;
	}
	const size_t processed = ctx->started ? data->state.total_data_size : 0U;

	if (processed > SIZE_MAX - SHA256_PAD_RESERVE ||
	    pkt->in_len > SIZE_MAX - SHA256_PAD_RESERVE - processed) {
		rc = -EOVERFLOW;
		goto out;
	}
	if (!ctx->started) {
		sha256_start_hash(data);
		ctx->started = true;
	}
	if (sha256_err_not_ready()) {
		rc = -EIO;
		goto out;
	}
	if (pkt->in_len > 0U) {
		pico_sha256_update(&data->state, pkt->in_buf, pkt->in_len);
	}
	if (sha256_err_not_ready()) {
		rc = -EIO;
		goto out;
	}
	if (finish) {
		pico_sha256_write_padding(&data->state);
		if (sha256_err_not_ready()) {
			rc = -EIO;
			goto out;
		}
		sha256_wait_valid_blocking();
		for (size_t i = 0U; i < SHA256_WORDS; i++) {
			sys_put_be32(sha256_hw->sum[i], pkt->out_buf + i * sizeof(uint32_t));
		}
		/* A subsequent compute/update starts a fresh digest in this session. */
		ctx->started = false;
	}
	rc = 0;
out:
	k_mutex_unlock(&data->mutex);
	return rc;
}

static int sha256_query_hw_caps(const struct device *dev)
{
	ARG_UNUSED(dev);
	return SHA256_CAPS;
}

static int sha256_begin_session(const struct device *dev, struct hash_ctx *ctx, enum hash_algo algo)
{
	struct sha256_data *data = dev->data;
	int rc;

	if (algo != CRYPTO_HASH_ALGO_SHA256 || (ctx->flags & ~SHA256_CAPS) != 0U) {
		return -ENOTSUP;
	}
	rc = k_mutex_lock(&data->mutex, K_FOREVER);
	if (rc != 0) {
		return rc;
	}
	if (data->owner != NULL || !bootrom_try_acquire_lock(BOOTROM_LOCK_SHA_256)) {
		rc = -EBUSY;
		goto out;
	}
	data->state = (pico_sha256_state_t){
		.endianness = SHA256_BIG_ENDIAN, .channel = -1, .locked = true};
	data->owner = ctx;
	ctx->device = dev;
	ctx->drv_sessn_state = data;
	ctx->hash_hndlr = sha256_hash_handler;
	ctx->started = false;
	rc = 0;
out:
	k_mutex_unlock(&data->mutex);
	return rc;
}

static int sha256_free_session(const struct device *dev, struct hash_ctx *ctx)
{
	struct sha256_data *data = dev->data;
	int rc = k_mutex_lock(&data->mutex, K_FOREVER);

	if (rc != 0) {
		return rc;
	}
	if (ctx == NULL || data->owner != ctx || ctx->drv_sessn_state != data) {
		rc = -EINVAL;
		goto out;
	}
	/* Abandon unfinished input too; the next operation resets the hardware. */
	memset(&data->state, 0, sizeof(data->state));
	data->owner = NULL;
	ctx->drv_sessn_state = NULL;
	ctx->hash_hndlr = NULL;
	ctx->started = false;
	bootrom_release_lock(BOOTROM_LOCK_SHA_256);
	rc = 0;
out:
	k_mutex_unlock(&data->mutex);
	return rc;
}

static int sha256_init(const struct device *dev)
{
	struct sha256_data *data = dev->data;

	k_mutex_init(&data->mutex);
	return 0;
}

static DEVICE_API(crypto, sha256_api) = {
	.query_hw_caps = sha256_query_hw_caps,
	.hash_begin_session = sha256_begin_session,
	.hash_free_session = sha256_free_session,
};

static struct sha256_data sha256_data;

DEVICE_DT_INST_DEFINE(0, sha256_init, NULL, &sha256_data, NULL, POST_KERNEL,
		      CONFIG_CRYPTO_INIT_PRIORITY, &sha256_api);
