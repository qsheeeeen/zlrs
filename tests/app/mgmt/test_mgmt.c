/* SPDX-License-Identifier: Apache-2.0 */
#include "lifecycle.h"
#include "log.h"
#include "mgmt.h"
#include "persist.h"
#include "platform/slot_timer.h"
#include "rf.h"
#include <errno.h>
#include <string.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <zephyr/kernel.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>
static const zlrs_link_status_t initial_link = {
	.cfg = {.role = ZLRS_ROLE_LEADER,
		.region = ZLRS_REGION_ISM2400,
		.profile = ZLRS_PROFILE_P1,
		.tx_power_dbm = -9},
	.stats = {.last_rssi = -48, .tx_frames = 1024, .rx_frames = 1000, .tx_bytes = 12345},
	.peer_rssi_dbm = -51,
	.peer_lq_percent = 97,
	.fhss = {.region = &zlrs_regions[ZLRS_REGION_ISM2400],
		 .sequence = {[236] = 76},
		 .sequence_count = 237,
		 .index = 236},
};
static zlrs_link_status_t rf_link;
static const zlrs_rf_state_t initial_rf = {
	.link = &rf_link,
	.links_ready = true,
	.power = {.floor_dbm = -17, .ceiling_dbm = 12, .current_dbm = 3},
	.rate = {.wins = 16, .state = ZLRS_STATE_SEARCHING},
	.power_fixed = true,
	.frame_period_us = 9080,
	.temperature_centi_c = 2675,
	.hardware_sampled_ms = 17U,
};
static zlrs_rf_state_t rf;
static unsigned int credential_applies, pairs, radio_refreshes;
static int pair_rc;
static bool fail_write;
static size_t response_capacity = CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
static unsigned int watchdog_disarms;
static K_SEM_DEFINE(rollback_checked, 0, 1);
static atomic_t mutate_after_snapshot;
static k_tid_t snapshot_requester;

int __real_z_impl_k_sem_take(struct k_sem *sem, k_timeout_t timeout);

int __wrap_z_impl_k_sem_take(struct k_sem *sem, k_timeout_t timeout)
{
	const int rc = __real_z_impl_k_sem_take(sem, timeout);
	if (rc == 0 && atomic_get(&mutate_after_snapshot) &&
	    k_current_get() == snapshot_requester) {
		atomic_clear(&mutate_after_snapshot);
		rf_link.stats.tx_frames++;
		rf_link.state = ZLRS_STATE_SEARCHING;
		rf_link.fhss.sequence[236] = 0U;
		rf.hardware_sampled_ms = 9999U;
	}
	return rc;
}

void zlrs_lifecycle_check_rollback(bool fault, uint32_t timeout_ms)
{
	ARG_UNUSED(timeout_ms);
	zassert_false(fault);
	k_sem_give(&rollback_checked);
}

ssize_t __real_nvs_write(struct nvs_fs *, uint16_t, const void *, size_t);

ssize_t __wrap_nvs_write(struct nvs_fs *fs, uint16_t id, const void *bytes, size_t len)
{
	return fail_write ? -EIO : __real_nvs_write(fs, id, bytes, len);
}

const zlrs_rf_state_t *zlrs_rf_state(void)
{
	return &rf;
}

const zlrs_lr2021_diag_t *zlrs_lr2021_diag(void)
{
	static const zlrs_lr2021_diag_t diag = {.rx_crc_errors = 24};
	return &diag;
}

static bool rf_paused;

void zlrs_rf_pause(void)
{
	zassert_false(rf_paused);
	rf_paused = true;
}

void zlrs_rf_log_snapshot(void)
{
	zassert_true(rf_paused);
}

void zlrs_rf_resume(void)
{
	zassert_true(rf_paused);
	rf_paused = false;
}

uint32_t zlrs_slot_timer_now_ms(void)
{
	return k_uptime_get_32();
}

void zlrs_slot_timer_stats(struct zlrs_slot_timer_stats *out)
{
	*out = (struct zlrs_slot_timer_stats){.boundaries = 99U};
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
	const zlrs_persist_state_t *saved = zlrs_persist_state();
	rf_link.state = saved->configured
				? (saved->paired ? ZLRS_STATE_SEARCHING : ZLRS_STATE_UNPAIRED)
				: ZLRS_STATE_UNCONFIGURED;
	rf_link.paired = saved->paired;
	memcpy(rf_link.dev_id, saved->dev_id, sizeof(rf_link.dev_id));
}

int zlrs_rf_pair_start(void)
{
	pairs++;
	return rf_link.state != ZLRS_STATE_UNCONFIGURED ? pair_rc : -ENOTCONN;
}

void zlrs_watchdog_disarm(void)
{
	watchdog_disarms++;
}

void zlrs_watchdog_arm(void)
{
}

void zlrs_serial_publish_state(void)
{
}

static K_SEM_DEFINE(start_owner, 0, 1);

static void owner(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	k_sem_take(&start_owner, K_FOREVER);
	for (;;) {
		(void)zlrs_mgmt_process();
		k_msleep(1);
	}
}

K_THREAD_DEFINE(owner_tid, 4096, owner, NULL, NULL, NULL, 0, 0, 0);

static void *setup(void)
{
	zassert_ok(zlrs_persist_init());
	rf = initial_rf;
	rf_link = initial_link;
	k_sem_give(&start_owner);
	return NULL;
}

struct product_reply {
	int64_t view_value;
	bool has_view;
	int32_t rate_state;
	uint64_t uptime_ms, cpu_window_ms;
	int32_t cpu_load_permille;

	struct {
		uint32_t frequency_hz, tx_frames, hardware_sampled_ms, state;
		int16_t rssi_dbm;
	} status;
};

static int product_request(uint32_t op, const uint8_t *value, size_t len,
			   struct product_reply *reply)
{
	static uint8_t input[128], output[CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE];
	struct cbor_nb_reader reader = {0};
	struct cbor_nb_writer writer = {0};
	struct smp_streamer stream = {.reader = &reader, .writer = &writer};
	zcbor_state_t zs[6];
	const struct mgmt_handler *handler = mgmt_find_handler(66, 0);
	zcbor_new_encode_state(zs, ARRAY_SIZE(zs), input, sizeof(input), 0);
	zassert_true(zcbor_map_start_encode(zs, 2) && zcbor_tstr_put_lit(zs, "op") &&
		     zcbor_uint32_put(zs, op) && zcbor_tstr_put_lit(zs, "value") &&
		     zcbor_bstr_encode_ptr(zs, value, len) && zcbor_map_end_encode(zs, 2));
	zcbor_new_decode_state(reader.zs, ARRAY_SIZE(reader.zs), input, zs[0].payload - input, 1,
			       NULL, 0);
	zcbor_new_encode_state(writer.zs, ARRAY_SIZE(writer.zs), output, response_capacity, 0);
	zassert_true(zcbor_map_start_encode(writer.zs, 6));
	int rc = handler->mh_write(&stream);
	if (rc == MGMT_ERR_ENOTSUP) {
		return -ENOTSUP;
	}
	if (rc == MGMT_ERR_EMSGSIZE) {
		return -EMSGSIZE;
	}
	zassert_ok(rc);
	zassert_true(zcbor_map_end_encode(writer.zs, 6));
	zcbor_new_decode_state(zs, ARRAY_SIZE(zs), output, writer.zs[0].payload - output, 1, NULL,
			       0);
	zassert_true(zcbor_map_start_decode(zs) && zcbor_tstr_expect_lit(zs, "result") &&
		     zcbor_int32_decode(zs, &rc));
	if (rc != 0 || reply == NULL) {
		return rc;
	}
	if (op != ZLRS_MGMT_CMD_STATUS) {
		return rc;
	}
	zassert_true(zcbor_tstr_expect_lit(zs, "uptime_ms") &&
		     zcbor_uint64_decode(zs, &reply->uptime_ms) &&
		     zcbor_tstr_expect_lit(zs, "hardware_ms") &&
		     zcbor_uint32_decode(zs, &reply->status.hardware_sampled_ms) &&
		     zcbor_tstr_expect_lit(zs, "cpu_load_permille") &&
		     zcbor_int32_decode(zs, &reply->cpu_load_permille) &&
		     zcbor_tstr_expect_lit(zs, "cpu_window_ms") &&
		     zcbor_uint64_decode(zs, &reply->cpu_window_ms) &&
		     zcbor_tstr_expect_lit(zs, "status") && zcbor_map_start_decode(zs));
	static const char *const view_keys[] = {NULL, "tx_bytes", "windows", "boundaries",
						"dynamic_power_dbm"};
	const char *key = len == 1U ? view_keys[value[0]] : NULL;
	reply->has_view = false;
	reply->rate_state = -1;
	reply->status.state = UINT32_MAX;
	static struct zcbor_string seen[160];
	size_t count = 0U;
	while (!zcbor_array_at_end(zs)) {
		struct zcbor_string name;
		zassert_true(zcbor_tstr_decode(zs, &name));
		for (size_t i = 0; i < count; i++) {
			zassert_false(name.len == seen[i].len &&
					      memcmp(name.value, seen[i].value, name.len) == 0,
				      "duplicate status key");
		}
		zassert_true(count < ARRAY_SIZE(seen));
		seen[count++] = name;

		if (name.len == 5 && memcmp(name.value, "state", 5) == 0) {
			zassert_true(zcbor_uint32_decode(zs, &reply->status.state));
		} else if (name.len == 10 && memcmp(name.value, "rate_state", 10) == 0) {
			zassert_true(zcbor_int32_decode(zs, &reply->rate_state));
		} else if (name.len == 12 && memcmp(name.value, "frequency_hz", 12) == 0) {
			zassert_true(zcbor_uint32_decode(zs, &reply->status.frequency_hz));
		} else if (name.len == 9 && memcmp(name.value, "tx_frames", 9) == 0) {
			zassert_true(zcbor_uint32_decode(zs, &reply->status.tx_frames));
		} else if (name.len == 8 && memcmp(name.value, "rssi_dbm", 8) == 0) {
			int32_t rssi;
			zassert_true(zcbor_int32_decode(zs, &rssi));
			reply->status.rssi_dbm = rssi;
		} else if (key != NULL && name.len == strlen(key) &&
			   memcmp(name.value, key, name.len) == 0) {
			zassert_true(zcbor_int64_decode(zs, &reply->view_value));
			reply->has_view = true;
		} else {
			zassert_true(zcbor_any_skip(zs, NULL));
		}
	}
	zassert_true(zcbor_map_end_decode(zs) && zcbor_map_end_decode(zs));
	return rc;
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	fail_write = false;
	zassert_ok(product_request(ZLRS_MGMT_CMD_EXIT, NULL, 0, NULL));
	rf = initial_rf;
	rf_link = initial_link;
	credential_applies = pairs = radio_refreshes = watchdog_disarms = 0;
}

ZTEST(mgmt, test_online_snapshot_never_refreshes_radio)
{
	struct product_reply reply;
	for (int i = 0; i < 3; i++) {
		zassert_ok(product_request(ZLRS_MGMT_CMD_STATUS, NULL, 0, &reply));
		zassert_equal(reply.status.frequency_hz, 2478000000U);
		zassert_equal(reply.status.rssi_dbm, initial_link.stats.last_rssi);
		zassert_equal(reply.status.tx_frames, initial_link.stats.tx_frames);
		zassert_equal(reply.status.state, initial_link.state);
		zassert_equal(reply.status.hardware_sampled_ms, 17);
	}
	zassert_equal(radio_refreshes, 0);
	zassert_equal(watchdog_disarms, 0);
	uint8_t extra = 255;
	zassert_equal(product_request(ZLRS_MGMT_CMD_STATUS, &extra, 1, &reply), -EINVAL);
	zassert_equal(product_request(ZLRS_MGMT_CMD_PAIR, NULL, 0, NULL), -EACCES);
}

ZTEST(mgmt, test_status_wire_preserves_names_and_values)
{
	const uint8_t request_bytes[] = {0xa0}; /* Empty read defaults to STATUS. */
	uint8_t response[2048];
	struct cbor_nb_reader reader = {0};
	struct cbor_nb_writer writer = {0};
	struct smp_streamer stream = {.reader = &reader, .writer = &writer};
	const struct mgmt_handler *handler = mgmt_find_handler(66, 0);
	zassert_not_null(handler);
	zcbor_new_decode_state(reader.zs, ARRAY_SIZE(reader.zs), request_bytes,
			       sizeof(request_bytes), 1, NULL, 0);
	zcbor_new_encode_state(writer.zs, ARRAY_SIZE(writer.zs), response, sizeof(response), 0);
	zassert_true(zcbor_map_start_encode(writer.zs, 6));
	zassert_ok(handler->mh_read(&stream));
	zassert_true(zcbor_map_end_encode(writer.zs, 6));
	zcbor_state_t zs[6];
	zcbor_new_decode_state(zs, ARRAY_SIZE(zs), response, writer.zs[0].payload - response, 1,
			       NULL, 0);
	zassert_true(zcbor_map_start_decode(zs));
	zassert_true(zcbor_tstr_expect_lit(zs, "result") && zcbor_int32_expect(zs, 0));
	zassert_true(zcbor_tstr_expect_lit(zs, "uptime_ms") && zcbor_any_skip(zs, NULL));
	zassert_true(zcbor_tstr_expect_lit(zs, "hardware_ms") && zcbor_uint32_expect(zs, 17));
	int32_t cpu_load = 0;
	uint64_t cpu_window = 0;
	zassert_true(zcbor_tstr_expect_lit(zs, "cpu_load_permille") &&
		     zcbor_int32_decode(zs, &cpu_load));
	zassert_true(cpu_load >= 0 && cpu_load <= 1000);
	zassert_true(zcbor_tstr_expect_lit(zs, "cpu_window_ms") &&
		     zcbor_uint64_decode(zs, &cpu_window));
	zassert_true(cpu_window <= k_uptime_get());
	zassert_true(zcbor_tstr_expect_lit(zs, "status") && zcbor_map_start_decode(zs));

	const struct {
		const char *name;
		int64_t value;
	} expected[] = {
		{"flags", 0x80},
		{"active_region", ZLRS_REGION_ISM2400},
		{"regions", zlrs_region_supported_mask()},
		{"tx_power_dbm", 12},
		{"active_power_dbm", -9},
		{"rssi_dbm", -48},
		{"frequency_hz", 2478000000LL},
		{"region", zlrs_persist_state()->settings.region},
		{"data_baud", zlrs_persist_state()->settings.data_baud},
		{"radio_temperature_centi_c", 0},
		{"radio_irq", 0},
	};

	size_t matched = 0;
	while (!zcbor_array_at_end(zs)) {
		struct zcbor_string name;
		zassert_true(zcbor_tstr_decode(zs, &name));
		bool checked = false;
		for (size_t j = 0; j < ARRAY_SIZE(expected); j++) {
			if (name.len == strlen(expected[j].name) &&
			    memcmp(name.value, expected[j].name, name.len) == 0) {
				zassert_true(zcbor_int64_expect(zs, expected[j].value), "%s",
					     expected[j].name);
				matched++;
				checked = true;
				break;
			}
		}
		if (!checked) {
			zassert_true(zcbor_any_skip(zs, NULL));
		}
	}
	zassert_equal(matched, ARRAY_SIZE(expected));
	zassert_true(zcbor_map_end_decode(zs) && zcbor_map_end_decode(zs));
}

ZTEST(mgmt, test_cpu_window_uses_successful_status_samples)
{
	struct product_reply first, next;
	zassert_ok(product_request(ZLRS_MGMT_CMD_STATUS, NULL, 0, &first));
	k_sleep(K_MSEC(20));
	const uint8_t extra = 255;
	zassert_equal(product_request(ZLRS_MGMT_CMD_STATUS, &extra, 1, NULL), -EINVAL);
	k_sleep(K_MSEC(20));
	zassert_ok(product_request(ZLRS_MGMT_CMD_STATUS, NULL, 0, &next));
	zassert_equal(next.cpu_window_ms, next.uptime_ms - first.uptime_ms);
	zassert_true(next.cpu_window_ms >= 40);
	zassert_true(next.cpu_load_permille >= 0 && next.cpu_load_permille <= 1000);
	zassert_equal(radio_refreshes, 0);
	zassert_equal(watchdog_disarms, 0);
}

ZTEST(mgmt, test_status_response_owns_its_snapshot)
{
	struct product_reply reply;
	snapshot_requester = k_current_get();
	atomic_set(&mutate_after_snapshot, 1);
	zassert_ok(product_request(ZLRS_MGMT_CMD_STATUS, NULL, 0, &reply));
	zassert_false(atomic_get(&mutate_after_snapshot));
	zassert_equal(rf_link.stats.tx_frames, initial_link.stats.tx_frames + 1U);
	zassert_equal(rf.hardware_sampled_ms, 9999U);
	zassert_equal(reply.status.tx_frames, initial_link.stats.tx_frames);
	zassert_equal(reply.status.state, initial_link.state);
	zassert_equal(reply.status.hardware_sampled_ms, initial_rf.hardware_sampled_ms);
	zassert_equal(reply.status.frequency_hz, 2478000000U);
}

ZTEST(mgmt, test_enter_mutate_exit_owner_handoff)
{
	zassert_ok(product_request(ZLRS_MGMT_CMD_ENTER, NULL, 0, NULL));
	struct zlrs_mode mode;
	zassert_ok(zbus_chan_read(&zlrs_mode_chan, &mode, K_MSEC(10)));
	zassert_true(mode.session);
	zassert_equal(watchdog_disarms, 1);
	const uint8_t key[] = "4242424242424242424242424242424242424242424242424242424242424242";
	zassert_ok(product_request(ZLRS_MGMT_CMD_SET_KEY_HEX, key, sizeof(key) - 1, NULL));
	zassert_equal(credential_applies, 1);
	zassert_ok(product_request(ZLRS_MGMT_CMD_PAIR, NULL, 0, NULL));
	zassert_equal(pairs, 1);
	struct product_reply reply;
	zassert_ok(product_request(ZLRS_MGMT_CMD_STATUS, NULL, 0, &reply));
	zassert_equal(radio_refreshes, 1);
	zassert_ok(product_request(ZLRS_MGMT_CMD_EXIT, NULL, 0, NULL));
	zassert_ok(zbus_chan_read(&zlrs_mode_chan, &mode, K_MSEC(10)));
	zassert_false(mode.session);
	zassert_equal(product_request(ZLRS_MGMT_CMD_PAIR, NULL, 0, NULL), -EACCES);
}

ZTEST(mgmt, test_idle_management_keeps_checking_boot_deadline)
{
	zassert_ok(product_request(ZLRS_MGMT_CMD_ENTER, NULL, 0, NULL));
	/* Drain any initial check, then require another without a product command. */
	k_sem_reset(&rollback_checked);
	zassert_ok(k_sem_take(&rollback_checked, K_MSEC(100)));
	k_sem_reset(&rollback_checked);
	zassert_ok(k_sem_take(&rollback_checked, K_MSEC(100)));
	zassert_ok(product_request(ZLRS_MGMT_CMD_EXIT, NULL, 0, NULL));
}

#if defined(CONFIG_ZLRS_APP_DEBUG)
ZTEST(mgmt, test_status_views_are_online_and_share_unique_common_fields)
{
	const int64_t expected[] = {12345, 16, 99, 3};
	for (uint8_t view = ZLRS_STATUS_LINK; view <= ZLRS_STATUS_POWER; view++) {
		struct product_reply reply;
		zassert_ok(product_request(ZLRS_MGMT_CMD_STATUS, &view, 1U, &reply));
		zassert_true(reply.has_view);
		zassert_equal(reply.view_value, expected[view - ZLRS_STATUS_LINK]);
		zassert_equal(reply.status.frequency_hz, 2478000000U);
		zassert_equal(reply.status.tx_frames, initial_link.stats.tx_frames);
		zassert_equal(reply.status.state, initial_link.state);
		if (view == ZLRS_STATUS_RATE) {
			zassert_equal(reply.rate_state, initial_rf.rate.state);
		}
	}
	zassert_equal(radio_refreshes, 0);
	zassert_equal(watchdog_disarms, 0);
	uint8_t view = ZLRS_STATUS_RADIO;
	response_capacity = 64U;
	zassert_equal(product_request(ZLRS_MGMT_CMD_STATUS, &view, 1U, NULL), -EMSGSIZE);
	response_capacity = CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
	view = 255U;
	zassert_equal(product_request(ZLRS_MGMT_CMD_STATUS, &view, 1U, NULL), -EINVAL);
	const uint8_t extra[] = {ZLRS_STATUS_LINK, 0U};
	zassert_equal(product_request(ZLRS_MGMT_CMD_STATUS, extra, sizeof(extra), NULL), -EINVAL);
}
#else
ZTEST(mgmt, test_extended_status_views_are_unavailable_in_release)
{
	for (uint8_t view = ZLRS_STATUS_LINK; view <= ZLRS_STATUS_POWER; view++) {
		zassert_equal(product_request(ZLRS_MGMT_CMD_STATUS, &view, 1U, NULL), -EINVAL);
	}
	zassert_equal(watchdog_disarms, 0);
}
#endif

ZTEST(mgmt, test_invalid_and_failed_writes_do_not_apply_credentials)
{
	zassert_ok(product_request(ZLRS_MGMT_CMD_ENTER, NULL, 0, NULL));
	const uint8_t invalid = 0xff;
	zassert_equal(product_request(ZLRS_MGMT_CMD_SET_PROFILE, &invalid, 1, NULL), -EINVAL);
	uint8_t malformed_key[2 * ZLRS_KEY_LEN];
	memset(malformed_key, '0', sizeof(malformed_key));
	malformed_key[sizeof(malformed_key) - 1] = 'g';
	zassert_equal(product_request(ZLRS_MGMT_CMD_SET_KEY_HEX, malformed_key,
				      sizeof(malformed_key), NULL),
		      -EINVAL);
	zassert_equal(credential_applies, 0);
	const uint8_t key[] = "4242424242424242424242424242424242424242424242424242424242424242";
	fail_write = true;
	zassert_equal(product_request(ZLRS_MGMT_CMD_SET_KEY_HEX, key, sizeof(key) - 1, NULL), -EIO);
	fail_write = false;
	zassert_equal(credential_applies, 0);
	zassert_equal(product_request(255, NULL, 0, NULL), -ENOTSUP);
	zassert_ok(product_request(ZLRS_MGMT_CMD_EXIT, NULL, 0, NULL));
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
	zassert_ok(product_request(ZLRS_MGMT_CMD_EXIT, NULL, 0, NULL));
}

ZTEST_SUITE(mgmt, NULL, setup, before, NULL, NULL);
