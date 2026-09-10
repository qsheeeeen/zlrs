/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 协议核心的主机侧单元测试。期望值都来自独立来源：
 *   - MD5 用 RFC 1321 的标准测试向量
 *   - UID 用 Python hashlib 独立算出的结果
 *   - CRC 用按文档算法独立实现的参考值
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zlrs/proto/config.h"
#include "zlrs/proto/crc.h"
#include "zlrs/proto/fhss.h"
#include "zlrs/proto/uid.h"

static unsigned int checks;
static unsigned int failures;

#define CHECK(cond)                                                                \
	do {                                                                       \
		checks++;                                                          \
		if (!(cond)) {                                                     \
			failures++;                                                \
			printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
		}                                                                  \
	} while (0)

static void to_hex(const uint8_t *in, size_t len, char *out)
{
	static const char digits[] = "0123456789abcdef";

	for (size_t i = 0; i < len; i++) {
		out[i * 2] = digits[in[i] >> 4];
		out[i * 2 + 1] = digits[in[i] & 0x0FU];
	}
	out[len * 2] = '\0';
}

/* ---- MD5：RFC 1321 A.5 测试向量 ------------------------------------------ */
static void test_md5(void)
{
	static const struct {
		const char *input;
		const char *digest;
	} vectors[] = {
		{"", "d41d8cd98f00b204e9800998ecf8427e"},
		{"abc", "900150983cd24fb0d6963f7d28e17f72"},
		{"The quick brown fox jumps over the lazy dog",
		 "9e107d9d372bb6826bd81d3542a419d6"},
	};

	printf("md5\n");

	for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
		zlrs_md5_ctx_t ctx;
		uint8_t digest[16];
		char hex[33];

		zlrs_md5_init(&ctx);
		zlrs_md5_update(&ctx, vectors[i].input, strlen(vectors[i].input));
		zlrs_md5_final(&ctx, digest);
		to_hex(digest, sizeof(digest), hex);

		CHECK(strcmp(hex, vectors[i].digest) == 0);
	}
}

/* ---- UID：绑定短语 -> UID ------------------------------------------------ */
static void test_uid(void)
{
	uint8_t uid[ZLRS_UID_LEN];
	uint8_t other[ZLRS_UID_LEN];

	printf("uid\n");

	zlrs_uid_from_binding_phrase("", uid);
	CHECK(!zlrs_uid_is_bound(uid));
	CHECK(uid[0] == 0U && uid[5] == 0U);

	/* md5('-DMY_BINDING_PHRASE="test"') 的前 6 字节 */
	zlrs_uid_from_binding_phrase("test", uid);
	CHECK(uid[0] == 0x4fU && uid[1] == 0x04U && uid[2] == 0xfdU &&
	      uid[3] == 0x82U && uid[4] == 0x21U && uid[5] == 0x55U);
	CHECK(zlrs_uid_is_bound(uid));

	/* 换个短语必须换一个 UID */
	zlrs_uid_from_binding_phrase("my-radio", other);
	CHECK(memcmp(uid, other, sizeof(uid)) != 0);
	CHECK(other[0] == 0x2cU && other[1] == 0x49U && other[2] == 0xfdU &&
	      other[3] == 0xa0U);

	/* NULL 等价于空短语 */
	zlrs_uid_from_binding_phrase(NULL, uid);
	CHECK(!zlrs_uid_is_bound(uid));

	/* tail 取的是 UID 后两个字节 */
	{
		uint8_t uid4 = 0U;
		uint8_t uid5 = 0U;

		zlrs_uid_from_binding_phrase("test", uid);
		zlrs_uid_tail(uid, &uid4, &uid5);
		CHECK(uid4 == uid[4] && uid5 == uid[5]);
	}
}

/* ---- CRC：按文档算法独立实现的参考值 ------------------------------------- */
static void test_crc(void)
{
	zlrs_crc_t crc;
	uint16_t value;
	uint16_t part;

	printf("crc\n");

	zlrs_crc_init(&crc, 16U, 0x3D65U);
	CHECK(crc.bitmask == 0xFFFFU);
	CHECK(zlrs_crc_calc(&crc, (const uint8_t *)"123456789", 9U, 0U) == 0x3D48U);
	CHECK(zlrs_crc_calc(&crc, (const uint8_t *)"abc", 3U, 0U) == 0xA8E3U);

	/* 增量计算等价于一次算完 */
	part = zlrs_crc_calc(&crc, (const uint8_t *)"ab", 2U, 0U);
	part = zlrs_crc_calc(&crc, (const uint8_t *)"c", 1U, part);
	CHECK(part == 0xA8E3U);

	zlrs_crc_init(&crc, 14U, 0x2E57U);
	CHECK(crc.bitmask == 0x3FFFU);
	value = zlrs_crc_calc(&crc, (const uint8_t *)"abc", 3U, 0U);
	CHECK(value == 0x3DFDU);
	CHECK(value <= crc.bitmask);

	/* 改一个字节，校验值必须变 */
	CHECK(zlrs_crc_calc(&crc, (const uint8_t *)"abd", 3U, 0U) != value);

	/* 重复 init 相同参数是幂等的 */
	zlrs_crc_init(&crc, 14U, 0x2E57U);
	CHECK(zlrs_crc_calc(&crc, (const uint8_t *)"abc", 3U, 0U) == value);
}

/* ---- FHSS：结构性质 ------------------------------------------------------ */
static void test_fhss(void)
{
	zlrs_fhss_t fhss;

	printf("fhss\n");

	zlrs_fhss_init(&fhss, ZLRS_DOMAIN_ISM2400, 0x12345678U);

	CHECK(fhss.sequence_count > 0U);
	CHECK((fhss.sequence_count % fhss.domain->freq_count) == 0U);
	CHECK(fhss.sync_channel == (uint8_t)(fhss.domain->freq_count / 2U));

	/* 每块的第 0 个时隙必须是 sync 频道，且所有频点在合法范围内 */
	for (uint16_t i = 0; i < fhss.sequence_count; i++) {
		if ((i % fhss.domain->freq_count) == 0U) {
			CHECK(fhss.sequence[i] == fhss.sync_channel);
		}
		CHECK(fhss.sequence[i] < fhss.domain->freq_count);
	}

	/* 同一块内不出现重复频点 */
	for (uint16_t block = 0; block < fhss.sequence_count / fhss.domain->freq_count;
	     block++) {
		uint8_t seen[256] = {0};

		for (uint32_t k = 0; k < fhss.domain->freq_count; k++) {
			uint8_t ch = fhss.sequence[block * fhss.domain->freq_count + k];

			CHECK(seen[ch] == 0U);
			seen[ch] = 1U;
		}
	}

	/* 初始化后落在 sync 频道 */
	CHECK(zlrs_fhss_on_sync_channel(&fhss));
	CHECK(zlrs_fhss_initial_freq(&fhss) >= fhss.domain->freq_start);
	CHECK(zlrs_fhss_initial_freq(&fhss) <= fhss.domain->freq_stop);

	(void)zlrs_fhss_next_freq(&fhss);
	CHECK(zlrs_fhss_curr_index(&fhss) == 1U);
	CHECK(!zlrs_fhss_on_sync_channel(&fhss));

	/* 相同种子得到相同序列，不同种子得到不同序列 */
	{
		zlrs_fhss_t same;
		zlrs_fhss_t shuffled;

		zlrs_fhss_init(&same, ZLRS_DOMAIN_ISM2400, 0x12345678U);
		CHECK(memcmp(fhss.sequence, same.sequence, fhss.sequence_count) == 0);

		zlrs_fhss_init(&shuffled, ZLRS_DOMAIN_ISM2400, 0xDEADBEEFU);
		CHECK(memcmp(fhss.sequence, shuffled.sequence, fhss.sequence_count) != 0);
	}

	/* 越界 domain 回落到 2.4G */
	zlrs_fhss_init(&fhss, ZLRS_DOMAIN_COUNT, 1U);
	CHECK(fhss.domain == &zlrs_fhss_domains[ZLRS_DOMAIN_ISM2400]);
}

int main(void)
{
	printf("zlrs proto host tests\n\n");

	test_md5();
	test_uid();
	test_crc();
	test_fhss();

	printf("\n%u checks, %u failure(s)\n", checks, failures);

	return (failures == 0U) ? 0 : 1;
}
