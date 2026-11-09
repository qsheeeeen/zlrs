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

#include "platform/slot_timer.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/cpu_load.h>
#include <zephyr/sys/util.h>

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
static atomic_t fs_active, fs_handle_open;

static void fs_barrier(struct k_work *work)
{
	ARG_UNUSED(work);
}

static K_WORK_DEFINE(fs_close_barrier, fs_barrier);

bool zlrs_mgmt_fs_busy(void)
{
	return atomic_get(&fs_active) != 0 || atomic_get(&fs_handle_open) != 0;
}

static K_MUTEX_DEFINE(request_lock);
static K_SEM_DEFINE(request_pending, 0, 1);
static K_SEM_DEFINE(request_done, 0, 1);

static struct {
	uint32_t op;
	const uint8_t *value; /* Borrowed until this serialized request finishes. */
	size_t len;
	int result;

	union {
		struct {
			uint64_t uptime_ms, cpu_window_ms;
			int32_t cpu_load_permille;
			zlrs_rf_state_t rf;
			zlrs_link_status_t link;
			zlrs_lr2021_diag_t radio;
			zlrs_persist_settings_t settings;
			struct zlrs_slot_timer_stats timer;
			uint32_t protocol_ms, kernel_ms;
		} status;

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
	       (rf->link->cfg.automatic_band ? ZLRS_MGMT_FLAG_BAND_AUTO : 0U) |
	       (rf->link->radio_acquiring ? ZLRS_MGMT_FLAG_BAND_ACQUIRING : 0U) |
	       (rf->power_fixed ? ZLRS_MGMT_FLAG_POWER_FIXED : 0U) |
	       (rf->link->band_plan.switch_frame != 0U ? ZLRS_MGMT_FLAG_BAND_PLANNED : 0U) |
	       (rf->link->state != ZLRS_STATE_UNCONFIGURED ? ZLRS_MGMT_FLAG_KEY : 0U) |
	       (rf->link->paired ? ZLRS_MGMT_FLAG_PAIRED : 0U) |
	       (rf->link->session_valid ? ZLRS_MGMT_FLAG_SESSION : 0U);
}

static int operation(bool maintenance)
{
	__typeof__(request.reply) *out = &request.reply;
	const uint8_t *value = request.value;
	const size_t len = request.len;
	const uint32_t op = request.op;
	if (op == ZLRS_MGMT_CMD_STATUS) {
		static uint64_t cpu_sampled_ms;
		if (len > 1U || (len == 1U && value[0] > (IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
								  ? ZLRS_STATUS_POWER
								  : ZLRS_STATUS_BASIC))) {
			return -EINVAL;
		}
		if (maintenance) {
			zlrs_rf_status_refresh();
		}
		/* Shared STATUS window; the first sample measures since boot. */
		out->status.uptime_ms = k_uptime_get();
		out->status.cpu_load_permille = cpu_load_get(true);
		out->status.cpu_window_ms = out->status.uptime_ms - cpu_sampled_ms;
		cpu_sampled_ms = out->status.uptime_ms;
		/* Copy public facts once; the SMP worker owns all subsequent encoding. */
		out->status.rf = *zlrs_rf_state();
		out->status.link = *out->status.rf.link;
		out->status.rf.link = &out->status.link;
		out->status.radio = *zlrs_lr2021_diag();
		out->status.settings = zlrs_persist_state()->settings;
#if defined(CONFIG_ZLRS_APP_DEBUG)
		if (len == 1U && value[0] == ZLRS_STATUS_RADIO) {
			zlrs_slot_timer_stats(&out->status.timer);
			out->status.protocol_ms = zlrs_slot_timer_now_ms();
			out->status.kernel_ms = k_uptime_get_32();
			if (out->status.rf.paused) {
				const uint64_t elapsed = k_cyc_to_us_floor64(
					k_cycle_get_64() - out->status.rf.pause_started_cycles);
				out->status.rf.maintenance.total_us += elapsed;
				out->status.rf.maintenance.max_us =
					MAX(out->status.rf.maintenance.max_us, elapsed);
			}
		}
#endif

		return 0;
	}
#if defined(CONFIG_ZLRS_APP_DEBUG_TRACE)
	if (op == ZLRS_MGMT_CMD_TRACE) {
		if (len == 0U) {
			return 0;
		}
		return len == 5U && value[0] <= ZLRS_TRACE_ARM
			       ? zlrs_trace_control(value[0], sys_get_le32(value + 1U), maintenance)
			       : -EINVAL;
	}
#endif
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

	default:
		return -ENOTSUP;
	}
}

static int encode_response(zcbor_state_t *zs, int rc);

static bool complete_request(bool maintenance)
{
	const int rc = operation(maintenance);
	request.result = rc;
	const bool exiting = maintenance && request.op == ZLRS_MGMT_CMD_EXIT && rc == 0;
	if (!exiting) {
		k_sem_give(&request_done);
	}
	return exiting;
}

static int submit(uint32_t op, const uint8_t *value, size_t len, zcbor_state_t *writer)
{
	k_mutex_lock(&request_lock, K_FOREVER);
	request.op = op;
	request.len = len;
	request.value = value;
	int prepared = len > 64U || (len != 0U && value == NULL) ? -EINVAL : 0;
#if defined(CONFIG_ZLRS_APP_DEBUG_TRACE)
	if (prepared == 0 && op == ZLRS_MGMT_CMD_TRACE && len == 5U && value[0] <= ZLRS_TRACE_ARM) {
		prepared = zlrs_trace_prepare(value[0], sys_get_le32(value + 1U));
	}
#endif
	if (prepared == 0) {
		k_sem_give(&request_pending);
		k_sem_take(&request_done, K_FOREVER);
	} else {
		request.result = prepared;
	}
	const int rc = writer != NULL ? encode_response(writer, request.result) : request.result;
	request.value = NULL;
	k_mutex_unlock(&request_lock);
	return rc;
}

bool zlrs_mgmt_process(void)
{
	publish_mode();
	if (k_sem_take(&request_pending, K_NO_WAIT) != 0) {
		return false;
	}
	if (request.op != ZLRS_MGMT_CMD_ENTER || request.len != 0) {
		(void)complete_request(false);
		return false;
	}
	zlrs_rf_pause();
	LOG_WRN("entering management mode");
	zlrs_rf_log_snapshot();
	zlrs_watchdog_disarm();
	mode.session = true;
	mode_pending = true;
	publish_mode();
	mgmt_log_flush();
	(void)zlrs_trace_discover();
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
			exit_requested = complete_request(true);
			atomic_set(&last_activity, k_uptime_get_32());
		}
	}
	atomic_set(&session_active, 0);
	while (atomic_get(&fs_active) != 0) {
		k_msleep(1);
	}
	if (atomic_get(&fs_handle_open) != 0) {
		/* The native FS idle closer runs on the system workqueue. Flush a
		 * barrier after its deadline before the application resumes RF. */
		struct k_work_sync sync;
		k_msleep(CONFIG_MCUMGR_GRP_FS_FILE_AUTOMATIC_IDLE_CLOSE_TIME + 1);
		(void)k_work_submit(&fs_close_barrier);
		(void)k_work_flush(&fs_close_barrier, &sync);
		atomic_set(&fs_handle_open, 0);
	}
	mode.session = false;
	mode_pending = true;
	publish_mode();
	if (exit_requested) {
		k_sem_give(&request_done);
	}
	zlrs_watchdog_arm();
	if (!exit_requested) {
		LOG_WRN("management session timed out");
	}
	LOG_INF("management mode done, back to normal");
	zlrs_log_console_sync();
	log_flush();
	zlrs_rf_resume();
	return true;
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
	if (!write && op != ZLRS_MGMT_CMD_STATUS) {
		return MGMT_ERR_EINVAL;
	}
	return submit(op, value.value, value.len, ctxt->writer->zs);
}

struct mgmt_field {
	const char *name;
	int64_t value;
};

static bool encode_fields(zcbor_state_t *zs, const struct mgmt_field *fields, size_t count)
{
	bool ok = true;
	for (size_t i = 0; ok && i < count; i++) {
		ok = zcbor_tstr_encode_ptr(zs, fields[i].name, strlen(fields[i].name)) &&
		     zcbor_int64_put(zs, fields[i].value);
	}
	return ok;
}

#if defined(CONFIG_ZLRS_APP_DEBUG)
static bool encode_view(uint8_t view, zcbor_state_t *zs)
{
	const zlrs_rf_state_t *rf = &request.reply.status.rf;
	const zlrs_link_status_t *link = rf->link;
	const zlrs_link_cfg_t *cfg = &link->cfg;
	const zlrs_link_stats_t *stats = &link->stats;
	switch (view) {
	case ZLRS_STATUS_LINK: {
		const struct mgmt_field fields[] = {
			{"frame_index", link->frame_index},
			{"tx_bytes", stats->tx_bytes},
			{"rx_bytes", stats->rx_bytes},
			{"tx_data_frames", stats->tx_data_frames},
			{"tx_data_bytes", stats->tx_data_bytes},
			{"tx_full_data_frames", stats->tx_full_data_frames},
			{"tx_ack_only_frames", stats->tx_ack_only_frames},
			{"tx_data_ack_frames", stats->tx_data_ack_frames},
			{"tx_arq_wait_stalls", stats->tx_arq_wait_stalls},
			{"tx_retry_age_last_ms", stats->tx_retry_age_ms.last},
			{"tx_retry_age_max_ms", stats->tx_retry_age_ms.max},
			{"tx_ack_delay_last_ms", stats->tx_ack_delay_ms.last},
			{"tx_ack_delay_max_ms", stats->tx_ack_delay_ms.max},
			{"tx_ack_delay_ms_sum", stats->tx_ack_delay_ms_sum},
			{"tx_ack_entries", stats->tx_ack_entries},
			{"slot_tx_0", stats->slot_tx[0]},
			{"slot_tx_1", stats->slot_tx[1]},
			{"slot_rx_0", stats->slot_rx[0]},
			{"slot_rx_1", stats->slot_rx[1]},
			{"slot_hit_0", stats->slot_hit[0]},
			{"slot_hit_1", stats->slot_hit[1]},
			{"slot_fail_0", stats->slot_fail[0]},
			{"slot_fail_1", stats->slot_fail[1]},
			{"pair_req_tx", stats->pair_req_tx},
			{"pair_resp_tx", stats->pair_resp_tx},
			{"pair_cfm_tx", stats->pair_cfm_tx},
			{"pair_req_rx", stats->pair_req_rx},
			{"pair_resp_rx", stats->pair_resp_rx},
			{"pair_cfm_rx", stats->pair_cfm_rx},
			{"pair_rejects", stats->pair_rejects},
			{"tx_syncs", stats->tx_syncs},
			{"rx_syncs", stats->rx_syncs},
			{"sess_resp_tx", stats->sess_resp_tx},
			{"sess_resp_rx", stats->sess_resp_rx},
			{"sess_cfm_tx", stats->sess_cfm_tx},
			{"sess_cfm_rx", stats->sess_cfm_rx},
			{"tx_arq_retry_runs", stats->tx_arq_retry_runs},
			{"rx_dup", stats->rx_dup},
			{"rx_out_of_order", stats->rx_out_of_order},
			{"rx_bad_frame", stats->rx_bad_frame},
			{"hop_advances", stats->hop_advances},
			{"hop_resync", stats->hop_resync},
			{"queue_count", link->queue_count},
			{"arq_used", link->arq_used},
			{"arq_seq", link->arq_seq},
			{"arq_rx_seq", link->arq_rx_seq},
			{"arq_ack_pending", link->arq_ack_pending},
			{"resync_every_n", cfg->resync_every_n},
		};
		return encode_fields(zs, fields, ARRAY_SIZE(fields));
	}
	case ZLRS_STATUS_RATE: {
		const zlrs_rf_rate_sample_t *sample = &rf->rate;
		const uint32_t span = MAX(sample->span_ms, 1U);
		const struct mgmt_field fields[] = {
			{"span_ms", sample->span_ms},
			{"windows", sample->wins},
			{"heard", sample->hits},
			{"tx", sample->tx},
			{"rate_state", sample->state},
			{"windows_per_second_x10", (sample->wins * 10000U + span / 2U) / span},
			{"heard_per_second_x10", (sample->hits * 10000U + span / 2U) / span},
			{"tx_per_second_x10", (sample->tx * 10000U + span / 2U) / span},
			{"hit_percent_x10",
			 sample->wins ? (sample->hits * 1000U + sample->wins / 2U) / sample->wins
				      : 0U},
		};
		return encode_fields(zs, fields, ARRAY_SIZE(fields));
	}
	case ZLRS_STATUS_RADIO: {
		const zlrs_lr2021_diag_t *radio = &request.reply.status.radio;
		const struct zlrs_slot_timer_stats *timer = &request.reply.status.timer;
		const uint32_t protocol_ms = request.reply.status.protocol_ms;
		const uint32_t kernel_ms = request.reply.status.kernel_ms;
		const struct mgmt_field fields[] = {
			{"slot_budget_us", rf->frame_period_us / ZLRS_SLOT_COUNT},
			{"prepare_us", rf->slot_prepare_us},
			{"data_mtu", cfg->data_mtu},
			{"full_frame_airtime_us",
			 zlrs_phy_airtime_us(cfg->phy, cfg->data_mtu + ZLRS_DATA_HEADROOM +
							       zlrs_profile_tag_len(cfg->profile))},
			{"launch_guard_us", CONFIG_ZLRS_APP_TX_GUARD_US},
			{"maintenance_count", rf->maintenance.count},
			{"maintenance_total_us", rf->maintenance.total_us},
			{"maintenance_max_us", rf->maintenance.max_us},
			{"skipped_after_maintenance", rf->maintenance.skipped_after_maintenance},
			{"skipped_without_maintenance",
			 rf->maintenance.skipped_without_maintenance},
			{"protocol_ms", protocol_ms},
			{"kernel_ms", kernel_ms},
			{"clock_delta_ms", (int32_t)(protocol_ms - kernel_ms)},
			{"boundaries", timer->boundaries},
			{"irq_late_us_max", timer->irq_late_us_max},
			{"missed_boundaries", timer->missed_boundaries},
			{"skipped_frames", rf->skipped_frames},
			{"phase_err_us", rf->phase_err_us},
			{"phase_last_us", rf->phase_last_us},
			{"phase_hits", rf->phase_hits},
			{"transactions", radio->transactions},
			{"tx_done", radio->tx_done},
			{"rx_packets", radio->rx_packets},
			{"overruns", radio->overruns},
			{"late_starts", radio->prepare_late_starts + radio->trigger_late_starts},
			{"empty_irq", radio->empty_irq},
			{"tx_us_last", radio->tx_us_last},
			{"rx_us_last", radio->rx_us_last},
			{"irq_last", radio->irq_last},
			{"stale_irq", radio->stale_irq},
			{"stale_irq_bits", radio->stale_irq_bits},
			{"tx_finalize_last_us", rf->work.tx_finalize_us.last},
			{"tx_finalize_max_us", rf->work.tx_finalize_us.max},
			{"rx_accept_last_us", rf->work.rx_accept_us.last},
			{"rx_accept_max_us", rf->work.rx_accept_us.max},
			{"tx_prepare_last_us", rf->work.tx_prepare_us.last},
			{"tx_prepare_max_us", rf->work.tx_prepare_us.max},
			{"hash_busy_tx", rf->work.hash_busy_tx},
			{"hash_busy_rx", rf->work.hash_busy_rx},
			{"launch_late_last_us", radio->launch_late_us.last},
			{"launch_late_max_us", radio->launch_late_us.max},
			{"launch_irq_late_last_us", radio->launch_irq_late_us.last},
			{"launch_irq_late_max_us", radio->launch_irq_late_us.max},
			{"launch_cmd_last_us", radio->launch_cmd_us.last},
			{"launch_cmd_max_us", radio->launch_cmd_us.max},
			{"launch_jitter_last_us", radio->launch_jitter_us.last},
			{"launch_jitter_max_us", radio->launch_jitter_us.max},
			{"tx_launch_cmd_last_us", radio->tx_launch_cmd_us.last},
			{"tx_launch_cmd_max_us", radio->tx_launch_cmd_us.max},
			{"submit_last_us", radio->submit_us.last},
			{"submit_max_us", radio->submit_us.max},
			{"tx_fifo_load_last_us", radio->tx_fifo_load_us.last},
			{"tx_fifo_load_max_us", radio->tx_fifo_load_us.max},
			{"rx_fifo_last_us", radio->rx_fifo_us.last},
			{"rx_fifo_max_us", radio->rx_fifo_us.max},
			{"irq_service_last_us", radio->irq_service_us.last},
			{"irq_service_max_us", radio->irq_service_us.max},
			{"prepare_late_starts", radio->prepare_late_starts},
			{"trigger_late_starts", radio->trigger_late_starts},
			{"late_trigger_irq", radio->late_trigger_irq},
			{"tx_ready_lead_us_last", radio->tx_ready_lead_us_last},
			{"tx_fifo_loads", radio->tx_fifo_loads},
			{"tx_fifo_bytes", radio->tx_fifo_bytes},
			{"tx_fifo_errors", radio->tx_fifo_errors},
			{"tx_fifo_underflows", radio->tx_fifo_underflows},
			{"rx_header_errors", radio->rx_header_errors},
			{"rx_length_errors", radio->rx_length_errors},
			{"rx_payload_errors", radio->rx_payload_errors},
			{"rx_expired", radio->rx_expired},
			{"rx_incomplete", radio->rx_incomplete},
			{"rx_restarted", radio->rx_restarted},
			{"rx_fifo_bytes", radio->rx_fifo_bytes},
			{"tx_expired", radio->tx_expired},
			{"fs_reused", radio->fs_reused},
			{"chip_errors", radio->chip_errors},
			{"chip_error_freq_hz", radio->chip_error_freq_hz},
			{"tx_startup_last_us", radio->tx_startup_us.last},
			{"tx_startup_max_us", radio->tx_startup_us.max},
			{"late_prepare_entry_us", rf->late_prepare_entry_us},
			{"late_prepare_build_us", rf->late_prepare_build_us},
			{"late_prepare_spi_us", rf->late_prepare_spi_us},
			{"tx_standby_irq_us", radio->tx_stage_us[0]},
			{"tx_modem_us", radio->tx_stage_us[1]},
			{"tx_fifo_us", radio->tx_stage_us[2]},
			{"tx_fs_busy_us", radio->tx_stage_us[3]},
		};
		return encode_fields(zs, fields, ARRAY_SIZE(fields));
	}
	case ZLRS_STATUS_POWER: {
		const struct mgmt_field fields[] = {
			{"dynamic_power_dbm", rf->power.current_dbm},
			{"subg_region", rf->region != NULL ? rf->region - zlrs_regions : -1},
			{"region_cap_dbm", rf->region != NULL ? rf->region->max_tx_power_dbm : 0},
			{"rf_output_measured", 0},
		};
		return encode_fields(zs, fields, ARRAY_SIZE(fields));
	}
	default:
		return false;
	}
}
#endif

/* Shared fields finish before view-local tables, keeping their stack lifetimes disjoint. */
static bool encode_status_fields(zcbor_state_t *zs)
{
	const zlrs_rf_state_t *rf = &request.reply.status.rf;
	const zlrs_link_status_t *link = rf->link;
	const zlrs_lr2021_diag_t *radio = &request.reply.status.radio;

	const struct mgmt_field fields[] = {
		{"role", link->cfg.role},
		{"active_region", link->cfg.region},
		{"state", link->state},
		{"flags", status_flags(rf)},
		{"lq_percent", link->stats.lq_percent},
		{"regions", zlrs_region_supported_mask()},
		{"region", request.reply.status.settings.region},
		{"profile", link->cfg.profile},
		{"tx_power_dbm", rf->power.ceiling_dbm},
		{"data_baud", request.reply.status.settings.data_baud},
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
		{"power_rssi_dbm", rf->power.rssi_q8 / 256},
		{"active_power_dbm",
		 rf->links_ready ? link->cfg.tx_power_dbm : rf->power.ceiling_dbm},
		{"power_floor_dbm", rf->power.floor_dbm},
		{"temperature_centi_c", rf->temperature_centi_c},
		{"tx_frames", link->stats.tx_frames},
		{"tx_frame_builds", link->stats.tx_frame_builds},
		{"tx_auth_tags", link->stats.tx_auth_tags},
		{"tx_cancelled_frames", link->stats.tx_cancelled_frames},
		{"tx_failed_frames", link->stats.tx_failed_frames},
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

	return encode_fields(zs, fields, ARRAY_SIZE(fields)) && zcbor_tstr_put_lit(zs, "dev_id") &&
	       zcbor_bstr_encode_ptr(zs, link->dev_id, ZLRS_DEV_ID_LEN) &&
	       zcbor_tstr_put_lit(zs, "peer_id") &&
	       zcbor_bstr_encode_ptr(zs, link->pairing.peer_id, ZLRS_DEV_ID_LEN);
}

static bool encode_status(zcbor_state_t *zs)
{
	const zlrs_rf_state_t *rf = &request.reply.status.rf;
	bool ok = zcbor_tstr_put_lit(zs, "uptime_ms") &&
		  zcbor_uint64_put(zs, request.reply.status.uptime_ms) &&
		  zcbor_tstr_put_lit(zs, "hardware_ms") &&
		  zcbor_uint32_put(zs, rf->hardware_sampled_ms) &&
		  zcbor_tstr_put_lit(zs, "cpu_load_permille") &&
		  zcbor_int32_put(zs, request.reply.status.cpu_load_permille) &&
		  zcbor_tstr_put_lit(zs, "cpu_window_ms") &&
		  zcbor_uint64_put(zs, request.reply.status.cpu_window_ms) &&
		  zcbor_tstr_put_lit(zs, "status") && zcbor_map_start_encode(zs, 160);

	ok = ok && encode_status_fields(zs);
#if defined(CONFIG_ZLRS_APP_DEBUG)
	if (request.len == 1U && request.value[0] != ZLRS_STATUS_BASIC) {
		ok = ok && encode_view(request.value[0], zs);
	}
#endif
	return ok && zcbor_map_end_encode(zs, 160);
}

static int encode_response(zcbor_state_t *zs, int rc)
{
#if defined(CONFIG_ZLRS_APP_DEBUG_TRACE)
	if (request.op == ZLRS_MGMT_CMD_TRACE) {
		return zlrs_trace_response(zs, rc);
	}
#endif
	if (rc == -ENOTSUP) {
		return MGMT_ERR_ENOTSUP;
	}
	bool ok = zcbor_tstr_put_lit(zs, "result") && zcbor_int32_put(zs, rc);
	if (rc != 0) {
		return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}
	const uint32_t op = request.op;
	if (op == ZLRS_MGMT_CMD_STATUS) {
		ok = ok && encode_status(zs);
	}

	if (op == ZLRS_MGMT_CMD_LOG_INFO) {
		ok = ok && zcbor_tstr_put_lit(zs, "files") &&
		     zcbor_list_start_encode(zs, CONFIG_LOG_BACKEND_FS_FILES_LIMIT);
		for (size_t i = 0; i < request.reply.logs.count; i++) {
			char path[48];
			snprintf(path, sizeof(path),
				 CONFIG_LOG_BACKEND_FS_DIR "/" CONFIG_LOG_BACKEND_FS_FILE_PREFIX
							   "%04u",
				 request.reply.logs.num[i]);
			ok = ok && zcbor_map_start_encode(zs, 2) &&
			     zcbor_tstr_put_lit(zs, "path") &&
			     zcbor_tstr_encode_ptr(zs, path, strlen(path)) &&
			     zcbor_tstr_put_lit(zs, "size") &&
			     zcbor_uint32_put(zs, request.reply.logs.size[i]) &&
			     zcbor_map_end_encode(zs, 2);
		}
		ok = ok && zcbor_list_end_encode(zs, CONFIG_LOG_BACKEND_FS_FILES_LIMIT);
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
	if (event == MGMT_EVT_OP_CMD_DONE) {
		const struct mgmt_evt_op_cmd_arg *cmd = data;
		if (cmd->group == MGMT_GROUP_ID_FS) {
			atomic_set(&fs_active, 0);
			if (cmd->id == FS_MGMT_ID_OPENED_FILE && cmd->err == 0) {
				atomic_set(&fs_handle_open, 0);
			}
		}
		return MGMT_CB_OK;
	}
	if (event == MGMT_EVT_OP_FS_MGMT_FILE_ACCESS_DONE) {
		atomic_set(&fs_handle_open, 0);
		return MGMT_CB_OK;
	}
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
		const size_t directory_len = sizeof(CONFIG_LOG_BACKEND_FS_DIR "/") - 1U;
		bool allowed = strncmp(path, CONFIG_LOG_BACKEND_FS_DIR "/", directory_len) == 0 &&
			       zlrs_log_file_id(path + directory_len) >= 0;
#if defined(CONFIG_ZLRS_APP_DEBUG_TRACE)
		allowed |= zlrs_trace_file_readable(path);
#endif
		if (!atomic_get(&session_active) || !allowed ||
		    access->access == FS_MGMT_FILE_ACCESS_WRITE) {
			*rc = MGMT_ERR_EACCESSDENIED;
			return MGMT_CB_ERROR_RC;
		}
		if (access->access == FS_MGMT_FILE_ACCESS_READ) {
			atomic_set(&fs_handle_open, 1);
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
	if (cmd->group == MGMT_GROUP_ID_FS) {
		atomic_set(&fs_active, 1);
	}
	return MGMT_CB_OK;
}

/* Event masks combine only within one MCUmgr callback group. */
static struct mgmt_callback smp_callback = {
	.callback = native_access,
	.event_id = MGMT_EVT_OP_CMD_RECV | MGMT_EVT_OP_CMD_DONE,
};
static struct mgmt_callback fs_callback = {
	.callback = native_access,
	.event_id = MGMT_EVT_OP_FS_MGMT_FILE_ACCESS | MGMT_EVT_OP_FS_MGMT_FILE_ACCESS_DONE,
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
