/* SPDX-License-Identifier: Apache-2.0 */

#include "link/bench.h"
#include "link/bench_pattern.h"
#include "platform/slot_timer.h"

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_START     -1
#define BENCH_STOP      -2
#define BENCH_MAX_BYTES (16U * 1024U * 1024U)

/* Commands are one word: positive = arm byte count, negative = start/stop. */
static atomic_t command;
static atomic_t command_result;
K_SEM_DEFINE(command_done, 0, 1);

/* Only the RF owner accesses the live state. Shell reads published atomic counters. */
static struct {
	bool active;
	bool started;
	bool role_tx;
	uint32_t target;
	uint32_t generated;
	uint32_t received;
	uint32_t bad;
	uint32_t first_us;
	uint32_t first_len;
} bench;

static struct {
	atomic_t active;
	atomic_t target;
	atomic_t generated;
	atomic_t received;
	atomic_t bad;
	atomic_t duration_us;
	atomic_t first_len;
} published;

bool zlrs_bench_pump(zlrs_link_t *link)
{
	const int request = atomic_set(&command, 0);

	if (request != 0) {
		int rc = 0;

		if (request > 0) {
			if (link->queue_count != 0U || link->state != ZLRS_STATE_CONNECTED ||
			    link->sess_cfm_due) {
				rc = -EBUSY;
			} else {
				memset(&bench, 0, sizeof(bench));
				bench.active = true;
				bench.role_tx = link->cfg.role == ZLRS_ROLE_TX;
				bench.target = (uint32_t)request;
				atomic_set(&published.active, 1);
				atomic_set(&published.target, request);
				atomic_set(&published.generated, 0);
				atomic_set(&published.received, 0);
				atomic_set(&published.bad, 0);
				atomic_set(&published.duration_us, 0);
				atomic_set(&published.first_len, 0);
			}
		} else if (request == BENCH_START) {
			if (!bench.active) {
				rc = -EINVAL;
			} else {
				bench.started = true;
			}
		} else if (link->queue_count != 0U) {
			rc = -EBUSY;
		} else {
			bench.active = false;
			bench.started = false;
			atomic_set(&published.active, 0);
		}
		atomic_set(&command_result, rc);
		k_sem_give(&command_done);
	}
	if (!bench.active) {
		return false;
	}
	while (bench.started && bench.generated < bench.target) {
		uint8_t data[512];
		const uint16_t room = zlrs_link_tx_space(link);
		const uint16_t len =
			MIN(MIN((uint32_t)room, sizeof(data)), bench.target - bench.generated);

		if (len == 0U) {
			break;
		}
		zlrs_bench_pattern_fill(data, len, bench.generated, bench.role_tx);
		bench.generated += zlrs_link_write(link, data, len);
		atomic_set(&published.generated, bench.generated);
	}
	return true;
}

bool zlrs_bench_receive(const uint8_t *data, uint16_t len)
{
	if (!bench.active) {
		return false;
	}
	const uint32_t now = zlrs_slot_timer_now_us();

	if (bench.received == 0U) {
		bench.first_us = now;
		bench.first_len = len;
		atomic_set(&published.first_len, len);
	}
	const uint16_t valid =
		bench.received < bench.target ? MIN(len, bench.target - bench.received) : 0U;

	bench.bad += len - valid;
	bench.bad += zlrs_bench_pattern_bad(data, valid, bench.received, !bench.role_tx);
	bench.received += len;
	atomic_set(&published.bad, bench.bad);
	atomic_set(&published.duration_us, now - bench.first_us);
	/* Publish completion last: final timing/error counters precede it. */
	atomic_set(&published.received, bench.received);
	return true;
}

int zlrs_bench_command(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 1U || (argc == 2U && strcmp(argv[1], "status") == 0)) {
		const uint32_t received = atomic_get(&published.received);
		const uint32_t first = atomic_get(&published.first_len);
		const uint32_t duration = atomic_get(&published.duration_us);
		const uint32_t goodput =
			duration != 0U && received > first
				? (uint64_t)(received - first) * 1000000U / duration
				: 0U;

		shell_print(sh,
			    "BENCH active=%u target=%u queued=%u rx=%u bad=%u duration_us=%u "
			    "goodput_Bps=%u",
			    (unsigned int)atomic_get(&published.active),
			    (unsigned int)atomic_get(&published.target),
			    (unsigned int)atomic_get(&published.generated), received,
			    (unsigned int)atomic_get(&published.bad), duration, goodput);
		return 0;
	}
	int request;

	if (argc == 3U && strcmp(argv[1], "arm") == 0) {
		char *end;
		const unsigned long bytes = strtoul(argv[2], &end, 0);

		if (argv[2][0] == '-' || *end != '\0' || bytes == 0U || bytes > BENCH_MAX_BYTES) {
			return -EINVAL;
		}
		request = (int)bytes;
	} else if (argc == 2U && strcmp(argv[1], "start") == 0) {
		request = BENCH_START;
	} else if (argc == 2U && strcmp(argv[1], "stop") == 0) {
		request = BENCH_STOP;
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

	shell_print(sh, "BENCH %s: %d", argv[1], rc);
	return rc;
}
