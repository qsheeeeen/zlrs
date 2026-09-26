/*
 * Dynamic TX power, see link/power.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "link/power.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "zlrs/link/power.h"

LOG_MODULE_REGISTER(zlrs_link_power, CONFIG_APP_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
static zlrs_power_ctl_t power_ctl;

/* Uplink quality from the latest telemetry; the verdict runs at 1 Hz, so stash it here */
static struct {
	int16_t rssi_dbm;
	uint8_t lq;
	bool fresh;
} tlm_latest;
#endif

void zlrs_link_power_init(int8_t ceiling_dbm, int8_t floor_dbm)
{
#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
	zlrs_power_init(&power_ctl, ceiling_dbm, floor_dbm, 0U);
#else
	ARG_UNUSED(ceiling_dbm);
	ARG_UNUSED(floor_dbm);
#endif
}

void zlrs_link_power_telemetry(uint8_t rssi_neg_dbm, uint8_t lq_percent)
{
#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
	/* The frame carries the RSSI as a positive magnitude; the policy works in dBm */
	tlm_latest.rssi_dbm = -(int16_t)rssi_neg_dbm;
	tlm_latest.lq = lq_percent;
	tlm_latest.fresh = true;
#else
	ARG_UNUSED(rssi_neg_dbm);
	ARG_UNUSED(lq_percent);
#endif
}

void zlrs_link_power_tick(zlrs_link_t *control, uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
	int8_t next = 0;
	const bool fresh = tlm_latest.fresh;

	tlm_latest.fresh = false;

	/*
	 * If telemetry arrived, judge quality from its RSSI/LQ; otherwise take the valid=false
	 * timeout path, which goes back to the ceiling.
	 */
	if (zlrs_power_update(&power_ctl, fresh, tlm_latest.rssi_dbm, tlm_latest.lq, now_ms, &next)) {
		zlrs_link_set_power(control, next);
		LOG_INF("tx power -> %d dBm (rssi %d dBm, lq %u%%)", next, tlm_latest.rssi_dbm,
			tlm_latest.lq);
	}
#else
	ARG_UNUSED(control);
	ARG_UNUSED(now_ms);
#endif
}

void zlrs_link_power_stats(int8_t *current_dbm, uint32_t *raises, uint32_t *lowers,
			   uint32_t *fallbacks)
{
	if (current_dbm != NULL) {
		*current_dbm = 0;
	}
	if (raises != NULL) {
		*raises = 0U;
	}
	if (lowers != NULL) {
		*lowers = 0U;
	}
	if (fallbacks != NULL) {
		*fallbacks = 0U;
	}

#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
	if (current_dbm != NULL) {
		*current_dbm = zlrs_power_current_dbm(&power_ctl);
	}
	if (raises != NULL) {
		*raises = power_ctl.raises;
	}
	if (lowers != NULL) {
		*lowers = power_ctl.lowers;
	}
	if (fallbacks != NULL) {
		*fallbacks = power_ctl.fallbacks;
	}
#endif
}
