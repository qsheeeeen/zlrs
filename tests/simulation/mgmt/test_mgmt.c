/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt_callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_callbacks.h>
#include "mgmt.h"
#include "boot.h"
#include "persist.h"
#include "rf.h"
static const zlrs_rf_status_t initial_rf = {
	.role = ZLRS_ROLE_LEADER,
	.profile = ZLRS_PROFILE_P1,
	.rssi_dbm = -48,
	.peer_rssi_dbm = -51,
	.peer_lq_percent = 97,
	.tx_power_dbm = -9,
	.power_floor_dbm = -17,
	.power_ceiling_dbm = 12,
	.power_fixed = true,
	.tx_frames = 1024,
	.rx_frames = 1000,
	.rx_crc_errors = 24,
	.frequency_hz = 2478000000U,
	.hop_index = 236,
	.frame_period_us = 9080,
	.temperature_centi_c = 2675,
	.hardware_sampled_ms = 17U,
};
static zlrs_rf_status_t rf;
static unsigned int credential_applies, pairs, radio_refreshes;
static int pair_rc;
static bool fail_write;
static unsigned int watchdog_disarms;
static K_SEM_DEFINE(rollback_checked, 0, 1);

void zlrs_boot_check_rollback(bool fault)
{
	zassert_false(fault);
	k_sem_give(&rollback_checked);
}

ssize_t __real_nvs_write(struct nvs_fs *, uint16_t, const void *, size_t);

ssize_t __wrap_nvs_write(struct nvs_fs *fs, uint16_t id, const void *bytes, size_t len)
{
	return fail_write ? -EIO : __real_nvs_write(fs, id, bytes, len);
}

zlrs_rf_status_t zlrs_rf_status(void)
{
	return rf;
}

void zlrs_rf_status_refresh(void)
{
	radio_refreshes++;
	rf.radio = (zlrs_lr2021_status_t){
		.temperature_centi_c = -125,
		.xosc_temperature_centi_c = 3125,
		.vbat_mv = 3301,
		.version_major = 1,
		.version_minor = 0x18,
		.chip_mode = 2,
		.command_status = 2,
		.reset_status = 1,
		.valid = 0x3F,
		.errors = 0x8401,
		.irq = 0xFEDCBA98U,
	};
}

void zlrs_rf_credentials(void)
{
	credential_applies++;
	uint8_t key[ZLRS_KEY_LEN];
	zlrs_persist_pairing_t pairing;
	rf.configured = zlrs_persist_get_root_key(key);
	rf.paired = zlrs_persist_get_pairing(&pairing);
	if (!zlrs_persist_get_device_id(rf.dev_id)) {
		memset(rf.dev_id, 0, sizeof(rf.dev_id));
	}
}

int zlrs_rf_pair_start(void)
{
	pairs++;
	return rf.configured ? pair_rc : -ENOTCONN;
}

void zlrs_watchdog_disarm(void)
{
	watchdog_disarms++;
}

void zlrs_watchdog_arm(void)
{
}

void zlrs_log_console_sync(void)
{
}

void zlrs_serial_publish_state(void)
{
}

int zlrs_log_info(zlrs_log_info_t *info)
{
	memset(info, 0, sizeof(*info));
	return 0;
}

static K_SEM_DEFINE(start_owner, 0, 1);

static void owner(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	k_sem_take(&start_owner, K_FOREVER);
	for (;;) {
		if (zlrs_mgmt_process_idle()) {
			zlrs_mgmt_run();
		}
		k_msleep(1);
	}
}

K_THREAD_DEFINE(owner_tid, 4096, owner, NULL, NULL, NULL, 0, 0, 0);

static void *setup(void)
{
	zassert_ok(zlrs_persist_init());
	rf = initial_rf;
	k_sem_give(&start_owner);
	return NULL;
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	fail_write = false;
	zassert_ok(zlrs_mgmt_request(ZLRS_MGMT_CMD_EXIT, NULL, 0, NULL));
	rf = initial_rf;
	credential_applies = pairs = radio_refreshes = watchdog_disarms = 0;
}

ZTEST(mgmt, test_online_snapshot_never_refreshes_radio)
{
	struct zlrs_mgmt_reply reply;
	for (int i = 0; i < 3; i++) {
		zassert_ok(zlrs_mgmt_request(ZLRS_MGMT_CMD_OBSERVE, NULL, 0, &reply));
		zassert_equal(reply.status.frequency_hz, initial_rf.frequency_hz);
		zassert_equal(reply.status.rssi_dbm, initial_rf.rssi_dbm);
		zassert_equal(reply.status.tx_frames, initial_rf.tx_frames);
		zassert_equal(reply.hardware_ms, 17);
	}
	zassert_equal(radio_refreshes, 0);
	zassert_equal(watchdog_disarms, 0);
	uint8_t extra = 1;
	zassert_equal(zlrs_mgmt_request(ZLRS_MGMT_CMD_OBSERVE, &extra, 1, &reply), -EINVAL);
	zassert_equal(zlrs_mgmt_request(ZLRS_MGMT_CMD_PAIR, NULL, 0, NULL), -EACCES);
}

ZTEST(mgmt, test_enter_mutate_exit_owner_handoff)
{
	zassert_ok(zlrs_mgmt_request(ZLRS_MGMT_CMD_ENTER, NULL, 0, NULL));
	struct zlrs_mode mode;
	zassert_ok(zbus_chan_read(&zlrs_mode_chan, &mode, K_MSEC(10)));
	zassert_true(mode.session);
	zassert_equal(watchdog_disarms, 1);
	const uint8_t key[] = "4242424242424242424242424242424242424242424242424242424242424242";
	zassert_ok(zlrs_mgmt_request(ZLRS_MGMT_CMD_SET_KEY_HEX, key, sizeof(key) - 1, NULL));
	zassert_equal(credential_applies, 1);
	zassert_ok(zlrs_mgmt_request(ZLRS_MGMT_CMD_PAIR, NULL, 0, NULL));
	zassert_equal(pairs, 1);
	struct zlrs_mgmt_reply reply;
	zassert_ok(zlrs_mgmt_request(ZLRS_MGMT_CMD_STATUS, NULL, 0, &reply));
	zassert_equal(radio_refreshes, 1);
	zassert_ok(zlrs_mgmt_request(ZLRS_MGMT_CMD_EXIT, NULL, 0, NULL));
	zassert_ok(zbus_chan_read(&zlrs_mode_chan, &mode, K_MSEC(10)));
	zassert_false(mode.session);
	zassert_equal(zlrs_mgmt_request(ZLRS_MGMT_CMD_PAIR, NULL, 0, NULL), -EACCES);
}

ZTEST(mgmt, test_idle_management_keeps_checking_boot_deadline)
{
	zassert_ok(zlrs_mgmt_request(ZLRS_MGMT_CMD_ENTER, NULL, 0, NULL));
	/* Drain any initial check, then require another without a product command. */
	k_sem_reset(&rollback_checked);
	zassert_ok(k_sem_take(&rollback_checked, K_MSEC(100)));
	k_sem_reset(&rollback_checked);
	zassert_ok(k_sem_take(&rollback_checked, K_MSEC(100)));
	zassert_ok(zlrs_mgmt_request(ZLRS_MGMT_CMD_EXIT, NULL, 0, NULL));
}

ZTEST(mgmt, test_invalid_and_failed_writes_do_not_apply_credentials)
{
	zassert_ok(zlrs_mgmt_request(ZLRS_MGMT_CMD_ENTER, NULL, 0, NULL));
	const uint8_t invalid = 0xff;
	zassert_equal(zlrs_mgmt_request(ZLRS_MGMT_CMD_SET_PROFILE, &invalid, 1, NULL), -EINVAL);
	const uint8_t key[] = "4242424242424242424242424242424242424242424242424242424242424242";
	fail_write = true;
	zassert_equal(zlrs_mgmt_request(ZLRS_MGMT_CMD_SET_KEY_HEX, key, sizeof(key) - 1, NULL),
		      -EIO);
	fail_write = false;
	zassert_equal(credential_applies, 0);
	zassert_equal(zlrs_mgmt_request(255, NULL, 0, NULL), -ENOTSUP);
	zassert_ok(zlrs_mgmt_request(ZLRS_MGMT_CMD_EXIT, NULL, 0, NULL));
}

static enum mgmt_cb_return notify(uint32_t event, void *data, size_t size, int expected)
{
	int32_t error = 0;
	uint16_t group = 0;
	const enum mgmt_cb_return result = mgmt_callback_notify(event, data, size, &error, &group);
	zassert_equal(error, expected, "event 0x%x: got %d, expected %d", event, error, expected);
	return result;
}

ZTEST(mgmt, test_native_img_fs_and_os_callback_groups)
{
	struct mgmt_evt_op_cmd_arg cmd = {.group = MGMT_GROUP_ID_IMAGE, .op = MGMT_OP_WRITE};
	zassert_equal(notify(MGMT_EVT_OP_CMD_RECV, &cmd, sizeof(cmd), MGMT_ERR_EACCESSDENIED),
		      MGMT_CB_ERROR_RC);
	cmd.op = MGMT_OP_READ;
	zassert_equal(notify(MGMT_EVT_OP_CMD_RECV, &cmd, sizeof(cmd), 0), MGMT_CB_OK);
	struct fs_mgmt_file_access file = {.filename = CONFIG_LOG_BACKEND_FS_DIR "/log.0000",
					   .access = FS_MGMT_FILE_ACCESS_READ};
	zassert_equal(notify(MGMT_EVT_OP_FS_MGMT_FILE_ACCESS, &file, sizeof(file),
			     MGMT_ERR_EACCESSDENIED),
		      MGMT_CB_ERROR_RC);
	struct os_mgmt_reset_data reset = {0};
	zassert_equal(notify(MGMT_EVT_OP_OS_MGMT_RESET, &reset, sizeof(reset), 0), MGMT_CB_OK);
	zassert_equal(watchdog_disarms, 1);
	zassert_equal(notify(MGMT_EVT_OP_FS_MGMT_FILE_ACCESS, &file, sizeof(file), 0), MGMT_CB_OK);
	file.access = FS_MGMT_FILE_ACCESS_WRITE;
	zassert_equal(notify(MGMT_EVT_OP_FS_MGMT_FILE_ACCESS, &file, sizeof(file),
			     MGMT_ERR_EACCESSDENIED),
		      MGMT_CB_ERROR_RC);
	file.access = FS_MGMT_FILE_ACCESS_READ;
	file.filename = "/lfs/../storage";
	zassert_equal(notify(MGMT_EVT_OP_FS_MGMT_FILE_ACCESS, &file, sizeof(file),
			     MGMT_ERR_EACCESSDENIED),
		      MGMT_CB_ERROR_RC);
	zassert_ok(zlrs_mgmt_request(ZLRS_MGMT_CMD_EXIT, NULL, 0, NULL));
}

ZTEST_SUITE(mgmt, NULL, setup, before, NULL, NULL);
