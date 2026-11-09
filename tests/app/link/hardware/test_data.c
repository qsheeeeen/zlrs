/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <errno.h>
#include <string.h>

#include "data_pattern.h"
#include "fixtures.h"
#include "platform/slot_timer.h"

#define START     -1
#define STOP      -2
#define MAX_BYTES (16U * 1024U * 1024U)

static atomic_t command;
static atomic_t command_result;
K_SEM_DEFINE(command_done, 0, 1);

/* Only the production RF owner touches the live source/sink. */
static struct {
	bool active;
	bool started;
	bool role_leader;
	uint32_t target;
	uint32_t generated;
	uint32_t received;
	uint32_t bad;
	uint32_t first_us;
} stream;

static struct {
	atomic_t active;
	atomic_t target;
	atomic_t generated;
	atomic_t received;
	atomic_t bad;
	atomic_t duration_us;
	atomic_t first_len;
} published;

uint8_t *__real_zlrs_link_tx_reserve(zlrs_link_t *link, uint16_t *capacity);
uint16_t __real_zlrs_serial_read(uint8_t *buf, uint16_t len);
bool __real_zlrs_serial_receive(const uint8_t *buf, uint16_t len);

uint8_t *__wrap_zlrs_link_tx_reserve(zlrs_link_t *link, uint16_t *capacity)
{
	{
		zlrs_test_radio_apply(link);
		const int request = atomic_set(&command, 0);
		int rc = 0;

		if (request > 0) {
			if (link->status.queue_count != 0U ||
			    link->status.state != ZLRS_STATE_CONNECTED ||
			    link->status.sess_cfm_due) {
				rc = -EBUSY;
			} else {
				memset(&stream, 0, sizeof(stream));
				stream.active = true;
				stream.role_leader = link->status.cfg.role == ZLRS_ROLE_LEADER;
				stream.target = (uint32_t)request;
				atomic_set(&published.active, 1);
				atomic_set(&published.target, request);
				atomic_clear(&published.generated);
				atomic_clear(&published.received);
				atomic_clear(&published.bad);
				atomic_clear(&published.duration_us);
				atomic_clear(&published.first_len);
			}
		} else if (request == START) {
			if (!stream.active) {
				rc = -EINVAL;
			} else {
				stream.started = true;
			}
		} else if (request == STOP) {
			if (link->status.queue_count != 0U) {
				rc = -EBUSY;
			} else {
				stream.active = false;
				stream.started = false;
				atomic_clear(&published.active);
			}
		}
		if (request != 0) {
			atomic_set(&command_result, rc);
			k_sem_give(&command_done);
		}
	}
	return __real_zlrs_link_tx_reserve(link, capacity);
}

uint16_t __wrap_zlrs_serial_read(uint8_t *buf, uint16_t len)
{
	if (!stream.active) {
		return __real_zlrs_serial_read(buf, len);
	}
	if (!stream.started || stream.generated == stream.target) {
		return 0U;
	}
	len = MIN(len, stream.target - stream.generated);
	zlrs_bench_pattern_fill(buf, len, stream.generated, stream.role_leader);
	stream.generated += len;
	atomic_set(&published.generated, stream.generated);
	return len;
}

bool __wrap_zlrs_serial_receive(const uint8_t *buf, uint16_t len)
{
	if (!stream.active) {
		return __real_zlrs_serial_receive(buf, len);
	}
	const uint32_t now = zlrs_slot_timer_now_us();

	if (stream.received == 0U) {
		stream.first_us = now;
		atomic_set(&published.first_len, len);
	}
	const uint16_t valid =
		stream.received < stream.target ? MIN(len, stream.target - stream.received) : 0U;

	stream.bad += len - valid;
	stream.bad += zlrs_bench_pattern_bad(buf, valid, stream.received, !stream.role_leader);
	stream.received += len;
	atomic_set(&published.bad, stream.bad);
	atomic_set(&published.duration_us, now - stream.first_us);
	/* Publish completion after the error and timing counters. */
	atomic_set(&published.received, stream.received);
	return true;
}

int zlrs_test_data_control(struct zlrs_test_report *report, enum zlrs_test_op op, uint32_t arg)
{
	if (op == TEST_DATA_STATUS) {
		const uint32_t received = atomic_get(&published.received);
		const uint32_t first = atomic_get(&published.first_len);
		const uint32_t duration = atomic_get(&published.duration_us);
		const uint32_t rate = duration != 0U && received > first
					      ? (uint64_t)(received - first) * 1000000U / duration
					      : 0U;

		zlrs_test_print(report,
				"BENCH active=%u target=%u queued=%u rx=%u bad=%u duration_us=%u "
				"goodput_Bps=%u",
				(unsigned int)atomic_get(&published.active),
				(unsigned int)atomic_get(&published.target),
				(unsigned int)atomic_get(&published.generated), received,
				(unsigned int)atomic_get(&published.bad), duration, rate);
		return 0;
	}
	int request;

	if (op == TEST_DATA_ARM) {
		const uint32_t bytes = arg;

		if (bytes == 0U || bytes > MAX_BYTES) {
			return -EINVAL;
		}
		request = (int)bytes;
	} else if (op == TEST_DATA_START) {
		request = START;
	} else if (op == TEST_DATA_STOP) {
		request = STOP;
	} else {
		return -EINVAL;
	}
	k_sem_reset(&command_done);
	if (!atomic_cas(&command, 0, request)) {
		return -EBUSY;
	}
	if (k_sem_take(&command_done, K_SECONDS(2)) != 0) {
		(void)atomic_cas(&command, request, 0);
		return -ETIMEDOUT;
	}
	const int rc = atomic_get(&command_result);

	zlrs_test_print(report, "BENCH %s: %d",
			op == TEST_DATA_ARM     ? "arm"
			: op == TEST_DATA_START ? "start"
						: "stop",
			rc);
	return rc;
}
