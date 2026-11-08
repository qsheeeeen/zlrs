/* SPDX-License-Identifier: Apache-2.0 */

#include "trace.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/app_version.h>
#include <zephyr/crypto/crypto.h>
#include <zephyr/data/json.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/timing/timing.h>
#include <zephyr/version.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <tracing_backend.h>
#include <tracing_core.h>

#include "rf.h"
#include "platform/watchdog.h"
#include "proto/sha256.h"

BUILD_ASSERT(!IS_ENABLED(CONFIG_SMP));
BUILD_ASSERT(IS_ENABLED(CONFIG_TRACING_SYNC) && IS_ENABLED(CONFIG_TRACING_BACKEND_RAM));
BUILD_ASSERT(IS_ENABLED(CONFIG_TRACING_HANDLE_HOST_CMD));
BUILD_ASSERT(!IS_ENABLED(CONFIG_TRACING_SHELL));
BUILD_ASSERT(!IS_ENABLED(CONFIG_SHELL));
BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_THREAD_PRIO >= 0);
BUILD_ASSERT(IS_ENABLED(CONFIG_MCUMGR_GRP_FS_FILE_ACCESS_HOOK));
BUILD_ASSERT(IS_ENABLED(CONFIG_MCUMGR_SMP_COMMAND_STATUS_HOOKS));

/* This is the same array exported by Zephyr's documented GDB RAM dump method. */
extern uint8_t ram_tracing[CONFIG_RAM_TRACING_BUFFER_SIZE];

enum {
	STOP_DEADLINE = 1,
	STOP_MANUAL,
	STOP_MAINTENANCE,
	STOP_TRIGGER
};
static struct zlrs_trace_status capture = {.slot = -1};
static uint32_t dropped_at_start;
#if defined(CONFIG_ZLRS_APP_DEBUG_TRACE_ROLLING)
static int64_t save_after_ms;
#endif
static bool maintenance_active;
static bool session_active;
static bool mounted;
static bool discovered;
static bool committed[2];
static atomic_t export_active;
static atomic_t export_file_open;
static atomic_t request_pending;
static enum zlrs_trace_command requested_command;
static uint32_t requested_duration;
static int request_result;
K_MUTEX_DEFINE(request_lock);
K_SEM_DEFINE(request_done, 0, 1);

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(trace_fs);
static struct fs_mount_t trace_mount = {
	.type = FS_LITTLEFS,
	.fs_data = &trace_fs,
	.storage_dev = (void *)PARTITION_ID(trace_partition),
	.mnt_point = "/trace",
};

struct artifact {
	uint32_t generation;
	uint32_t capture_id;
	uint32_t requested_ms;
	uint32_t stop_reason;
	uint32_t core_dropped;
	uint32_t capacity;
	uint32_t event_mask;
	uint64_t started_ns;
	uint64_t stopped_ns;
	const char *sha256;
	const char *build;
	const char *zephyr;
	uint32_t rf_band;
};

static const struct json_obj_descr artifact_fields[] = {
	JSON_OBJ_DESCR_PRIM(struct artifact, generation, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct artifact, capture_id, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct artifact, requested_ms, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct artifact, stop_reason, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct artifact, core_dropped, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct artifact, capacity, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct artifact, event_mask, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct artifact, started_ns, JSON_TOK_UINT64),
	JSON_OBJ_DESCR_PRIM(struct artifact, stopped_ns, JSON_TOK_UINT64),
	JSON_OBJ_DESCR_PRIM(struct artifact, sha256, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct artifact, build, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct artifact, zephyr, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct artifact, rf_band, JSON_TOK_NUMBER),
};

static void freeze(uint32_t reason)
{
	const unsigned int key = irq_lock();

	if (capture.state == ZLRS_TRACE_CAPTURING) {
		sys_trace_named_event("cap_end", capture.id, reason);
		tracing_set_enabled(false);
		capture.stopped_ns = timing_ns_get();
		capture.reason = reason;
		capture.dropped = tracing_packet_drop_count_get() - dropped_at_start;
		capture.state = ZLRS_TRACE_FROZEN;
	}
	irq_unlock(key);
}


static void deadline(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	freeze(STOP_DEADLINE);
}

K_TIMER_DEFINE(capture_timer, deadline, NULL);

#if defined(CONFIG_ZLRS_APP_DEBUG_TRACE_ROLLING)
void zlrs_trace_trigger(uint32_t sequence, uint32_t elapsed_us)
{
	if (capture.state == ZLRS_TRACE_CAPTURING) {
		ZLRS_TRACE_EVENT("latency_trigger", sequence, elapsed_us);
		freeze(STOP_TRIGGER);
		k_timer_stop(&capture_timer);
		save_after_ms = k_uptime_get() + 250;
	}
}
#endif


struct zlrs_trace_status zlrs_trace_status(void)
{
	const unsigned int key = irq_lock();
	struct zlrs_trace_status status = capture;

	if (status.state == ZLRS_TRACE_CAPTURING) {
		status.dropped = tracing_packet_drop_count_get() - dropped_at_start;
	}
	irq_unlock(key);
	return status;
}

void zlrs_trace_maintenance(bool entering)
{
	if (entering) {
		freeze(STOP_MAINTENANCE);
		k_timer_stop(&capture_timer);
	}
	const unsigned int key = irq_lock();

	maintenance_active = entering;
	irq_unlock(key);
}

bool zlrs_trace_export_busy(void)
{
	return atomic_get(&export_active) != 0 || atomic_get(&export_file_open) != 0;
}

/* Hash through the Zephyr crypto driver while RF is paused. This also verifies
 * file contents on remount without overwriting an unsaved RAM capture. */
static int file_digest(const char *path, char hex[65])
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(sha256));
	struct hash_ctx ctx = {.flags = CAP_SEPARATE_IO_BUFS | CAP_SYNC_OPS};
	struct fs_file_t file;
	uint8_t buffer[512];
	uint8_t digest[32];
	size_t total = 0;
	int rc;

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if (rc != 0) {
		return rc;
	}
	rc = hash_begin_session(dev, &ctx, CRYPTO_HASH_ALGO_SHA256);
	if (rc == 0) {
		for (;;) {
			const ssize_t got = fs_read(&file, buffer, sizeof(buffer));

			if (got < 0) {
				rc = (int)got;
				break;
			}
			if (got == 0) {
				struct hash_pkt pkt = {.out_buf = digest};

				rc = hash_compute(&ctx, &pkt);
				break;
			}
			total += (size_t)got;
			struct hash_pkt pkt = {.in_buf = buffer, .in_len = (size_t)got};

			rc = hash_update(&ctx, &pkt);
			if (rc != 0 || total > sizeof(ram_tracing)) {
				rc = rc != 0 ? rc : -EFBIG;
				break;
			}
			zlrs_watchdog_feed();
		}
		const int free_rc = hash_free_session(dev, &ctx);

		if (rc == 0) {
			rc = free_rc;
		}
	}
	const int close_rc = fs_close(&file);

	if (rc == 0) {
		rc = close_rc;
	}
	if (rc == 0 && total != sizeof(ram_tracing)) {
		rc = -EBADMSG;
	}
	if (rc == 0) {
		static const char digits[] = "0123456789abcdef";

		for (size_t i = 0; i < sizeof(digest); i++) {
			hex[2U * i] = digits[digest[i] >> 4];
			hex[2U * i + 1U] = digits[digest[i] & 15U];
		}
		hex[64] = '\0';
	}
	return rc;
}

static void artifact_path(char *out, size_t capacity, unsigned int slot, const char *name)
{
	(void)snprintf(out, capacity, "/trace/%u/%s", slot, name);
}

static int discover(void)
{
	if (!mounted) {
		const int rc = fs_mount(&trace_mount);

		if (rc != 0) {
			return rc;
		}
		mounted = true;
	}
	if (discovered) {
		return 0;
	}
	for (unsigned int slot = 0; slot < 2U; slot++) {
		char path[48];
		char json[1024];
		char digest[65];
		struct artifact saved = {0};
		struct fs_file_t file;

		artifact_path(path, sizeof(path), slot, "capture.json");
		fs_file_t_init(&file);
		if (fs_open(&file, path, FS_O_READ) != 0) {
			continue;
		}
		const ssize_t got = fs_read(&file, json, sizeof(json) - 1U);
		const int close_rc = fs_close(&file);

		if (got <= 0 || got >= sizeof(json) - 1U || close_rc != 0) {
			continue;
		}
		json[got] = '\0';
		const int64_t fields = json_obj_parse(json, got, artifact_fields,
						      ARRAY_SIZE(artifact_fields), &saved);

		if (fields != BIT_MASK(ARRAY_SIZE(artifact_fields)) ||
		    saved.capacity != sizeof(ram_tracing) || saved.generation == 0U ||
		    strlen(saved.sha256) != 64U) {
			continue;
		}
		artifact_path(path, sizeof(path), slot, "channel0_0");
		if (file_digest(path, digest) != 0 || strcmp(digest, saved.sha256) != 0) {
			continue;
		}
		committed[slot] = true;
		if (saved.generation > capture.generation) {
			capture.generation = saved.generation;
			capture.slot = (int)slot;
		}
	}
	discovered = true;
	return 0;
}

static void export_barrier(struct k_work *work)
{
	ARG_UNUSED(work);
}

K_WORK_DEFINE(export_close_barrier, export_barrier);

void zlrs_trace_session(bool entering)
{
	if (entering) {
		capture.error = discover();
		const unsigned int key = irq_lock();

		session_active = true;
		irq_unlock(key);
		return;
	}
	const unsigned int key = irq_lock();

	session_active = false;
	irq_unlock(key);
	while (atomic_get(&export_active) != 0) {
		k_msleep(1);
	}
	/* The filesystem group's idle closer uses the system workqueue. Close
	 * abandoned read handles before RF resumes, after denying new requests. */
	if (atomic_get(&export_file_open) != 0) {
		struct k_work_sync sync;

		k_msleep(CONFIG_MCUMGR_GRP_FS_FILE_AUTOMATIC_IDLE_CLOSE_TIME + 1);
		(void)k_work_submit(&export_close_barrier);
		(void)k_work_flush(&export_close_barrier, &sync);
		atomic_set(&export_file_open, 0);
	}
}

static int write_file(const char *path, const void *bytes, size_t size)
{
	struct fs_file_t file;
	const uint8_t *cursor = bytes;

	fs_file_t_init(&file);
	int rc = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);

	if (rc != 0) {
		return rc;
	}
	while (size > 0U) {
		const ssize_t written = fs_write(&file, cursor, MIN(size, 1024U));

		if (written <= 0) {
			rc = written != 0 ? (int)written : -EIO;
			break;
		}
		cursor += written;
		size -= (size_t)written;
		zlrs_watchdog_feed();
	}
	if (rc == 0) {
		rc = fs_sync(&file);
	}
	const int close_rc = fs_close(&file);

	return rc != 0 ? rc : close_rc;
}

static int save_capture(void)
{
	int rc = discover();

	if (rc != 0) {
		return rc;
	}
	if (capture.generation >= INT32_MAX) {
		return -EOVERFLOW;
	}
	const unsigned int slot = capture.slot == 0 ? 1U : 0U;
	char path[48];
	char digest[65];
	uint8_t expected[ZLRS_SHA256_LEN];
	char expected_hex[65];
	char json[1024];

	(void)snprintf(path, sizeof(path), "/trace/%u", slot);
	rc = fs_mkdir(path);
	if (rc != 0 && rc != -EEXIST) {
		return rc;
	}
	committed[slot] = false;
	artifact_path(path, sizeof(path), slot, "capture.json");
	rc = fs_unlink(path);
	if (rc != 0 && rc != -ENOENT) {
		return rc;
	}
	artifact_path(path, sizeof(path), slot, "channel0_0");
	/* This inactive slot is already uncommitted. Release its old data before
	 * writing, rather than requiring LittleFS space for a third full snapshot. */
	rc = fs_unlink(path);
	if (rc != 0 && rc != -ENOENT) {
		return rc;
	}
	rc = tracing_backend_flush(tracing_backend_get("tracing_backend_ram"));
	if (rc != 0) {
		return rc;
	}
	zlrs_sha256(ram_tracing, sizeof(ram_tracing), NULL, 0U, expected);
	for (size_t i = 0U; i < sizeof(expected); i++) {
		(void)snprintf(&expected_hex[2U * i], 3U, "%02x", expected[i]);
	}
	rc = write_file(path, ram_tracing, sizeof(ram_tracing));
	if (rc == 0) {
		rc = file_digest(path, digest);
	}
	if (rc != 0) {
		return rc;
	}
	if (strcmp(digest, expected_hex) != 0) {
		return -EBADMSG;
	}
	struct artifact saved = {
		.generation = capture.generation + 1U,
		.capture_id = capture.id,
		.requested_ms = capture.duration_ms,
		.stop_reason = capture.reason,
		.core_dropped = capture.dropped,
		.capacity = sizeof(ram_tracing),
		.event_mask = IS_ENABLED(CONFIG_TRACING_THREAD) |
			      (IS_ENABLED(CONFIG_TRACING_ISR) << 1) |
			      (IS_ENABLED(CONFIG_TRACING_SEMAPHORE) << 2) |
			      (IS_ENABLED(CONFIG_TRACING_IDLE) << 3) | BIT(4),
		.started_ns = capture.started_ns,
		.stopped_ns = capture.stopped_ns,
		.sha256 = digest,
		.build = APP_VERSION_STRING
		"-" STRINGIFY(APP_BUILD_VERSION),
			      .zephyr = KERNEL_VERSION_STRING
			      "-" STRINGIFY(BUILD_VERSION),
					    .rf_band = DT_PROP(DT_NODELABEL(zlrs_rf), band),
			      };

	rc = json_obj_encode_buf(artifact_fields, ARRAY_SIZE(artifact_fields), &saved, json,
				 sizeof(json));
	if (rc == 0) {
		artifact_path(path, sizeof(path), slot, "capture.tmp");
		rc = write_file(path, json, strlen(json));
	}
	if (rc == 0) {
		char final_path[48];

		artifact_path(final_path, sizeof(final_path), slot, "capture.json");
		rc = fs_rename(path, final_path);
	}
	if (rc == 0) {
		committed[slot] = true;
		capture.generation = saved.generation;
		capture.slot = (int)slot;
	}
	return rc;
}

static int reset_capture(uint32_t duration_ms)
{
	if ((duration_ms < 10U || duration_ms > 5000U) &&
	    !(IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_TRACE_ROLLING) && duration_ms == 0U)) {
		return -EINVAL;
	}
	const unsigned int key = irq_lock();
	const bool unavailable = maintenance_active || (capture.state != ZLRS_TRACE_IDLE &&
							capture.state != ZLRS_TRACE_SAVED);

	irq_unlock(key);
	if (unavailable) {
		return -EBUSY;
	}
	k_timer_stop(&capture_timer);
	tracing_set_enabled(false);
	struct tracing_backend *backend = tracing_backend_get("tracing_backend_ram");

	if (backend == NULL) {
		return -ENODEV;
	}
	/* The MCUmgr worker can be preempted by RF. Clearing 64 KiB on the
	 * cooperative RF owner would delay its next slot before recording begins.
	 * request_lock excludes another start/save until the owner acknowledges.
	 */
	tracing_backend_init(backend);
	return 0;
}

static int start_capture(uint32_t duration_ms, bool maintenance)
{
	if (maintenance ||
	    (capture.state != ZLRS_TRACE_IDLE && capture.state != ZLRS_TRACE_SAVED)) {
		return -EBUSY;
	}
	const unsigned int key = irq_lock();

	capture.id++;
#if defined(CONFIG_ZLRS_APP_DEBUG_TRACE_ROLLING)
	save_after_ms = 0;
#endif
	capture.duration_ms = duration_ms;
	capture.reason = 0U;
	capture.error = 0;
	capture.stopped_ns = 0U;
	capture.started_ns = timing_ns_get();
	dropped_at_start = tracing_packet_drop_count_get();
	capture.dropped = 0U;
	capture.state = ZLRS_TRACE_CAPTURING;
	tracing_set_enabled(true);
	sys_trace_named_event("cap_begin", capture.id, duration_ms);
	if (duration_ms != 0U) {
		k_timer_start(&capture_timer, K_MSEC(duration_ms), K_NO_WAIT);
	}
	irq_unlock(key);
	return 0;
}

int zlrs_trace_request(enum zlrs_trace_command command, uint32_t duration_ms)
{
	if (command == ZLRS_TRACE_ARM) {
		if (!IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_TRACE_ROLLING)) {
			return -ENOTSUP;
		}
		duration_ms = 0U;
	}
	int rc = k_mutex_lock(&request_lock, K_FOREVER);

	if (rc != 0) {
		return rc;
	}
	if (command == ZLRS_TRACE_START || command == ZLRS_TRACE_ARM) {
		rc = reset_capture(duration_ms);
		if (rc != 0) {
			capture.error = rc;
			k_mutex_unlock(&request_lock);
			return rc;
		}
	}
	requested_command = command;
	requested_duration = duration_ms;
	atomic_set(&request_pending, 1);
	(void)k_sem_take(&request_done, K_FOREVER);
	rc = request_result;
	k_mutex_unlock(&request_lock);
	return rc;
}

static int save_frozen_capture(bool maintenance)
{
	capture.state = ZLRS_TRACE_SAVING;
	if (!maintenance) {
		zlrs_rf_pause();
	}
	int rc = save_capture();
	if (!maintenance) {
		zlrs_rf_resume();
	}
	capture.state = rc == 0 ? ZLRS_TRACE_SAVED : ZLRS_TRACE_FROZEN;
	return rc;
}

void zlrs_trace_process(bool maintenance)
{
#if defined(CONFIG_ZLRS_APP_DEBUG_TRACE_ROLLING)
	if (save_after_ms != 0 && k_uptime_get() >= save_after_ms) {
		save_after_ms = 0;
		if (capture.state == ZLRS_TRACE_FROZEN) {
			capture.error = save_frozen_capture(maintenance);
		}
	}
#endif
	if (!atomic_cas(&request_pending, 1, 0)) {
		return;
	}
	int rc = 0;

	switch (requested_command) {
	case ZLRS_TRACE_ARM:
	case ZLRS_TRACE_START:
		rc = start_capture(requested_duration, maintenance);
		break;
	case ZLRS_TRACE_STOP:
		if (capture.state != ZLRS_TRACE_CAPTURING && capture.state != ZLRS_TRACE_FROZEN) {
			rc = -EINVAL;
			break;
		}
		freeze(STOP_MANUAL);
		k_timer_stop(&capture_timer);
		break;
	case ZLRS_TRACE_DISCARD:
		if (capture.state != ZLRS_TRACE_FROZEN && capture.state != ZLRS_TRACE_SAVED) {
			rc = -EBUSY;
		} else {
			capture.state = ZLRS_TRACE_IDLE;
		}
		break;
	case ZLRS_TRACE_SAVE:
		if (capture.state != ZLRS_TRACE_FROZEN || zlrs_trace_export_busy()) {
			rc = -EBUSY;
			break;
		}
		rc = save_frozen_capture(maintenance);
		break;
	default:
		rc = -EINVAL;
		break;
	}
	capture.error = rc;
	request_result = rc;
	k_sem_give(&request_done);
}

static enum mgmt_cb_return smp_access(uint32_t event, enum mgmt_cb_return previous, int32_t *rc,
				      uint16_t *group, bool *abort_more, void *data, size_t size)
{
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(size);
	struct mgmt_evt_op_cmd_arg *cmd = data;

	if (cmd->group != MGMT_GROUP_ID_FS) {
		return MGMT_CB_OK;
	}
	if (event == MGMT_EVT_OP_CMD_DONE) {
		atomic_set(&export_active, 0);
		if (cmd->id == FS_MGMT_ID_OPENED_FILE && cmd->err == 0) {
			atomic_set(&export_file_open, 0);
		}
		return MGMT_CB_OK;
	}
	if (previous != MGMT_CB_OK) {
		return previous;
	}
	const unsigned int key = irq_lock();
	const bool allowed = maintenance_active && session_active && mounted &&
			     capture.state != ZLRS_TRACE_SAVING &&
			     capture.state != ZLRS_TRACE_CAPTURING;

	if (allowed) {
		atomic_set(&export_active, 1);
	}
	irq_unlock(key);
	if (!allowed) {
		*rc = MGMT_ERR_EBUSY;
		return MGMT_CB_ERROR_RC;
	}
	return MGMT_CB_OK;
}

static enum mgmt_cb_return file_access(uint32_t event, enum mgmt_cb_return previous, int32_t *rc,
				       uint16_t *group, bool *abort_more, void *data, size_t size)
{
	ARG_UNUSED(event);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(size);
	struct fs_mgmt_file_access *access = data;
	bool allowed = false;

	if (event == MGMT_EVT_OP_FS_MGMT_FILE_ACCESS_DONE) {
		atomic_set(&export_file_open, 0);
		return MGMT_CB_OK;
	}
	if (previous != MGMT_CB_OK) {
		return previous;
	}
	if (strncmp(access->filename, "/trace/", 7) != 0) {
		return MGMT_CB_OK;
	}
	for (unsigned int slot = 0; slot < 2U; slot++) {
		char path[48];

		if (!committed[slot]) {
			continue;
		}
		artifact_path(path, sizeof(path), slot, "channel0_0");
		allowed |= strcmp(access->filename, path) == 0;
		artifact_path(path, sizeof(path), slot, "capture.json");
		allowed |= strcmp(access->filename, path) == 0;
	}
	if (!allowed || access->access == FS_MGMT_FILE_ACCESS_WRITE) {
		*rc = MGMT_ERR_EACCESSDENIED;
		return MGMT_CB_ERROR_RC;
	}
	if (access->access == FS_MGMT_FILE_ACCESS_READ) {
		atomic_set(&export_file_open, 1);
	}
	return MGMT_CB_OK;
}

static struct mgmt_callback smp_callback = {
	.callback = smp_access,
	.event_id = MGMT_EVT_OP_CMD_RECV | MGMT_EVT_OP_CMD_DONE,
};
static struct mgmt_callback file_callback = {
	.callback = file_access,
	.event_id = MGMT_EVT_OP_FS_MGMT_FILE_ACCESS | MGMT_EVT_OP_FS_MGMT_FILE_ACCESS_DONE,
};

/* SMP group 64, command 0: capture control; rolling ARM is fixture-only. */
static int trace_response(struct smp_streamer *ctxt, int result)
{
	const struct zlrs_trace_status status = zlrs_trace_status();
	zcbor_state_t *zse = ctxt->writer->zs;
	const bool ok =
		zcbor_tstr_put_lit(zse, "result") && zcbor_int32_put(zse, result) &&
		zcbor_tstr_put_lit(zse, "state") && zcbor_uint32_put(zse, status.state) &&
		zcbor_tstr_put_lit(zse, "id") && zcbor_uint32_put(zse, status.id) &&
		zcbor_tstr_put_lit(zse, "requested_ms") &&
		zcbor_uint32_put(zse, status.duration_ms) &&
		zcbor_tstr_put_lit(zse, "started_ns") && zcbor_uint64_put(zse, status.started_ns) &&
		zcbor_tstr_put_lit(zse, "stopped_ns") && zcbor_uint64_put(zse, status.stopped_ns) &&
		zcbor_tstr_put_lit(zse, "reason") && zcbor_uint32_put(zse, status.reason) &&
		zcbor_tstr_put_lit(zse, "dropped") && zcbor_uint32_put(zse, status.dropped) &&
		zcbor_tstr_put_lit(zse, "generation") && zcbor_uint32_put(zse, status.generation) &&
		zcbor_tstr_put_lit(zse, "slot") && zcbor_int32_put(zse, status.slot) &&
		zcbor_tstr_put_lit(zse, "error") && zcbor_int32_put(zse, status.error) &&
		zcbor_tstr_put_lit(zse, "capacity") &&
		zcbor_uint32_put(zse, CONFIG_RAM_TRACING_BUFFER_SIZE);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int trace_read(struct smp_streamer *ctxt)
{
	return trace_response(ctxt, 0);
}

static int trace_write(struct smp_streamer *ctxt)
{
	uint32_t operation = UINT32_MAX;
	uint32_t duration_ms = 50U;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("op", zcbor_uint32_decode, &operation),
		ZCBOR_MAP_DECODE_KEY_DECODER("ms", zcbor_uint32_decode, &duration_ms),
	};

	if (zcbor_map_decode_bulk(ctxt->reader->zs, fields, ARRAY_SIZE(fields), &decoded) != 0 ||
	    operation > ZLRS_TRACE_ARM) {
		return MGMT_ERR_EINVAL;
	}
	return trace_response(ctxt, zlrs_trace_request(operation, duration_ms));
}

static const struct mgmt_handler trace_handlers[] = {
	{.mh_read = trace_read, .mh_write = trace_write},
};
static struct mgmt_group trace_group = {
	.mg_handlers = trace_handlers,
	.mg_handlers_count = ARRAY_SIZE(trace_handlers),
	.mg_group_id = 64,
};

static int trace_init(void)
{
	mgmt_register_group(&trace_group);
	mgmt_callback_register(&smp_callback);
	mgmt_callback_register(&file_callback);
	return 0;
}

SYS_INIT(trace_init, APPLICATION, 90);
