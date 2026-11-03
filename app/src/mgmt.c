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
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt_callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_callbacks.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include "persist.h"
#include "rf.h"
#include "serial.h"
#include "trace.h"
#include "platform/watchdog.h"
#include "zlrs/proto/crypto.h"
#if defined(CONFIG_ZLRS_APP_DEBUG)
#include "diagnostics.h"
#endif

BUILD_ASSERT(DT_SAME_NODE(DT_CHOSEN(zephyr_uart_mcumgr), DT_ALIAS(zlrs_mgmt_cdc)),
	     "Native SMP must use the management CDC, not the DATA or log UART");
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
	struct zlrs_mgmt_reply reply;
} request;

static void publish_mode(void)
{
	if (mode_pending && zbus_chan_pub(&zlrs_mode_chan, &mode, K_NO_WAIT) == 0) {
		mode_pending = false;
	}
}

int zlrs_mgmt_init(void)
{
	return 0; /* Native transport owns the UART and IRQs. */
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

static uint32_t read_status(zlrs_mgmt_status_t *out)
{
	const zlrs_rf_status_t rf = zlrs_rf_status();
	const zlrs_persist_settings_t *const settings = zlrs_persist_settings();

	*out = (zlrs_mgmt_status_t){
		.role = rf.role,
		.profile = rf.profile,
		.state = rf.state,
		.lq_percent = rf.lq_percent,
		.band = DT_PROP(DT_NODELABEL(zlrs_rf), band),
		.region = settings->region,
		.tx_power_dbm = rf.power_ceiling_dbm,
		.data_baud = settings->data_baud,
		.rssi_dbm = rf.rssi_dbm,
		.peer_rssi_dbm = rf.peer_rssi_dbm,
		.peer_lq_percent = rf.peer_lq_percent,
		.active_power_dbm = rf.tx_power_dbm,
		.power_floor_dbm = rf.power_floor_dbm,
		.temperature_centi_c = rf.temperature_centi_c,
		.tx_frames = rf.tx_frames,
		.rx_frames = rf.rx_frames,
		.rx_crc_errors = rf.rx_crc_errors,
		.rx_bad_tag = rf.rx_bad_tag,
		.tx_retries = rf.tx_retries,
		.data_drop_bytes = rf.data_drop_bytes,
		.missed_slots = rf.missed_slots,
		.radio_fail = rf.radio_fail,
		.frequency_hz = rf.frequency_hz,
		.hop_index = rf.hop_index,
		.frame_period_us = rf.frame_period_us,
		.radio_temperature_centi_c = rf.radio.temperature_centi_c,
		.radio_xosc_temperature_centi_c = rf.radio.xosc_temperature_centi_c,
		.radio_vbat_mv = rf.radio.vbat_mv,
		.radio_version_major = rf.radio.version_major,
		.radio_version_minor = rf.radio.version_minor,
		.radio_chip_mode = rf.radio.chip_mode,
		.radio_command_status = rf.radio.command_status,
		.radio_reset_status = rf.radio.reset_status,
		.radio_valid = rf.radio.valid,
		.radio_errors = rf.radio.errors,
		.radio_irq = rf.radio.irq,
		.data_phy = rf.data_phy,
	};
	if (rf.data_band == ZLRS_DATA_BAND_SUBG) {
		out->flags |= ZLRS_MGMT_FLAG_DATA_SUBG;
	}
	if (rf.band_mode == ZLRS_DATA_BAND_AUTO) {
		out->flags |= ZLRS_MGMT_FLAG_BAND_AUTO;
	}
	if (rf.band_acquiring) {
		out->flags |= ZLRS_MGMT_FLAG_BAND_ACQUIRING;
	}
	if (rf.power_fixed) {
		out->flags |= ZLRS_MGMT_FLAG_POWER_FIXED;
	}
	if (rf.band_planned) {
		out->flags |= ZLRS_MGMT_FLAG_BAND_PLANNED;
	}
	if (rf.configured) {
		out->flags |= ZLRS_MGMT_FLAG_KEY;
	}
	if (rf.paired) {
		out->flags |= ZLRS_MGMT_FLAG_PAIRED;
	}
	if (rf.session_valid) {
		out->flags |= ZLRS_MGMT_FLAG_SESSION;
	}
	memcpy(out->dev_id, rf.dev_id, ZLRS_DEV_ID_LEN);
	memcpy(out->peer_id, rf.peer_id, ZLRS_DEV_ID_LEN);
	return rf.hardware_sampled_ms;
}

static uint8_t hex_nibble(char c, bool *ok)
{
	if (c >= '0' && c <= '9') {
		return (uint8_t)(c - '0');
	}
	if (c >= 'a' && c <= 'f') {
		return (uint8_t)(c - 'a' + 10);
	}
	if (c >= 'A' && c <= 'F') {
		return (uint8_t)(c - 'A' + 10);
	}

	*ok = false;
	return 0U;
}

static int execute(bool maintenance)
{
	struct zlrs_mgmt_reply *out = &request.reply;
	const uint8_t *value = request.value;
	const size_t len = request.len;
	const uint32_t op = request.op;
	memset(out, 0, sizeof(*out));
	if (op == ZLRS_MGMT_CMD_STATUS || op == ZLRS_MGMT_CMD_OBSERVE) {
		if (maintenance) {
			zlrs_rf_status_refresh();
		}
		out->hardware_ms = read_status(&out->status);
		out->uptime_ms = k_uptime_get_32();
		return len == 0 ? 0 : -EINVAL;
	}
	if (!maintenance) {
		return op == ZLRS_MGMT_CMD_EXIT ? 0 : -EACCES;
	}
	switch (op) {
	case ZLRS_MGMT_CMD_ENTER:
		mgmt_log_flush();
		return len == 0 ? 0 : -EINVAL;
	case ZLRS_MGMT_CMD_EXIT:
		return len == 0 ? 0 : -EINVAL;
	case ZLRS_MGMT_CMD_PAIR:
		return len == 0 ? zlrs_rf_pair_start() : -EINVAL;
	case ZLRS_MGMT_CMD_NEW_DEV_ID: {
		if (len != 0) {
			return -EINVAL;
		}
		const int rc = zlrs_persist_new_identity();
		zlrs_rf_credentials();
		return rc;
	}
	case ZLRS_MGMT_CMD_ERASE_PAIR: {
		if (len != 0) {
			return -EINVAL;
		}
		const int rc = zlrs_persist_erase_pairing();
		if (rc == 0) {
			zlrs_rf_credentials();
		}
		return rc;
	}
	case ZLRS_MGMT_CMD_FACTORY_RESET: {
		if (len != 0) {
			return -EINVAL;
		}
		const int rc = zlrs_persist_factory_reset();
		zlrs_rf_credentials();
		return rc;
	}
	case ZLRS_MGMT_CMD_SET_KEY_HEX:
	case ZLRS_MGMT_CMD_SET_PHRASE: {
		uint8_t key[ZLRS_KEY_LEN];
		if (op == ZLRS_MGMT_CMD_SET_KEY_HEX) {
			if (len != 2U * ZLRS_KEY_LEN) {
				return -EINVAL;
			}
			bool valid = true;
			for (size_t i = 0; i < ZLRS_KEY_LEN; i++) {
				const uint8_t hi = hex_nibble(value[2 * i], &valid);
				const uint8_t lo = hex_nibble(value[2 * i + 1], &valid);
				key[i] = hi << 4 | lo;
			}
			if (!valid) {
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
			zlrs_root_key_from_phrase(phrase, key);
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
		zlrs_persist_settings_t settings = *zlrs_persist_settings();
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
		return len == 0 ? zlrs_log_info(&out->logs) : -EINVAL;
#if defined(CONFIG_ZLRS_APP_DEBUG)
	case ZLRS_MGMT_CMD_DEBUG_REPORT:
		return len == 1 ? zlrs_diagnostics_report(value[0], &out->report) : -EINVAL;
#endif
	default:
		return -ENOTSUP;
	}
}

int zlrs_mgmt_request(uint32_t op, const uint8_t *value, size_t len, struct zlrs_mgmt_reply *reply)
{
	if (len > sizeof(request.value) || (len != 0 && value == NULL)) {
		return -EINVAL;
	}
	k_mutex_lock(&request_lock, K_FOREVER);
	request.op = op;
	request.len = len;
	if (len) {
		memcpy(request.value, value, len);
	}
	k_sem_give(&request_pending);
	k_sem_take(&request_done, K_FOREVER);
	const int rc = request.result;
	if (reply) {
		*reply = request.reply;
	}
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
	request.result = execute(false);
	k_sem_give(&request_done);
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
	request.result = execute(true);
	k_sem_give(&request_done);
	bool exit_requested = false;
	while (!exit_requested && (uint32_t)(k_uptime_get_32() - atomic_get(&last_activity)) <
					  CONFIG_ZLRS_APP_MGMT_TIMEOUT_S * 1000U) {
		zlrs_trace_process(true);
		publish_mode();
		zlrs_serial_publish_state();
		zlrs_log_console_sync();
		(void)log_process();
		if (k_sem_take(&request_pending, K_MSEC(10)) == 0) {
			request.result = execute(true);
			exit_requested = request.op == ZLRS_MGMT_CMD_EXIT && request.result == 0;
			atomic_set(&last_activity, k_uptime_get_32());
			if (exit_requested) {
				/* Close native FS transfers before resuming RF. */
				k_msleep(CONFIG_MCUMGR_GRP_FS_FILE_AUTOMATIC_IDLE_CLOSE_TIME + 10);
				zlrs_trace_session(false);
				atomic_set(&session_active, 0);
				mode.session = false;
				mode_pending = true;
				publish_mode();
			}
			k_sem_give(&request_done);
		}
	}
	zlrs_trace_session(false);
	atomic_set(&session_active, 0);
	mode.session = false;
	mode_pending = true;
	publish_mode();
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
	struct zlrs_mgmt_reply reply;
	const int rc = zlrs_mgmt_request(op, value.value, value.len, &reply);
	if (rc == -ENOTSUP) {
		return MGMT_ERR_ENOTSUP;
	}
	zcbor_state_t *zs = ctxt->writer->zs;
	bool ok = zcbor_tstr_put_lit(zs, "result") && zcbor_int32_put(zs, rc);
	if (rc != 0) {
		return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}
	if (op == ZLRS_MGMT_CMD_STATUS || op == ZLRS_MGMT_CMD_OBSERVE) {
		ok = ok && zcbor_tstr_put_lit(zs, "uptime_ms") &&
		     zcbor_uint32_put(zs, reply.uptime_ms) &&
		     zcbor_tstr_put_lit(zs, "hardware_ms") &&
		     zcbor_uint32_put(zs, reply.hardware_ms) && zcbor_tstr_put_lit(zs, "status") &&
		     zcbor_map_start_encode(zs, 48);
#define FIELD(name)                                                                                \
	ok = ok && zcbor_tstr_put_lit(zs, #name) && zcbor_int64_put(zs, reply.status.name)
		FIELD(role);
		FIELD(state);
		FIELD(flags);
		FIELD(lq_percent);
		FIELD(band);
		FIELD(region);
		FIELD(profile);
		FIELD(tx_power_dbm);
		FIELD(data_baud);
		FIELD(rssi_dbm);
		FIELD(peer_rssi_dbm);
		FIELD(peer_lq_percent);
		FIELD(active_power_dbm);
		FIELD(power_floor_dbm);
		FIELD(temperature_centi_c);
		FIELD(tx_frames);
		FIELD(rx_frames);
		FIELD(rx_crc_errors);
		FIELD(rx_bad_tag);
		FIELD(tx_retries);
		FIELD(data_drop_bytes);
		FIELD(missed_slots);
		FIELD(radio_fail);
		FIELD(frequency_hz);
		FIELD(hop_index);
		FIELD(frame_period_us);
		FIELD(radio_temperature_centi_c);
		FIELD(radio_xosc_temperature_centi_c);
		FIELD(radio_vbat_mv);
		FIELD(radio_version_major);
		FIELD(radio_version_minor);
		FIELD(radio_chip_mode);
		FIELD(radio_command_status);
		FIELD(radio_reset_status);
		FIELD(radio_valid);
		FIELD(radio_errors);
		FIELD(radio_irq);
		FIELD(data_phy);
#undef FIELD
		ok = ok && zcbor_tstr_put_lit(zs, "dev_id") &&
		     zcbor_bstr_encode_ptr(zs, reply.status.dev_id, ZLRS_DEV_ID_LEN) &&
		     zcbor_tstr_put_lit(zs, "peer_id") &&
		     zcbor_bstr_encode_ptr(zs, reply.status.peer_id, ZLRS_DEV_ID_LEN) &&
		     zcbor_map_end_encode(zs, 48);
	}
	if (op == ZLRS_MGMT_CMD_DEBUG_REPORT) {
		ok = ok && zcbor_tstr_put_lit(zs, "report") &&
		     zcbor_tstr_encode_ptr(zs, reply.report, strlen(reply.report));
	}
	if (op == ZLRS_MGMT_CMD_LOG_INFO) {
		ok = ok && zcbor_tstr_put_lit(zs, "files") &&
		     zcbor_list_start_encode(zs, ZLRS_LOG_FILES_MAX);
		for (size_t i = 0; i < reply.logs.count; i++) {
			char path[48];
			snprintf(path, sizeof(path), CONFIG_LOG_BACKEND_FS_DIR "/log.%04u",
				 reply.logs.num[i]);
			ok = ok && zcbor_map_start_encode(zs, 2) &&
			     zcbor_tstr_put_lit(zs, "path") &&
			     zcbor_tstr_encode_ptr(zs, path, strlen(path)) &&
			     zcbor_tstr_put_lit(zs, "size") &&
			     zcbor_uint32_put(zs, reply.logs.size[i]) &&
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
			if (zlrs_mgmt_request(ZLRS_MGMT_CMD_ENTER, NULL, 0, NULL) != 0) {
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
