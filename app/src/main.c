/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include <string.h>

#include "boot.h"
#include "persist.h"
#include "serial.h"
#include "mgmt.h"
#include "platform/watchdog.h"
#include "rf.h"
#include "ui.h"

LOG_MODULE_REGISTER(zlrs_main, CONFIG_APP_LOG_LEVEL);

/* Last pairing acknowledged as saved; deleting its NVS record must not re-save the old link. */
static zlrs_persist_pairing_t saved_pairing;
static bool pairing_saved;

static void enter_management_mode(void)
{
	zlrs_rf_pause();
	LOG_WRN("entering management mode");
	zlrs_rf_log_snapshot();
	const int rc = zlrs_mgmt_run();
	if (rc != 0) {
		LOG_WRN("management mode ended without EXIT (rc %d)", rc);
	}
	zlrs_rf_management_return();
	LOG_INF("management mode done, back to normal");
	log_flush();
	zlrs_rf_resume();
}

static void save_pairing(void)
{
	zlrs_persist_pairing_t pairing;
	if (!zlrs_rf_pairing(pairing.peer_id, pairing.pair_salt)) {
		return;
	}
	if (pairing_saved &&
	    memcmp(pairing.peer_id, saved_pairing.peer_id, sizeof(pairing.peer_id)) == 0 &&
	    memcmp(pairing.pair_salt, saved_pairing.pair_salt, sizeof(pairing.pair_salt)) == 0) {
		return;
	}
	zlrs_rf_pause();
	const int rc = zlrs_persist_save_pairing(&pairing);
	zlrs_rf_resume();
	if (rc == 0) {
		saved_pairing = pairing;
		pairing_saved = true;
		LOG_INF("paired with %02x%02x%02x%02x", pairing.peer_id[0], pairing.peer_id[1],
			pairing.peer_id[2], pairing.peer_id[3]);
	} else {
		LOG_ERR("pairing save failed (%d); will retry at the next frame boundary", rc);
	}
}

int main(void)
{
	const int persist_rc = zlrs_persist_init();
	(void)zlrs_mgmt_init();
	const zlrs_persist_state_t *const saved = zlrs_persist_state();
	saved_pairing = saved->pairing;
	pairing_saved = saved->paired;
	const bool rf_ready = persist_rc == 0 && zlrs_rf_init(saved) == 0;
	if (rf_ready) {
		(void)zlrs_serial_init(saved->settings.data_baud);
	} else {
		LOG_ERR("fault state: not transmitting; management port stays available for "
			"flashing");
	}
	log_flush();
	if (rf_ready) {
		zlrs_rf_start();
	}
	uint32_t button_holds_seen = 0U;

	for (;;) {
		zlrs_watchdog_feed();
		zlrs_rf_publish_state();
		zlrs_serial_publish_state();
		zlrs_boot_poll(!rf_ready);
		if (zlrs_mgmt_poll_enter()) {
			enter_management_mode();
			continue;
		}
		struct zlrs_input asked;
		if (zbus_chan_read(&zlrs_input_chan, &asked, K_NO_WAIT) == 0 &&
		    asked.button_holds != button_holds_seen) {
			button_holds_seen = asked.button_holds;
			(void)zlrs_rf_pair_start();
		}
		if (rf_ready) {
			zlrs_rf_frame();
			save_pairing();
		} else {
			k_msleep(20);
		}
	}
}
