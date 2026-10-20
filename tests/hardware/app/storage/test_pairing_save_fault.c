/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include "persist.h"
#include "fixtures.h"

static atomic_t requested_ms;
static atomic_t active;
static atomic_t restores;
static struct k_spinlock trace_lock;
static uint32_t started_ms, calls, first_ms, last_ms, min_gap_ms;

bool __real_zlrs_persist_get_pairing(zlrs_persist_pairing_t *out);
int __real_zlrs_persist_save_pairing(const zlrs_persist_pairing_t *pairing);

bool __wrap_zlrs_persist_get_pairing(zlrs_persist_pairing_t *out)
{
	/* Preserve the real credentials. Hide their saved copy only while EIOs
	 * exercise the real application's retry scheduling during physical DATA.
	 */
	return !atomic_get(&active) && __real_zlrs_persist_get_pairing(out);
}

int __wrap_zlrs_persist_save_pairing(const zlrs_persist_pairing_t *pairing)
{
	if (!atomic_get(&active)) {
		return __real_zlrs_persist_save_pairing(pairing);
	}
	const uint32_t now = k_uptime_get_32();
	k_spinlock_key_t key = k_spin_lock(&trace_lock);
	const uint32_t elapsed = now - started_ms;
	if (calls == 0U) {
		first_ms = elapsed;
	} else {
		min_gap_ms = MIN(min_gap_ms, elapsed - last_ms);
	}
	last_ms = elapsed;
	calls++;
	k_spin_unlock(&trace_lock, key);
	return -EIO;
}

static void restore_storage(struct k_work *work)
{
	ARG_UNUSED(work);
	atomic_inc(&restores);
	atomic_clear(&active);
}

K_WORK_DELAYABLE_DEFINE(restore_storage_work, restore_storage);

void zlrs_test_pairing_save_fault_apply(void)
{
	const int duration = atomic_set(&requested_ms, 0);
	if (duration > 0) {
		k_spinlock_key_t key = k_spin_lock(&trace_lock);
		started_ms = k_uptime_get_32();
		min_gap_ms = UINT32_MAX;
		k_spin_unlock(&trace_lock, key);
		atomic_set(&active, 1);
		k_work_schedule(&restore_storage_work, K_MSEC(duration));
	}
}

int zlrs_test_pairing_save_fault_command(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 3U) {
		char *end;
		const unsigned long ms = strtoul(argv[2], &end, 0);
		if (argv[2][0] == '-' || *end != '\0' || ms < 4000UL || ms > 30000UL) {
			return -EINVAL;
		}
		if (atomic_get(&active) || atomic_get(&restores) ||
		    !atomic_cas(&requested_ms, 0, (int)ms)) {
			return -EBUSY;
		}
		shell_print(sh, "SAVE_FAULT queued_ms=%lu", ms);
		return 0;
	}
	if (argc != 2U) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&trace_lock);
	const uint32_t count = calls, first = first_ms, last = last_ms, gap = min_gap_ms;
	k_spin_unlock(&trace_lock, key);
	shell_print(sh,
		    "SAVE_FAULT active=%u pending=%u calls=%u first_ms=%u last_ms=%u min_gap_ms=%u "
		    "restores=%u",
		    (unsigned int)atomic_get(&active),
		    (unsigned int)(atomic_get(&requested_ms) != 0), count, first, last, gap,
		    (unsigned int)atomic_get(&restores));
	return 0;
}
