/* SPDX-License-Identifier: Apache-2.0 */
/* Run the real application loop and boot policy with simulated hardware/business endpoints. */
#include <errno.h>
#include <string.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/ztest.h>

#include "boot.h"
#include "persist.h"
#include "serial.h"
#include "mgmt.h"
#include "platform/watchdog.h"
#include "rf.h"
#include "ui.h"

int zlrs_application_main(void);

ZBUS_CHAN_DEFINE(zlrs_input_chan, struct zlrs_input, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

static K_THREAD_STACK_DEFINE(owner_stack, 4096);
static struct k_thread owner_thread;
static K_SEM_DEFINE(done, 0, 1);
static unsigned int reboots;
static unsigned int disarms;
static unsigned int confirm_writes;
static int confirm_result;
static int retention_result;
static bool image_confirmed;

bool boot_is_img_confirmed(void)
{
	return image_confirmed;
}

int boot_write_img_confirmed(void)
{
	confirm_writes++;
	if (confirm_result == 0) {
		image_confirmed = true;
	}
	return confirm_result;
}

int bootmode_set(uint8_t mode)
{
	zassert_equal(mode, BOOT_MODE_TYPE_BOOTLOADER);
	return retention_result;
}

void zlrs_watchdog_disarm(void)
{
	disarms++;
}

void zlrs_watchdog_feed(void)
{
}

static void finish(void)
{
	k_sem_give(&done);
	k_thread_abort(k_current_get());
}

void sys_reboot(int type)
{
	zassert_equal(type, SYS_REBOOT_COLD);
	reboots++;
	finish();
	CODE_UNREACHABLE;
}

enum action {
	CHECK_FAULT_ROLLBACK,
	CHECK_TIMEOUT_ROLLBACK,
	RECOVER,
	RUN_APPLICATION
};

static void owner(void *arg, void *unused1, void *unused2)
{
	const enum action action = (enum action)(uintptr_t)arg;
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	switch (action) {
	case CHECK_FAULT_ROLLBACK:
		zlrs_boot_check_rollback(true);
		break;
	case CHECK_TIMEOUT_ROLLBACK:
		zlrs_boot_check_rollback(false);
		break;
	case RECOVER:
		(void)zlrs_boot_recovery();
		break;
	case RUN_APPLICATION:
		(void)zlrs_application_main();
		break;
	}
	zassert_unreachable("owner should have exited through the simulated reboot/frame endpoint");
}

static void run(enum action action)
{
	k_sem_reset(&done);
	k_thread_create(&owner_thread, owner_stack, K_THREAD_STACK_SIZEOF(owner_stack), owner,
			(void *)(uintptr_t)action, NULL, NULL, 0, 0, K_NO_WAIT);
	zassert_equal(k_sem_take(&done, K_SECONDS(35)), 0,
		      "owner did not reach its expected endpoint");
	zassert_equal(k_thread_join(&owner_thread, K_SECONDS(1)), 0);
}

static bool rf_fault;
static unsigned int starts, frames, idle_calls, sessions, pairing_requests, saves, publications;
static unsigned int pause_depth;
static bool fail_first_save;
static zlrs_persist_pairing_t saved_pairing;
static bool pairing_saved, rf_paired;
static bool pairing_scenario, cancel_waiting_pair, expire_waiting_pair;
static unsigned int busy_pair_attempts;
static int pair_result;
static const zlrs_persist_settings_t settings = {.data_baud = 115200U};
static const zlrs_persist_pairing_t paired = {
	.peer_id = {1, 2, 3, 4},
	.pair_salt = {5, 6, 7, 8},
};

int zlrs_persist_init(void)
{
	return 0;
}

const zlrs_persist_settings_t *zlrs_persist_settings(void)
{
	return &settings;
}

bool zlrs_persist_get_pairing(zlrs_persist_pairing_t *out)
{
	if (!pairing_saved) {
		return false;
	}
	*out = saved_pairing;
	return true;
}

int zlrs_persist_save_pairing(const zlrs_persist_pairing_t *pairing)
{
	zassert_equal(pause_depth, 1U,
		      "pairing must be saved in an application maintenance window");
	saves++;
	if (fail_first_save && saves == 1U) {
		return -EIO;
	}
	saved_pairing = *pairing;
	pairing_saved = true;
	return 0;
}

int zlrs_mgmt_init(void)
{
	return 0;
}

bool zlrs_mgmt_process_idle(void)
{
	idle_calls++;
	if (pairing_scenario) {
		return cancel_waiting_pair && frames == 2U && sessions == 0U;
	}
	return idle_calls == 1U || (!rf_fault && frames == 3U && sessions == 1U);
}

int zlrs_mgmt_run(void)
{
	zassert_equal(pause_depth, 1U);
	sessions++;
	if (sessions == 2U) {
		/* ERASE_PAIR deletes the saved record and reloads RF credentials in this window. */
		zassert_true(pairing_saved && rf_paired);
		pairing_saved = false;
		zlrs_rf_credentials();
	}
	return 0;
}

void zlrs_rf_log_snapshot(void)
{
	zassert_equal(pause_depth, 1U);
}

void zlrs_rf_pause(void)
{
	pause_depth++;
}

void zlrs_rf_resume(void)
{
	zassert_equal(pause_depth, 1U);
	pause_depth--;
}

int zlrs_rf_init(void)
{
	rf_paired = true;
	return rf_fault ? -ENODEV : 0;
}

void zlrs_rf_credentials(void)
{
	zassert_equal(pause_depth, 1U);
	zlrs_persist_pairing_t pairing;
	rf_paired = zlrs_persist_get_pairing(&pairing);
}

void zlrs_rf_start(void)
{
	starts++;
}

void zlrs_rf_publish_state(void)
{
	publications++;
}

int zlrs_rf_pair_start(void)
{
	pairing_requests++;
	return pairing_requests <= busy_pair_attempts ? -EAGAIN : pair_result;
}

bool zlrs_rf_pairing(uint8_t peer_id[ZLRS_DEV_ID_LEN], uint8_t salt[ZLRS_PAIR_SALT_LEN])
{
	if (!rf_paired) {
		return false;
	}
	memcpy(peer_id, paired.peer_id, sizeof(paired.peer_id));
	memcpy(salt, paired.pair_salt, sizeof(paired.pair_salt));
	return true;
}

void zlrs_rf_frame(void)
{
	zassert_equal(pause_depth, 0U);
	k_msleep(500);
	frames++;
	if (expire_waiting_pair && frames == 2U) {
		k_msleep(30001);
	}
	if (frames == 5U) {
		finish();
	}
}

int zlrs_serial_init(uint32_t baud)
{
	zassert_equal(baud, 115200U);
	return 0;
}

void zlrs_serial_publish_state(void)
{
	if (rf_fault && idle_calls == 3U) {
		finish();
	}
}

ZTEST(application, test_lifecycle)
{
	/* A failed confirm must leave the image eligible for rollback; retry can succeed. */
	confirm_result = -EIO;
	zassert_equal(zlrs_boot_confirm(), -EIO);
	zassert_false(image_confirmed);
	run(CHECK_FAULT_ROLLBACK);
	zassert_equal(reboots, 1U);
	/* The fault wait has also passed the ordinary confirmation deadline. */
	run(CHECK_TIMEOUT_ROLLBACK);
	zassert_equal(reboots, 2U);
	/* Failed retention must keep watchdog protection; recovery success hands off to MCUboot. */
	retention_result = -EIO;
	zassert_equal(zlrs_boot_recovery(), -EIO);
	zassert_equal(disarms, 0U);
	retention_result = 0;
	run(RECOVER);
	zassert_equal(disarms, 1U);
	zassert_equal(reboots, 3U);
	confirm_result = 0;
	zassert_equal(zlrs_boot_confirm(), 0);
	zassert_equal(confirm_writes, 2U);
	zlrs_boot_check_rollback(true);
	zassert_equal(reboots, 3U, "confirmed firmware must remain reachable on an RF fault");

	const struct zlrs_input input = {.button_holds = 1U};
	zassert_equal(zbus_chan_pub(&zlrs_input_chan, &input, K_NO_WAIT), 0);
	fail_first_save = true;
	run(RUN_APPLICATION);
	zassert_equal(starts, 1U);
	zassert_equal(frames, 5U);
	zassert_equal(sessions, 2U);
	zassert_equal(pairing_requests, 1U, "a long press must be consumed once");
	zassert_equal(saves, 2U, "failed pairing save must retry without rewriting a saved record");
	zassert_false(pairing_saved || rf_paired,
		      "erasing pairing must clear both the saved record and the active link");
	zassert_equal(pause_depth, 0U);

	starts = frames = idle_calls = sessions = pairing_requests = saves = publications = 0U;
	rf_fault = true;
	run(RUN_APPLICATION);
	zassert_equal(starts, 0U);
	zassert_equal(frames, 0U);
	zassert_equal(sessions, 1U, "RF initialization failure must leave management reachable");
	zassert_true(publications > 0U);
	zassert_equal(pause_depth, 0U);

	/* The real boot policy has now confirmed this process. Fresh application
	 * owners below exercise independent button intents against that same boot.
	 */
	static const struct {
		unsigned int busy_attempts;
		int result;
		bool management;
		bool expire;
		unsigned int expected_attempts;
	} cases[] = {
		{2U, 0, false, false, 3U},
		{100U, 0, true, false, 2U},
		{0U, -EIO, false, false, 1U},
		{100U, 0, false, true, 2U},
	};

	for (size_t i = 0U; i < ARRAY_SIZE(cases); i++) {
		starts = frames = idle_calls = sessions = pairing_requests = saves = publications =
			0U;
		rf_fault = fail_first_save = pairing_saved = rf_paired = false;
		pairing_scenario = true;
		busy_pair_attempts = cases[i].busy_attempts;
		pair_result = cases[i].result;
		cancel_waiting_pair = cases[i].management;
		expire_waiting_pair = cases[i].expire;
		zassert_ok(zbus_chan_pub(&zlrs_input_chan, &input, K_NO_WAIT));
		run(RUN_APPLICATION);
		zassert_equal(pairing_requests, cases[i].expected_attempts, "button case %zu", i);
		zassert_equal(sessions, cases[i].management ? 1U : 0U);
		zassert_equal(frames, 5U, "waiting for entropy blocked the owner");
	}
}

ZTEST_SUITE(application, NULL, NULL, NULL, NULL, NULL);
