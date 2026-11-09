/* SPDX-License-Identifier: Apache-2.0 */

#include "trace.h"
#include "mgmt.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tracing_backend.h>
#include <tracing_core.h>
#include <zcbor_encode.h>

#include <zephyr/app_version.h>
#include <zephyr/crypto/crypto.h>
#include <zephyr/data/json.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/timing/timing.h>
#include <zephyr/version.h>

#include "platform/sha256.h"
#include "platform/watchdog.h"
#include "region/region.h"
#include "rf.h"

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
static bool discovered;
static bool committed[2];
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(trace_fs);
static struct fs_mount_t trace_mount = {
	.type = FS_LITTLEFS,
	.fs_data = &trace_fs,
	.storage_dev = (void *)PARTITION_ID(trace_partition),
	.mnt_point = "/trace",
};

struct artifact {
	struct zlrs_trace_status capture;
	uint32_t capacity;
	uint32_t event_mask;
	const char *sha256;
	const char *build;
	const char *zephyr;
	uint32_t regions;
};

static const struct json_obj_descr artifact_fields[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct artifact, "generation", capture.generation,
				  JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct artifact, "capture_id", capture.id, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct artifact, "requested_ms", capture.duration_ms,
				  JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct artifact, "stop_reason", capture.reason, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct artifact, "core_dropped", capture.dropped,
				  JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct artifact, capacity, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct artifact, event_mask, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct artifact, "started_ns", capture.started_ns,
				  JSON_TOK_UINT64),
	JSON_OBJ_DESCR_PRIM_NAMED(struct artifact, "stopped_ns", capture.stopped_ns,
				  JSON_TOK_UINT64),
	JSON_OBJ_DESCR_PRIM(struct artifact, sha256, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct artifact, build, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct artifact, zephyr, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct artifact, regions, JSON_TOK_NUMBER),
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

void zlrs_trace_status(struct zlrs_trace_status *status)
{
	const unsigned int key = irq_lock();
	*status = capture;

	if (status->state == ZLRS_TRACE_CAPTURING) {
		status->dropped = tracing_packet_drop_count_get() - dropped_at_start;
	}
	irq_unlock(key);
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
		(void)bin2hex(digest, sizeof(digest), hex, 65);
	}
	return rc;
}

static void artifact_path(char *out, size_t capacity, unsigned int slot, const char *name)
{
	(void)snprintf(out, capacity, "/trace/%u/%s", slot, name);
}

int zlrs_trace_discover(void)
{
	capture.error = 0;
	if (discovered) {
		return 0;
	}
	const int rc = fs_mount(&trace_mount);
	if (rc != 0) {
		return capture.error = rc;
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
		    saved.capacity != sizeof(ram_tracing) || saved.capture.generation == 0U ||
		    strlen(saved.sha256) != 64U) {
			continue;
		}
		artifact_path(path, sizeof(path), slot, "channel0_0");
		if (file_digest(path, digest) != 0 || strcmp(digest, saved.sha256) != 0) {
			continue;
		}
		committed[slot] = true;
		if (saved.capture.generation > capture.generation) {
			capture.generation = saved.capture.generation;
			capture.slot = (int)slot;
		}
	}
	discovered = true;
	return 0;
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
	int rc = zlrs_trace_discover();

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
	(void)bin2hex(expected, sizeof(expected), expected_hex, sizeof(expected_hex));
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
		.capture = capture,
		.capacity = sizeof(ram_tracing),
		.event_mask = IS_ENABLED(CONFIG_TRACING_THREAD) |
			      (IS_ENABLED(CONFIG_TRACING_ISR) << 1) |
			      (IS_ENABLED(CONFIG_TRACING_SEMAPHORE) << 2) |
			      (IS_ENABLED(CONFIG_TRACING_IDLE) << 3) | BIT(4),
		.sha256 = digest,
		.build = APP_VERSION_STRING
		"-" STRINGIFY(APP_BUILD_VERSION),
			      .zephyr = KERNEL_VERSION_STRING
			      "-" STRINGIFY(BUILD_VERSION), .regions = zlrs_region_supported_mask(),
			      };

	saved.capture.generation++;
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
		capture.generation = saved.capture.generation;
		capture.slot = (int)slot;
	}
	return rc;
}

int zlrs_trace_prepare(enum zlrs_trace_command command, uint32_t duration_ms)
{
	if (command == ZLRS_TRACE_ARM) {
		if (!IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_TRACE_ROLLING)) {
			return capture.error = -ENOTSUP;
		}
		duration_ms = 0U;
	} else if (command != ZLRS_TRACE_START) {
		return 0;
	}
	if ((duration_ms < 10U || duration_ms > 5000U) &&
	    !(IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_TRACE_ROLLING) && duration_ms == 0U)) {
		return capture.error = -EINVAL;
	}
	const unsigned int key = irq_lock();
	const bool unavailable = maintenance_active || (capture.state != ZLRS_TRACE_IDLE &&
							capture.state != ZLRS_TRACE_SAVED);

	irq_unlock(key);
	if (unavailable) {
		return capture.error = -EBUSY;
	}
	k_timer_stop(&capture_timer);
	tracing_set_enabled(false);
	struct tracing_backend *backend = tracing_backend_get("tracing_backend_ram");

	if (backend == NULL) {
		return capture.error = -ENODEV;
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
}

int zlrs_trace_control(enum zlrs_trace_command command, uint32_t duration_ms, bool maintenance)
{
	int rc = 0;

	switch (command) {
	case ZLRS_TRACE_ARM:
	case ZLRS_TRACE_START:
		rc = start_capture(command == ZLRS_TRACE_ARM ? 0U : duration_ms, maintenance);
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
		if (capture.state != ZLRS_TRACE_FROZEN || zlrs_mgmt_fs_busy()) {
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
	return rc;
}

bool zlrs_trace_file_readable(const char *path)
{
	for (unsigned int slot = 0U; slot < 2U; slot++) {
		char name[48];
		if (!committed[slot]) {
			continue;
		}
		artifact_path(name, sizeof(name), slot, "channel0_0");
		if (strcmp(path, name) == 0) {
			return true;
		}
		artifact_path(name, sizeof(name), slot, "capture.json");
		if (strcmp(path, name) == 0) {
			return true;
		}
	}
	return false;
}

/* The SMP worker encodes one locked capture snapshot. */
int zlrs_trace_response(zcbor_state_t *zse, int result)
{
	struct zlrs_trace_status status;
	zlrs_trace_status(&status);
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
