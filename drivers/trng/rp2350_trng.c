/*
 * RP2350 硬件 TRNG 熵源（zlrs 自带驱动，见 doc/13-roadmap.md）。
 *
 * 为什么不用 Zephyr 树里的 entropy_rpi_pico：那条路调 pico-sdk 的
 * get_rand_64()，取数时拿自旋锁并把中断关掉，等 192 个 ROSC 采样填满 EHR 才
 * 返回——USB 的中断与工作队列在这期间得不到服务，实测打开后 USB 枚举不出来。
 *
 * 取数流程照 RP2350 datasheet 12.12，走 Arm TRNG 的标准用法（bootrom 和
 * pico-sdk 走的是另一条路：置位 TRNG_DEBUG_CONTROL 绕过熵检查与去相关器，
 * 把原始 ROSC 采样交给 SHA-256 / xoroshiro 去处理）：
 *
 *   - 复位后三项熵检查默认全开，不用显式打开；TRNG_CONFIG 选 ROSC 链长，
 *     SAMPLE_CNT1 定采样间隔。datasheet 12.12.2 给的是"链长 0/1 + 采样数
 *     20–25，平均约 2 ms 一块"，但这档在 RP2350 上过不了 Von Neumann 检查
 *     （板子上实测：多次报 32 bit 连 0/连 1，偶尔直接自相关连败停摆），
 *     datasheet 12.12.4 自己也说官方驱动不走这条路。采样数取它点名的 100：
 *     同一台板子上连跑 10 块全干净，代价是生成时间按采样数成比例变长。
 *   - 一块是 6 个 EHR_DATA 字；读最后一个字才清空结果寄存器，也才接着采
 *     下一块（12.12.3）。
 *   - 生成时间不可预测，偶发会超过平均值的 100 倍，datasheet 因此建议攒一个
 *     小池子、池子有空位就启动下一次生成（12.12.4）。probe 里先把第一块启动
 *     起来（那时调度器还没起来，不能干等），后面取数就不必从零等。
 *
 * 两条底线：不关中断、不拿自旋锁；等待有界（超时返回错误）。
 * **不做软件兜底**：读不出来、熵检查不过就是失败，错误原样交给上层（CSPRNG /
 * PSA / 开机自检）——不能用"看起来能跑"的假熵糊过去。
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

/* 链长 0（ROSC 最快）配采样数 100，见文件头：20–25 那档过不了 VN 检查 */
#define TRNG_ROSC_CHAIN   0U
#define TRNG_SAMPLE_TICKS 100U

/* 一块 = 192 bit = 6 个 EHR_DATA 字 */
#define TRNG_BLOCK_WORDS 6U
#define TRNG_BLOCK_BYTES (TRNG_BLOCK_WORDS * sizeof(uint32_t))

/*
 * 池子攒两块：一块应付当前这次取数，另一块留在池子里，硬件继续往后采——
 * datasheet 12.12.4 说的"小池子"。
 */
#define TRNG_POOL_BLOCKS 2U
#define TRNG_POOL_BYTES  (TRNG_POOL_BLOCKS * TRNG_BLOCK_BYTES)

/* 生成时间偶发超过平均值的 100 倍（12.12.4），超时按这个量级留 */
#define TRNG_BLOCK_TIMEOUT_MS 1000U
#define TRNG_POLL_INTERVAL_MS 1U

/* 检查失败的那一块不能当数据用，重来一块是正常流程，不是兜底 */
#define TRNG_BLOCK_ATTEMPTS 3U

/* RNG_ISR 里软件能清的位；AUTOCORR_ERR 只有软复位能清（12.12.5） */
#define TRNG_ISR_CLEARABLE                                          \
	(TRNG_RNG_ISR_EHR_VALID_BITS | TRNG_RNG_ISR_VN_ERR_BITS |   \
	 TRNG_RNG_ISR_CRNGT_ERR_BITS)

/*
 * HAL 的 trng_hw_t 从 RNG_IMR（0x100）开始排，DT 的 reg 给的是 TRNG 块基址
 * （datasheet 12.12.5 的 0x400f0000）。
 */
#define TRNG_CONFIG_INIT(inst)                                              \
	{                                                                   \
		.regs = (trng_hw_t *)(uintptr_t)(DT_INST_REG_ADDR(inst) +   \
						 TRNG_RNG_IMR_OFFSET)       \
	}

struct trng_config {
	trng_hw_t *regs;
};

struct trng_data {
	/* 池子：取数只从这里拿字节，空了才去等硬件 */
	uint8_t pool[TRNG_POOL_BYTES];
	uint16_t pool_head; /* 下一个交出去的字节 */
	uint16_t pool_len;  /* pool_head 之后还有多少字节 */
	/* RND_SRC_EN 置位：有块在采，或采好了等着取 */
	bool running;
	/* 取数要等硬件，entropy API 要求线程安全 */
	struct k_mutex lock;
};

/* 软复位 + datasheet 12.12.2 的配置，熵源停在关闭状态 */
static void trng_reset(const struct trng_config *cfg, struct trng_data *data)
{
	trng_hw_t *const trng = cfg->regs;

	trng->rnd_source_enable = 0U;
	trng->trng_sw_reset = 1U;

	/* 软复位后要有固定延时（bootrom 的做法：读两次同一个寄存器） */
	(void)trng->trng_sw_reset;
	(void)trng->trng_sw_reset;

	trng->trng_config = TRNG_ROSC_CHAIN;
	trng->sample_cnt1 = TRNG_SAMPLE_TICKS;
	/* 轮询取数，不要中断。屏蔽位的复位值就是全屏蔽，写出来是说清楚意图 */
	trng->rng_imr = TRNG_RNG_IMR_EHR_VALID_INT_MASK_BITS |
			TRNG_RNG_IMR_VN_ERR_INT_MASK_BITS |
			TRNG_RNG_IMR_CRNGT_ERR_INT_MASK_BITS |
			TRNG_RNG_IMR_AUTOCORR_ERR_INT_MASK_BITS;
	trng->rng_icr = TRNG_ISR_CLEARABLE;
	data->running = false;
}

/* 启动一块。已经在采就不动——中途重写 RND_SRC_EN 会把当前这块打断 */
static void trng_start(const struct trng_config *cfg, struct trng_data *data)
{
	trng_hw_t *const trng = cfg->regs;

	if (data->running) {
		return;
	}

	/* RST_BITS_COUNTER 要求熵源处于关闭状态（12.12.5） */
	trng->rng_icr = TRNG_ISR_CLEARABLE;
	trng->rst_bits_counter = 1U;
	trng->rnd_source_enable = 1U;
	data->running = true;
}

/* 不用时关上熵源省电（12.12.3），顺手清掉采了一半的那块 */
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
 * 一块作废之后重新起一块：Von Neumann / CRNGT 检查失败清状态就行；自相关
 * 测试连败会让 RNG 整个停摆，那个位只有软复位能清（12.12.5）。
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
 * 等一块就绪：0 = EHR 里有数据可以读，其余是 errno。start 是这块开始等的时刻，
 * 重试共用同一个截止点：一块的等待上限就是 TRNG_BLOCK_TIMEOUT_MS。
 *
 * 只认 EHR_VALID，不认 trng_busy：生成刚开始那几拍 busy 还没抬起来，照 busy
 * 判会读到全 0 的结果寄存器。
 *
 * 检查位先于 EHR_VALID 判：出错的那一块也会把 EHR_VALID 置起来（实测），
 * datasheet 12.12.3 说检查不过的结果不能当数据用，所以宁可丢掉重来。
 */
static int trng_wait_block(const struct trng_config *cfg, uint32_t start)
{
	const trng_hw_t *trng = cfg->regs;

	for (;;) {
		const uint32_t isr = trng->rng_isr;

		if ((isr & TRNG_RNG_ISR_AUTOCORR_ERR_BITS) != 0U) {
			return -EIO;
		}
		if ((isr & (TRNG_RNG_ISR_VN_ERR_BITS |
			    TRNG_RNG_ISR_CRNGT_ERR_BITS)) != 0U) {
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

/* 把没取走的字节挪到最前面，下一块才能整块放进池子 */
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

/* 池子里还能放几块 */
static size_t trng_pool_room(struct trng_data *data)
{
	trng_pool_compact(data);

	return (sizeof(data->pool) - data->pool_len) / TRNG_BLOCK_BYTES;
}

/* 读满 6 个字搬进池子：读最后一个字会清结果寄存器，也接着采下一块（12.12.3） */
static void trng_pool_put_block(const struct trng_config *cfg, struct trng_data *data)
{
	const trng_hw_t *trng = cfg->regs;
	const size_t at = (size_t)data->pool_head + data->pool_len;

	__ASSERT_NO_MSG((sizeof(data->pool) - at) >= TRNG_BLOCK_BYTES);

	for (uint8_t i = 0U; i < TRNG_BLOCK_WORDS; i++) {
		sys_put_le32(trng->ehr_data[i],
			     &data->pool[at + (i * sizeof(uint32_t))]);
	}

	data->pool_len += TRNG_BLOCK_BYTES;
}

/* 等一块进池子。0 = 池子里多了一块 */
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
			/* 可能只是慢（12.12.4）：机器状态不动，下次取数接着等 */
			return rc;
		}

		trng_recover(cfg, data);
	}

	return -EIO;
}

/* 池子满了就关熵源；还有空位就把已经采好的块收进来，让硬件继续采下一块 */
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

		/* 还没好：停过（省电）或这块废了才重起，正采着就让它接着采 */
		if (!data->running ||
		    (isr & (TRNG_RNG_ISR_VN_ERR_BITS |
			    TRNG_RNG_ISR_CRNGT_ERR_BITS |
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

	/* 取数要等硬件（可以到几百毫秒），只能在调用线程里做 */
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
	 * 寄存器自检：驱动按 192 bit（6 个 EHR 字）取数，先确认这块 IP 真的是
	 * 192 bit 的 EHR——不是就说明这不是我们认识的 TRNG，别拿它当熵源。
	 */
	if ((cfg->regs->rng_version & TRNG_RNG_VERSION_EHR_WIDTH_192_BITS) == 0U) {
		LOG_ERR("TRNG reports a 128-bit EHR (RNG_VERSION 0x%x)",
			cfg->regs->rng_version);
		return -ENOTSUP;
	}

	k_mutex_init(&data->lock);
	trng_reset(cfg, data);

	/*
	 * 预取第一块：这里只把熵源打开，不等它——probe 跑在 PRE_KERNEL_1，
	 * 调度器还没起来，取数的等待不能放这儿。
	 */
	trng_start(cfg, data);

	return 0;
}

#define TRNG_DEFINE(inst)                                                    \
	static struct trng_data trng_data_##inst;                            \
	static const struct trng_config trng_config_##inst =                 \
		TRNG_CONFIG_INIT(inst);                                      \
	DEVICE_DT_INST_DEFINE(inst, trng_init, NULL, &trng_data_##inst,      \
			      &trng_config_##inst, PRE_KERNEL_1,             \
			      CONFIG_ENTROPY_INIT_PRIORITY, &trng_api);

DT_INST_FOREACH_STATUS_OKAY(TRNG_DEFINE)
