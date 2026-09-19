/*
 * RP2350 硬件 TRNG 熵源（zlrs 自带驱动，见 doc/13-roadmap.md）。
 *
 * 为什么不用 Zephyr 自带的 entropy_rpi_pico：它调 pico-sdk 的 get_rand_64()，
 * 取数时拿自旋锁并把中断关掉忙等 TRNG——USB 的中断与工作队列在这期间得不到
 * 服务，实测打开后 USB 枚举不出来（Windows 看得到 VID:PID，CDC 接口起不来），
 * 只能按 BOOTSEL 抢救。
 *
 * 这里的时序照 pico-sdk 的 TRNG 采样路径（rand.c）：熵源只开一次，等
 * `trng_busy` 清零后把 4 个 EHR 字一次读完——寄存器就是这么工作的，读最后一个
 * 字才会重新开始采样；按"每字等 valid"去读会连着拿到同一份数据。
 *
 * 两条底线：不关中断、不拿自旋锁；等待有界（超时返回 -ETIMEDOUT）。
 * **不做软件兜底**：硬件熵源读不出来就是失败，错误原样交给上层（CSPRNG / PSA /
 * 开机自检），由上层报错或停机——不能用"看起来能跑"的假熵糊过去。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <string.h>

#include <hardware/structs/trng.h>

LOG_MODULE_REGISTER(zlrs_trng, CONFIG_ZLRS_TRNG_LOG_LEVEL);

#define TRNG_WAIT_TIMEOUT_US 200U
#define TRNG_POLL_STEP_US 5U

/* 一次采样拿到的字（EHR 有 4 个字） */
#define TRNG_WORDS 4U

static bool started;
static uint32_t cache[TRNG_WORDS];
static uint8_t cached;

static int trng_acquire(void)
{
	uint32_t waited = 0U;

	if (!started) {
		trng_hw->sample_cnt1 = 0U;
		/* 关掉自检与去相关器：直接要原始 ROSC 采样（与 pico-sdk 一致） */
		trng_hw->trng_debug_control = 0xFFFFFFFFU;
		trng_hw->rnd_source_enable = 0xFFFFFFFFU;
		started = true;
	}

	trng_hw->rng_icr = 0xFFFFFFFFU;

	while (trng_hw->trng_busy != 0U) {
		if (waited >= TRNG_WAIT_TIMEOUT_US) {
			return -ETIMEDOUT;
		}

		k_busy_wait(TRNG_POLL_STEP_US);
		waited += TRNG_POLL_STEP_US;
	}

	for (uint8_t i = 0U; i < TRNG_WORDS; i++) {
		cache[i] = trng_hw->ehr_data[i];
	}
	cached = TRNG_WORDS;

	return 0;
}

static int trng_get_entropy(const struct device *dev, uint8_t *buf, uint16_t len)
{
	uint16_t off = 0U;

	ARG_UNUSED(dev);

	while (off < len) {
		if (cached == 0U) {
			const int rc = trng_acquire();

			if (rc != 0) {
				/* 不做软件兜底：清掉已写入的部分，把错误交给上层 */
				memset(buf, 0, len);
				LOG_ERR("TRNG read failed: %d", rc);
				return rc;
			}
		}

		const uint32_t word = cache[TRNG_WORDS - cached];
		const size_t n = MIN(sizeof(word), (size_t)(len - off));

		memcpy(&buf[off], &word, n);
		off = (uint16_t)(off + n);
		cached--;
	}

	return 0;
}

static const struct entropy_driver_api trng_api = {
	.get_entropy = trng_get_entropy,
};

#define TRNG_NODE DT_NODELABEL(zlrs_trng)

DEVICE_DT_DEFINE(TRNG_NODE, NULL, NULL, NULL, NULL, PRE_KERNEL_1,
		 CONFIG_ENTROPY_INIT_PRIORITY, &trng_api);
