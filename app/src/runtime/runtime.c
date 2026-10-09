/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

#include "config/config.h"
#include "crsf/crsf.h"
#include "data/data.h"
#include "mgmt/mode.h"
#include "platform/caps.h"
#include "platform/slot_timer.h"
#include "rf/rf.h"
#include "runtime/runtime.h"
#include "runtime/update.h"
#include "zlrs/proto/limits.h"

LOG_MODULE_REGISTER(zlrs_runtime, CONFIG_APP_LOG_LEVEL);

static uint32_t reboot_ms;

static void enter_management_mode(void)
{
	zlrs_rf_pause();
	LOG_WRN("entering management mode");
	zlrs_rf_log_snapshot();
	const int rc = zlrs_mgmt_mode_run();
	if (rc != 0) {
		LOG_WRN("management mode ended without EXIT (rc %d)", rc);
	}
	zlrs_link_crsf_service_reset();
	zlrs_rf_management_return();
	LOG_INF("management mode done, back to normal");
	/* Both confirmation and log persistence write flash while RF is paused. */
	zlrs_runtime_update_poll();
	log_flush();
	zlrs_rf_resume();
}

static bool before_frame(bool fault)
{
	if (fault) {
		zlrs_runtime_update_fault();
	} else {
		zlrs_runtime_update_poll();
	}
#if ZLRS_PORT_MGMT
	if (zlrs_mgmt_poll_enter()) {
		enter_management_mode();
		return true;
	}
#endif
	return false;
}

static void after_frame(uint32_t now_ms)
{
	if (reboot_ms != 0U && (int32_t)(now_ms - reboot_ms) >= 0) {
		sys_reboot(SYS_REBOOT_COLD);
	}
}

void zlrs_runtime_request_reboot(uint32_t delay_ms)
{
	reboot_ms = zlrs_slot_timer_now_ms() + MAX(delay_ms, 1U);
}

void zlrs_runtime_cancel_reboot(void)
{
	reboot_ms = 0U;
}

int zlrs_runtime_run(void)
{
	(void)zlrs_config_init();
	(void)zlrs_mgmt_init();
	if (zlrs_rf_init() == 0) {
		const zlrs_rf_status_t rf = zlrs_rf_status();
		const zlrs_link_crsf_service_cfg_t crsf = {
			.control_rate_hz = rf.control_rate_hz,
			.frame_period_us = rf.frame_period_us,
			.rssi_reference_dbm = rf.rssi_reference_dbm,
			.enabled = ZLRS_PORT_CTRL_FIXED,
			.control_enabled = rf.control_enabled,
			.role_tx = rf.role == ZLRS_ROLE_TX,
		};
		(void)zlrs_link_data_init(zlrs_config_state().active.data_baud);
		zlrs_link_crsf_service_init(&crsf);
	}
	const zlrs_rf_owner_hooks_t hooks = {
		.before_frame = before_frame,
		.after_frame = after_frame,
	};
	return zlrs_rf_run(&hooks);
}
