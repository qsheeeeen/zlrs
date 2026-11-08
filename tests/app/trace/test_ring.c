/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/sys/byteorder.h>
#include <tracing_backend.h>
#include <tracing_core.h>
extern uint8_t ram_tracing[CONFIG_RAM_TRACING_BUFFER_SIZE];
uint32_t latency_trace_overwritten(void);

static uint32_t length(uint32_t seq) { return 12U + (seq % 21U) * 3U; }

ZTEST(trace_ring, test_wrap_preserves_complete_variable_length_records)
{
	struct tracing_backend *b = tracing_backend_get("tracing_backend_ram");
	tracing_backend_init(b);
	for (uint32_t i = 0; i < 2000; i++) {
		uint8_t record[80];
		memset(record, i & 255U, sizeof(record));
		sys_put_le32(i, record);
		tracing_backend_output(b, record, length(i));
	}
	uint32_t first = latency_trace_overwritten();
	zassert_true(first > 1900U && first < 2000U);
	zassert_ok(tracing_backend_flush(b));
	uint32_t pos = 0;
	for (uint32_t i = first; i < 2000; i++) {
		zassert_equal(sys_get_le32(ram_tracing + pos), i);
		for (uint32_t j = 4; j < length(i); j++) {
			zassert_equal(ram_tracing[pos + j], i & 255U);
		}
		pos += length(i);
	}
	for (uint32_t j = pos; j < sizeof(ram_tracing); j++) {
		zassert_equal(ram_tracing[j], 0U);
	}
	uint8_t frozen[CONFIG_RAM_TRACING_BUFFER_SIZE];
	memcpy(frozen, ram_tracing, sizeof(frozen));
	zassert_ok(tracing_backend_flush(b));
	zassert_mem_equal(frozen, ram_tracing, sizeof(frozen));
}

ZTEST(trace_ring, test_oversize_and_reset)
{
	struct tracing_backend *b = tracing_backend_get("tracing_backend_ram");
	tracing_backend_init(b);
	uint8_t byte = 7;
	uint32_t dropped = tracing_packet_drop_count_get();
	tracing_backend_output(b, &byte, sizeof(ram_tracing));
	zassert_equal(tracing_packet_drop_count_get(), dropped + 1U);
	tracing_backend_output(b, &byte, 1U);
	zassert_ok(tracing_backend_flush(b));
	zassert_equal(ram_tracing[0], 7U);
	zassert_equal(ram_tracing[1], 0U);
	tracing_backend_init(b);
	zassert_equal(latency_trace_overwritten(), 0U);
	zassert_ok(tracing_backend_flush(b));
	zassert_equal(ram_tracing[0], 0U);
}
ZTEST_SUITE(trace_ring, NULL, NULL, NULL, NULL, NULL);
