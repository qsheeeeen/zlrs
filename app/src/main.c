/* SPDX-License-Identifier: Apache-2.0 */

#include "rf/rf.h"
#include "config/config.h"
#include "crsf/crsf.h"
#include "data/data.h"
#include "mgmt/mode.h"
#include "platform/caps.h"
#include "zlrs/proto/limits.h"

int main(void)
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
	return zlrs_rf_run();
}
