/* SPDX-License-Identifier: Apache-2.0 */
/* Product operations over native MCUmgr SMP. All RF and persistence work stays on its owner. */
#include "mgmt.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#if defined(CONFIG_MCUMGR_GRP_OS_INFO_CUSTOM_HOOKS)
#include <zephyr/app_version.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt.h>
#endif
#include "lifecycle.h"
#include "log.h"
#include "persist.h"
#include "platform/watchdog.h"
#include "proto/crypto.h"
#include "rf.h"
#include "serial.h"
#include "trace.h"
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include <mgmt/mcumgr/util/zcbor_bulk.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/cpu_load.h>
#include <zephyr/sys/util.h>
#if defined(CONFIG_ZLRS_APP_DEBUG)
#include "diagnostics.h"
#endif

BUILD_ASSERT(DT_SAME_NODE(DT_CHOSEN(zephyr_uart_mcumgr), DT_ALIAS(zlrs_mgmt_cdc)),
	     "Native SMP must use the management CDC, not the DATA or log UART");
BUILD_ASSERT(IS_ENABLED(CONFIG_MCUMGR_TRANSPORT_UART),
	     "Product management requires the native SMP UART transport");
BUILD_ASSERT(IS_ENABLED(CONFIG_MCUMGR_GRP_FS_DL_CHUNK_SIZE_LIMIT),
	     "FS downloads must not allocate the entire SMP netbuf on the worker stack");
LOG_MODULE_REGISTER(zlrs_mgmt, CONFIG_APP_LOG_LEVEL);
ZBUS_CHAN_DEFINE(zlrs_mode_chan, struct zlrs_mode, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
static struct zlrs_mode mode;
static bool mode_pending;
static atomic_t session_active;
static atomic_t last_activity;
static K_MUTEX_DEFINE(request_lock);
static K_SEM_DEFINE(request_pending, 0, 1);
static K_SEM_DEFINE(request_done, 0, 1);

static struct {
	uint32_t op;
	uint8_t value[64];
	size_t len;
	int result;
	zcbor_state_t *writer;

	struct {
		uint64_t uptime_ms, cpu_window_ms;
		int32_t cpu_load_permille;
		const char *report;
		zlrs_log_info_t logs;
	} reply;
} request;

static void publish_mode(void)
{
	if (mode_pending && zbus_chan_pub(&zlrs_mode_chan, &mode, K_NO_WAIT) == 0) {
		mode_pending = false;
	}
}

static void mgmt_log_flush(void)
{
	zlrs_log_console_sync();
	log_flush();

	/* With no log thread, log_flush() drains messages but does not emit its completion
	 * event. Notify the backends here so the file backend synchronizes before ENTER ACK. */
	if (!IS_ENABLED(CONFIG_LOG_PROCESS_THREAD)) {
		STRUCT_SECTION_FOREACH(log_backend, backend) {
			if (log_backend_is_active(backend)) {
				log_backend_notify(backend, LOG_BACKEND_EVT_PROCESS_THREAD_DONE,
						   NULL);
			}
		}
	}
}

#define ZLRS_MGMT_FLAG_POWER_FIXED    0x80U /* volatile debug override */
#define ZLRS_MGMT_FLAG_KEY            0x01U
#define ZLRS_MGMT_FLAG_PAIRED         0x02U
#define ZLRS_MGMT_FLAG_SESSION        0x04U
#define ZLRS_MGMT_FLAG_DATA_SUBG      0x08U /* DATA currently uses sub-GHz */
#define ZLRS_MGMT_FLAG_BAND_AUTO      0x10U
#define ZLRS_MGMT_FLAG_BAND_ACQUIRING 0x20U
#define ZLRS_MGMT_FLAG_BAND_PLANNED   0x40U

static uint8_t status_flags(const zlrs_rf_state_t *rf)
{
	return (rf->link->cfg.region != ZLRS_REGION_ISM2400 ? ZLRS_MGMT_FLAG_DATA_SUBG : 0U) |
	       (rf->band_mode == ZLRS_DATA_BAND_AUTO ? ZLRS_MGMT_FLAG_BAND_AUTO : 0U) |
	       (rf->link->radio_acquiring ? ZLRS_MGMT_FLAG_BAND_ACQUIRING : 0U) |
	       (rf->power_fixed ? ZLRS_MGMT_FLAG_POWER_FIXED : 0U) |
	       (rf->link->band_plan.switch_frame != 0U ? ZLRS_MGMT_FLAG_BAND_PLANNED : 0U) |
	       (rf->link->configured ? ZLRS_MGMT_FLAG_KEY : 0U) |
	       (rf->link->paired ? ZLRS_MGMT_FLAG_PAIRED : 0U) |
	       (rf->link->session_valid ? ZLRS_MGMT_FLAG_SESSION : 0U);
}

static int operation(bool maintenance)
{
	__typeof__(request.reply) *out = &request.reply;
	const uint8_t *value = request.value;
	const size_t len = request.len;
	const uint32_t op = request.op;
	memset(out, 0, sizeof(*out));
	if (op == ZLRS_MGMT_CMD_STATUS || op == ZLRS_MGMT_CMD_OBSERVE) {
		static uint64_t cpu_sampled_ms;
		if (len != 0) {
			return -EINVAL;
		}
		if (maintenance) {
			zlrs_rf_status_refresh();
		}
		/* Shared STATUS/OBSERVE window; the first sample measures since boot. */
		out->uptime_ms = k_uptime_get();
		out->cpu_load_permille = cpu_load_get(true);
		out->cpu_window_ms = out->uptime_ms - cpu_sampled_ms;
		cpu_sampled_ms = out->uptime_ms;
		return 0;
	}
	if (!maintenance) {
		return op == ZLRS_MGMT_CMD_EXIT ? 0 : -EACCES;
	}
	if (len != 0 &&
	    (op == ZLRS_MGMT_CMD_ENTER || op == ZLRS_MGMT_CMD_EXIT || op == ZLRS_MGMT_CMD_PAIR ||
	     op == ZLRS_MGMT_CMD_NEW_DEV_ID || op == ZLRS_MGMT_CMD_ERASE_PAIR ||
	     op == ZLRS_MGMT_CMD_FACTORY_RESET || op == ZLRS_MGMT_CMD_LOG_INFO)) {
		return -EINVAL;
	}

	switch (op) {
	case ZLRS_MGMT_CMD_ENTER:
		mgmt_log_flush();
		return 0;
	case ZLRS_MGMT_CMD_EXIT:
		return 0;
	case ZLRS_MGMT_CMD_PAIR:
		return zlrs_rf_pair_start();
	case ZLRS_MGMT_CMD_NEW_DEV_ID:
	case ZLRS_MGMT_CMD_FACTORY_RESET: {
		const int rc = op == ZLRS_MGMT_CMD_NEW_DEV_ID ? zlrs_persist_new_identity()
							      : zlrs_persist_factory_reset();
		zlrs_rf_credentials();
		return rc;
	}
	case ZLRS_MGMT_CMD_ERASE_PAIR: {
		const int rc = zlrs_persist_erase_pairing();
		if (rc == 0) {
			zlrs_rf_credentials();
		}
		return rc;
	}

	case ZLRS_MGMT_CMD_SET_KEY_HEX:
	case ZLRS_MGMT_CMD_SET_PHRASE: {
		uint8_t key[ZLRS_KEY_LEN];
		if (op == ZLRS_MGMT_CMD_SET_KEY_HEX) {
			if (len != 2U * ZLRS_KEY_LEN) {
				return -EINVAL;
			}
			if (hex2bin((const char *)value, len, key, sizeof(key)) != sizeof(key)) {
				memset(key, 0, sizeof(key));
				return -EINVAL;
			}
		} else {
			if (len == 0 || len > 32) {
				return -EINVAL;
			}
			char phrase[33];
			memcpy(phrase, value, len);
			phrase[len] = '\0';
			zlrs_pbkdf2_sha256(phrase, (const uint8_t *)ZLRS_KDF_V1_SALT,
					   sizeof(ZLRS_KDF_V1_SALT) - 1U, ZLRS_KDF_V1_ITERATIONS,
					   key);
			memset(phrase, 0, sizeof(phrase));
		}
		const int rc = zlrs_persist_set_root_key(key);
		memset(key, 0, sizeof(key));
		if (rc == 0) {
			zlrs_rf_credentials();
		}
		return rc;
	}
	case ZLRS_MGMT_CMD_SET_BAND:
	case ZLRS_MGMT_CMD_SET_BAUD:
	case ZLRS_MGMT_CMD_SET_REGION:
	case ZLRS_MGMT_CMD_SET_ROLE:
	case ZLRS_MGMT_CMD_SET_PROFILE: {
		if (len != (op == ZLRS_MGMT_CMD_SET_BAUD ? 4U : 1U)) {
			return -EINVAL;
		}
		zlrs_persist_settings_t settings = zlrs_persist_state()->settings;
		switch (op) {
		case ZLRS_MGMT_CMD_SET_BAND:
			settings.band = value[0];
			break;
		case ZLRS_MGMT_CMD_SET_BAUD:
			settings.data_baud = sys_get_le32(value);
			break;
		case ZLRS_MGMT_CMD_SET_REGION:
			settings.region = value[0];
			break;
		case ZLRS_MGMT_CMD_SET_ROLE:
			settings.role = value[0];
			break;
		case ZLRS_MGMT_CMD_SET_PROFILE:
			settings.profile = value[0];
			break;
		}
		return zlrs_persist_save_settings(&settings);
	}
	case ZLRS_MGMT_CMD_LOG_INFO:
		return zlrs_log_info(&out->logs);
#if defined(CONFIG_ZLRS_APP_DEBUG)
	case ZLRS_MGMT_CMD_DEBUG_REPORT:
		return len == 1 ? zlrs_diagnostics_report(value[0], &out->report) : -EINVAL;
#endif
	default:
		return -ENOTSUP;
	}
}

static int encode_response(zcbor_state_t *zs, int rc);

static void complete_request(bool maintenance)
{
	const int rc = operation(maintenance);
	request.result = request.writer != NULL ? encode_response(request.writer, rc) : rc;
	k_sem_give(&request_done);
}

static int submit(uint32_t op, const uint8_t *value, size_t len, zcbor_state_t *writer)
{
	if (len > sizeof(request.value) || (len != 0 && value == NULL)) {
		return writer != NULL ? encode_response(writer, -EINVAL) : -EINVAL;
	}
	k_mutex_lock(&request_lock, K_FOREVER);
	request.op = op;
	request.writer = writer;
	request.len = len;
	if (len) {
		memcpy(request.value, value, len);
	}
	k_sem_give(&request_pending);
	k_sem_take(&request_done, K_FOREVER);
	const int rc = request.result;
	memset(request.value, 0, sizeof(request.value));
	k_mutex_unlock(&request_lock);
	return rc;
}

bool zlrs_mgmt_process_idle(void)
{
	publish_mode();
	if (k_sem_take(&request_pending, K_NO_WAIT) != 0) {
		return false;
	}
	if (request.op == ZLRS_MGMT_CMD_ENTER && request.len == 0) {
		return true;
	}
	complete_request(false);
	return false;
}

int zlrs_mgmt_run(void)
{
	zlrs_watchdog_disarm();
	mode.session = true;
	mode_pending = true;
	publish_mode();
	mgmt_log_flush();
	zlrs_trace_session(true);
	atomic_set(&session_active, 1);
	atomic_set(&last_activity, k_uptime_get_32());
	complete_request(true);
	bool exit_requested = false;
	while (!exit_requested && (uint32_t)(k_uptime_get_32() - atomic_get(&last_activity)) <
					  CONFIG_ZLRS_APP_MGMT_TIMEOUT_S * 1000U) {
		/* A maintenance session cannot extend an unconfirmed image's boot deadline. */
		zlrs_lifecycle_check_rollback(false, CONFIG_ZLRS_APP_MCUBOOT_CONFIRM_TIMEOUT_MS);
		zlrs_trace_process(true);
		publish_mode();
		zlrs_serial_publish_state();
		zlrs_log_console_sync();
		(void)log_process();
		if (k_sem_take(&request_pending, K_MSEC(10)) == 0) {
			const int rc = operation(true);
			request.result =
				request.writer != NULL ? encode_response(request.writer, rc) : rc;
			exit_requested = request.op == ZLRS_MGMT_CMD_EXIT && rc == 0;
			atomic_set(&last_activity, k_uptime_get_32());
			if (exit_requested) {
				/* A successful EXIT is acknowledged only after common session cleanup. */
				k_msleep(CONFIG_MCUMGR_GRP_FS_FILE_AUTOMATIC_IDLE_CLOSE_TIME + 10);
			} else {
				k_sem_give(&request_done);
			}
		}
	}
	zlrs_trace_session(false);
	atomic_set(&session_active, 0);
	mode.session = false;
	mode_pending = true;
	publish_mode();
	if (exit_requested) {
		k_sem_give(&request_done);
	}
	zlrs_watchdog_arm();
	return exit_requested ? 0 : 1;
}

static int product_command(struct smp_streamer *ctxt, bool write)
{
	uint32_t op = ZLRS_MGMT_CMD_STATUS;
	struct zcbor_string value = {0};
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("op", zcbor_uint32_decode, &op),
		ZCBOR_MAP_DECODE_KEY_DECODER("value", zcbor_bstr_decode, &value),
	};
	if (zcbor_map_decode_bulk(ctxt->reader->zs, fields, ARRAY_SIZE(fields), &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}
	if (!write && (op != ZLRS_MGMT_CMD_STATUS && op != ZLRS_MGMT_CMD_OBSERVE)) {
		return MGMT_ERR_EINVAL;
	}
	return submit(op, value.value, value.len, ctxt->writer->zs);
}

static int encode_response(zcbor_state_t *zs, int rc)
{
	if (rc == -ENOTSUP) {
		return MGMT_ERR_ENOTSUP;
	}
	bool ok = zcbor_tstr_put_lit(zs, "result") && zcbor_int32_put(zs, rc);
	if (rc != 0) {
		return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}
	const uint32_t op = request.op;
	if (op == ZLRS_MGMT_CMD_STATUS || op == ZLRS_MGMT_CMD_OBSERVE) {
		const zlrs_rf_state_t *rf = zlrs_rf_state();
		const zlrs_link_status_t *link = rf->link;
		const zlrs_lr2021_diag_t *radio = zlrs_lr2021_diag();
		ok = ok && zcbor_tstr_put_lit(zs, "uptime_ms") &&
		     zcbor_uint64_put(zs, request.reply.uptime_ms) &&
		     zcbor_tstr_put_lit(zs, "hardware_ms") &&
		     zcbor_uint32_put(zs, rf->hardware_sampled_ms) &&
		     zcbor_tstr_put_lit(zs, "cpu_load_permille") &&
		     zcbor_int32_put(zs, request.reply.cpu_load_permille) &&
		     zcbor_tstr_put_lit(zs, "cpu_window_ms") &&
		     zcbor_uint64_put(zs, request.reply.cpu_window_ms) &&
		     zcbor_tstr_put_lit(zs, "status") && zcbor_map_start_encode(zs, 64);

		const struct {
			const char *name;
			int64_t value;
		} fields[] = {
			{"role", link->cfg.role},
			{"state", link->state},
			{"flags", status_flags(rf)},
			{"lq_percent", link->stats.lq_percent},
			{"regions", zlrs_region_supported_mask()},
			{"region", zlrs_persist_state()->settings.region},
			{"profile", link->cfg.profile},
			{"tx_power_dbm", rf->power.ceiling_dbm},
			{"data_baud", zlrs_persist_state()->settings.data_baud},
			{"rssi_dbm", link->stats.last_rssi},
			{"peer_rssi_dbm", link->peer_rssi_dbm},
			{"peer_lq_percent", link->peer_lq_percent},
			{"lq_generation", link->stats.lq_generation},
			{"peer_lq_generation", link->peer_lq_generation},
			{"power_raises", rf->power.raises},
			{"power_lowers", rf->power.lowers},
			{"power_fallbacks", rf->power.fallbacks},
			{"power_changed_ms", rf->power.last_change_ms},
			{"peer_feedback_ms", link->last_peer_feedback_ms},
			{"power_rssi_dbm", rf->power.filtering ? rf->power.rssi_q8 / 256 : 0},
			{"active_power_dbm",
			 rf->links_ready ? link->cfg.tx_power_dbm : rf->power.ceiling_dbm},
			{"power_floor_dbm", rf->power.floor_dbm},
			{"temperature_centi_c", rf->temperature_centi_c},
			{"tx_frames", link->stats.tx_frames},
			{"rx_frames", link->stats.rx_frames},
			{"rx_crc_errors", radio->rx_crc_errors},
			{"rx_bad_tag", link->stats.rx_bad_tag},
			{"tx_retries", link->stats.tx_retries},
			{"data_drop_bytes", link->stats.data_drop_bytes},
			{"missed_slots", rf->missed_slots},
			{"radio_fail", link->stats.radio_fail},
			{"frequency_hz", zlrs_fhss_current_hz(&link->fhss)},
			{"hop_index", link->fhss.index},
			{"frame_period_us", rf->frame_period_us},
			{"radio_temperature_centi_c", rf->radio.temperature_centi_c},
			{"radio_xosc_temperature_centi_c", rf->radio.xosc_temperature_centi_c},
			{"radio_vbat_mv", rf->radio.vbat_mv},
			{"radio_version_major", rf->radio.version_major},
			{"radio_version_minor", rf->radio.version_minor},
			{"radio_chip_mode", rf->radio.chip_mode},
			{"radio_command_status", rf->radio.command_status},
			{"radio_reset_status", rf->radio.reset_status},
			{"radio_valid", rf->radio.valid},
			{"radio_errors", rf->radio.errors},
			{"radio_irq", rf->radio.irq},
			{"data_phy", link->cfg.phy},
		};

		for (size_t i = 0; ok && i < ARRAY_SIZE(fields); i++) {
			ok = zcbor_tstr_encode_ptr(zs, fields[i].name, strlen(fields[i].name)) &&
			     zcbor_int64_put(zs, fields[i].value);
		}
		ok = ok && zcbor_tstr_put_lit(zs, "dev_id") &&
		     zcbor_bstr_encode_ptr(zs, link->dev_id, ZLRS_DEV_ID_LEN) &&
		     zcbor_tstr_put_lit(zs, "peer_id") &&
		     zcbor_bstr_encode_ptr(zs, link->pairing.peer_id, ZLRS_DEV_ID_LEN) &&
		     zcbor_map_end_encode(zs, 64);
	}
	if (op == ZLRS_MGMT_CMD_DEBUG_REPORT) {
		ok = ok && zcbor_tstr_put_lit(zs, "report") &&
		     zcbor_tstr_encode_ptr(zs, request.reply.report, strlen(request.reply.report));
	}
	if (op == ZLRS_MGMT_CMD_LOG_INFO) {
		ok = ok && zcbor_tstr_put_lit(zs, "files") &&
		     zcbor_list_start_encode(zs, ZLRS_LOG_FILES_MAX);
		for (size_t i = 0; i < request.reply.logs.count; i++) {
			char path[48];
			snprintf(path, sizeof(path), CONFIG_LOG_BACKEND_FS_DIR "/log.%04u",
				 request.reply.logs.num[i]);
			ok = ok && zcbor_map_start_encode(zs, 2) &&
			     zcbor_tstr_put_lit(zs, "path") &&
			     zcbor_tstr_encode_ptr(zs, path, strlen(path)) &&
			     zcbor_tstr_put_lit(zs, "size") &&
			     zcbor_uint32_put(zs, request.reply.logs.size[i]) &&
			     zcbor_map_end_encode(zs, 2);
		}
		ok = ok && zcbor_list_end_encode(zs, ZLRS_LOG_FILES_MAX);
	}
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int product_read(struct smp_streamer *ctxt)
{
	return product_command(ctxt, false);
}

static int product_write(struct smp_streamer *ctxt)
{
	return product_command(ctxt, true);
}

static enum mgmt_cb_return native_access(uint32_t event, enum mgmt_cb_return previous, int32_t *rc,
					 uint16_t *group, bool *abort_more, void *data, size_t size)
{
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(size);
	if (previous != MGMT_CB_OK) {
		return previous;
	}
#if defined(CONFIG_MCUMGR_GRP_OS_INFO_CUSTOM_HOOKS)
	if (event == MGMT_EVT_OP_OS_MGMT_INFO_APPEND) {
		struct os_mgmt_info_append *info = data;
		if (info->all_format_specified) {
			const size_t room = info->buffer_size - *info->output_length;
			const int n = snprintf((char *)info->output + *info->output_length, room,
					       "%szlrs %s", *info->prior_output ? " " : "",
					       APP_VERSION_STRING);
			if (n < 0 || n >= room) {
				*rc = MGMT_ERR_EMSGSIZE;
				return MGMT_CB_ERROR_RC;
			}
			*info->output_length += n;
			*info->prior_output = true;
		}
		return MGMT_CB_OK;
	}
#endif
	if (event == MGMT_EVT_OP_OS_MGMT_RESET) {
		/* Native OS reset owns boot_mode and delayed reboot; the owner parks RF first. */
		if (!atomic_get(&session_active)) {
			if (submit(ZLRS_MGMT_CMD_ENTER, NULL, 0, NULL) != 0) {
				*rc = MGMT_ERR_EBUSY;
				return MGMT_CB_ERROR_RC;
			}
		}
		return MGMT_CB_OK;
	}
	if (event == MGMT_EVT_OP_FS_MGMT_FILE_ACCESS) {
		struct fs_mgmt_file_access *access = data;
		const char *path = access->filename;
		bool allowed = false;
		if (strncmp(path, CONFIG_LOG_BACKEND_FS_DIR "/log.",
			    strlen(CONFIG_LOG_BACKEND_FS_DIR "/log.")) == 0) {
			const char *suffix = path + strlen(CONFIG_LOG_BACKEND_FS_DIR "/log.");
			allowed = strlen(suffix) == 4;
			for (size_t i = 0; allowed && i < 4; i++) {
				allowed = suffix[i] >= '0' && suffix[i] <= '9';
			}
		}
#if defined(CONFIG_ZLRS_APP_DEBUG_TRACE)
		/* Trace's own callback validates committed artifact names. */
		allowed |= strncmp(path, "/trace/", 7) == 0;
#endif
		if (!atomic_get(&session_active) || !allowed ||
		    access->access == FS_MGMT_FILE_ACCESS_WRITE) {
			*rc = MGMT_ERR_EACCESSDENIED;
			return MGMT_CB_ERROR_RC;
		}
		return MGMT_CB_OK;
	}
	struct mgmt_evt_op_cmd_arg *cmd = data;
	atomic_set(&last_activity, k_uptime_get_32());
	if ((cmd->group == MGMT_GROUP_ID_IMAGE || cmd->group == MGMT_GROUP_ID_FS) &&
	    !atomic_get(&session_active)) {
		/* Flash and filesystem work is restricted to an explicit maintenance session. */
		if (cmd->group == MGMT_GROUP_ID_FS || cmd->op == MGMT_OP_WRITE) {
			*rc = MGMT_ERR_EACCESSDENIED;
			return MGMT_CB_ERROR_RC;
		}
	}
	return MGMT_CB_OK;
}

/* Event masks combine only within one MCUmgr callback group. */
static struct mgmt_callback smp_callback = {
	.callback = native_access,
	.event_id = MGMT_EVT_OP_CMD_RECV,
};
static struct mgmt_callback fs_callback = {
	.callback = native_access,
	.event_id = MGMT_EVT_OP_FS_MGMT_FILE_ACCESS,
};
static struct mgmt_callback os_callback = {
	.callback = native_access,
	.event_id = MGMT_EVT_OP_OS_MGMT_RESET
#if defined(CONFIG_MCUMGR_GRP_OS_INFO_CUSTOM_HOOKS)
		    | MGMT_EVT_OP_OS_MGMT_INFO_APPEND
#endif
	,
};
static const struct mgmt_handler handlers[] = {
	{.mh_read = product_read, .mh_write = product_write}};
static struct mgmt_group product_group = {
	.mg_handlers = handlers,
	.mg_handlers_count = ARRAY_SIZE(handlers),
	.mg_group_id = 66,
};

static void register_management(void)
{
	mgmt_register_group(&product_group);
	mgmt_callback_register(&smp_callback);
	mgmt_callback_register(&fs_callback);
	mgmt_callback_register(&os_callback);
}

MCUMGR_HANDLER_DEFINE(zlrs_product, register_management);
