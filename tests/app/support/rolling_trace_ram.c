/* SPDX-License-Identifier: Apache-2.0 */
/* Fixture-only rolling storage. Zephyr still encodes complete synchronous CTF events. */
#include <errno.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <tracing_backend.h>
#include <tracing_core.h>

BUILD_ASSERT(IS_ENABLED(CONFIG_TRACING_SYNC) && IS_ENABLED(CONFIG_TRACING_CTF),
	     "Rolling retention needs synchronous whole CTF records");

uint8_t ram_tracing[CONFIG_RAM_TRACING_BUFFER_SIZE];
static uint32_t head, used, overwritten;
static bool normalized;

uint32_t latency_trace_overwritten(void)
{
	return overwritten;
}

static void copy_ring(uint32_t offset, void *buf, uint32_t len, bool write)
{
	uint32_t first = MIN(len, sizeof(ram_tracing) - offset);

	if (write) {
		memcpy(ram_tracing + offset, buf, first);
		memcpy(ram_tracing, (uint8_t *)buf + first, len - first);
	} else {
		memcpy(buf, ram_tracing + offset, first);
		memcpy((uint8_t *)buf + first, ram_tracing, len - first);
	}
}

static void output(const struct tracing_backend *backend, uint8_t *data, uint32_t len)
{
	ARG_UNUSED(backend);
	if (normalized || len == 0U || len > UINT16_MAX || len + 2U > sizeof(ram_tracing)) {
		tracing_packet_drop_handle();
		return;
	}
	while (used + len + 2U > sizeof(ram_tracing)) {
		uint8_t size[2];
		copy_ring(head, size, sizeof(size), false);
		uint32_t bytes = sys_get_le16(size) + 2U;
		head = (head + bytes) % sizeof(ram_tracing);
		used -= bytes;
		overwritten++;
	}
	uint32_t tail = (head + used) % sizeof(ram_tracing);
	uint8_t size[2];
	sys_put_le16(len, size);
	copy_ring(tail, size, sizeof(size), true);
	copy_ring((tail + 2U) % sizeof(ram_tracing), data, len, true);
	used += len + 2U;
}

static void reverse(uint32_t start, uint32_t end)
{
	while (start < end) {
		uint8_t byte = ram_tracing[start];
		ram_tracing[start++] = ram_tracing[--end];
		ram_tracing[end] = byte;
	}
}

/* Called only after freeze, in RF maintenance. Rotate and strip framing in place. */
static int flush(const struct tracing_backend *backend)
{
	ARG_UNUSED(backend);
	if (normalized) {
		return 0;
	}
	if (head != 0U) {
		reverse(0U, head);
		reverse(head, sizeof(ram_tracing));
		reverse(0U, sizeof(ram_tracing));
	}
	uint32_t read = 0U, write = 0U;

	while (read < used) {
		uint16_t len = sys_get_le16(ram_tracing + read);
		read += 2U;
		if (len > used - read) {
			return -EBADMSG;
		}
		memmove(ram_tracing + write, ram_tracing + read, len);
		read += len;
		write += len;
	}
	memset(ram_tracing + write, 0, sizeof(ram_tracing) - write);
	normalized = true;
	return 0;
}

static void init(void)
{
	memset(ram_tracing, 0, sizeof(ram_tracing));
	head = used = overwritten = 0U;
	normalized = false;
}

static const struct tracing_backend_api api = {.init = init, .output = output, .flush = flush};
TRACING_BACKEND_DEFINE(tracing_backend_ram, api);
