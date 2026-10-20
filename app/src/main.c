/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include <errno.h>

#include "boot.h"
#include "persist.h"
#include "pairing_save.h"
#include "serial.h"
#include "mgmt.h"
#include "platform/watchdog.h"
#include "rf.h"
#include "ui.h"

LOG_MODULE_REGISTER(zlrs_main, CONFIG_APP_LOG_LEVEL);

/* A physical request must not start pairing long after the user has left. */
#define PAIR_REQUEST_WAIT_MS 30000U

static void enter_management_mode(void)
{
	zlrs_rf_pause();
	LOG_WRN("entering management mode");
	zlrs_rf_log_snapshot();
	const int rc = zlrs_mgmt_run();
	if (rc != 0) {
		LOG_WRN("management mode ended without EXIT (rc %d)", rc);
	}
	LOG_INF("management mode done, back to normal");
	log_flush();
	zlrs_rf_resume();
}

static void save_pairing(zlrs_pairing_save_t *state)
{
	zlrs_persist_pairing_t pairing;
	const bool paired = zlrs_rf_pairing(pairing.peer_id, pairing.pair_salt);
	zlrs_persist_pairing_t saved_pairing;
	const bool saved = zlrs_persist_get_pairing(&saved_pairing);
	if (!zlrs_pairing_save_due(state, paired ? &pairing : NULL, saved ? &saved_pairing : NULL,
				   k_uptime_get_32())) {
		return;
	}
	zlrs_rf_pause();
	const int rc = zlrs_persist_save_pairing(&pairing);
	zlrs_rf_resume();
	const uint32_t retry_ms = zlrs_pairing_save_complete(state, rc == 0, k_uptime_get_32());
	if (rc == 0) {
		LOG_INF("paired with %02x%02x%02x%02x", pairing.peer_id[0], pairing.peer_id[1],
			pairing.peer_id[2], pairing.peer_id[3]);
	} else {
		LOG_ERR("pairing save failed (%d); retry in %u ms", rc, retry_ms);
	}
}

int main(void)
{
	const int persist_rc = zlrs_persist_init();
	(void)zlrs_mgmt_init();
	const bool rf_ready = persist_rc == 0 && zlrs_rf_init() == 0;
	if (rf_ready) {
		(void)zlrs_serial_init(zlrs_persist_settings()->data_baud);
	} else {
		LOG_ERR("fault state: not transmitting; management port stays available for "
			"flashing");
	}
	log_flush();
	if (rf_ready) {
		zlrs_rf_start();
	}
	uint32_t button_holds_seen = 0U;
	bool pair_requested = false;
	uint32_t pair_requested_at = 0U;
	zlrs_pairing_save_t pairing_save = {0};

	for (;;) {
		zlrs_watchdog_feed();
		zlrs_rf_publish_state();
		zlrs_serial_publish_state();
		zlrs_boot_check_rollback(!rf_ready);
		if (zlrs_mgmt_process_idle()) {
			/* Explicit management commands supersede a waiting button request. */
			pair_requested = false;
			enter_management_mode();
			continue;
		}
		struct zlrs_input asked;
		if (zbus_chan_read(&zlrs_input_chan, &asked, K_NO_WAIT) == 0 &&
		    asked.button_holds != button_holds_seen) {
			button_holds_seen = asked.button_holds;
			pair_requested = true;
			pair_requested_at = k_uptime_get_32();
		}
		if (pair_requested) {
			if ((uint32_t)(k_uptime_get_32() - pair_requested_at) >=
				    PAIR_REQUEST_WAIT_MS ||
			    zlrs_rf_pair_start() != -EAGAIN) {
				pair_requested = false;
			}
		}
		if (rf_ready) {
			zlrs_rf_frame();
			save_pairing(&pairing_save);
		} else {
			k_msleep(20);
		}
	}
}
