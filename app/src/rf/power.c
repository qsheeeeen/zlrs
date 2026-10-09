/*
 * Dynamic TX power policy and application wiring.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rf/power.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zlrs_link_power, CONFIG_APP_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
struct power_ctl {
	int8_t min_dbm;
	int8_t max_dbm;
	int8_t step_db;
	int8_t rssi_low_dbm;
	int8_t rssi_high_dbm;
	uint8_t lq_low;
	uint8_t lq_high;
	uint8_t windows;
	uint32_t hold_ms;
	uint32_t tlm_timeout_ms;
	int8_t current_dbm;
	uint8_t good;
	uint8_t bad;
	uint32_t last_change_ms;
	uint32_t last_tlm_ms;
	bool tlm_seen;
	uint32_t raises;
	uint32_t lowers;
	uint32_t fallbacks;
};

static int8_t clamp_dbm(const struct power_ctl *ctl, int value)
{
	if (value < ctl->min_dbm) {
		return ctl->min_dbm;
	}
	if (value > ctl->max_dbm) {
		return ctl->max_dbm;
	}

	return (int8_t)value;
}

static void power_init(struct power_ctl *ctl, int8_t configured_dbm, int8_t floor_dbm,
		       uint32_t now_ms)
{
	ctl->max_dbm = configured_dbm;
	ctl->min_dbm = (floor_dbm < configured_dbm) ? floor_dbm : configured_dbm;
	ctl->step_db = 3;
	/* Placeholder thresholds: calibrate against measured RSSI/LQ versus packet-error curves. */
	ctl->rssi_low_dbm = -95;
	ctl->rssi_high_dbm = -70;
	ctl->lq_low = 90U;
	ctl->lq_high = 98U;
	ctl->windows = 3U;
	ctl->hold_ms = 2000U;
	ctl->tlm_timeout_ms = 1000U;
	ctl->current_dbm = configured_dbm;
	ctl->good = 0U;
	ctl->bad = 0U;
	ctl->last_change_ms = now_ms;
	ctl->last_tlm_ms = now_ms;
	ctl->tlm_seen = false;
	ctl->raises = 0U;
	ctl->lowers = 0U;
	ctl->fallbacks = 0U;
}

static bool power_update(struct power_ctl *ctl, bool valid, int16_t rssi_dbm, uint8_t lq_percent,
			 uint32_t now_ms, int8_t *out_dbm)
{
	if (valid) {
		/* Fresh telemetry judges quality; only the polling path checks the timeout. */
		ctl->tlm_seen = true;
		ctl->last_tlm_ms = now_ms;
	} else {
		if (!ctl->tlm_seen || (uint32_t)(now_ms - ctl->last_tlm_ms) < ctl->tlm_timeout_ms) {
			return false;
		}

		ctl->good = 0U;
		ctl->bad = 0U;
		if (ctl->current_dbm == ctl->max_dbm) {
			return false;
		}

		/* Without telemetry, prefer link margin over power savings. */
		ctl->current_dbm = ctl->max_dbm;
		ctl->fallbacks++;
		ctl->last_change_ms = now_ms;
		if (out_dbm != NULL) {
			*out_dbm = ctl->current_dbm;
		}
		return true;
	}

	/* The hold interval must elapse before a new set of quality windows can count. */
	if ((uint32_t)(now_ms - ctl->last_change_ms) < ctl->hold_ms) {
		ctl->good = 0U;
		ctl->bad = 0U;
		return false;
	}

	if (rssi_dbm < ctl->rssi_low_dbm || lq_percent < ctl->lq_low) {
		ctl->bad++;
		ctl->good = 0U;
	} else if (rssi_dbm > ctl->rssi_high_dbm && lq_percent >= ctl->lq_high) {
		ctl->good++;
		ctl->bad = 0U;
	} else {
		ctl->good = 0U;
		ctl->bad = 0U;
		return false;
	}

	if (ctl->bad >= ctl->windows) {
		const int8_t next = clamp_dbm(ctl, (int)ctl->current_dbm + ctl->step_db);

		if (next == ctl->current_dbm) {
			ctl->bad = 0U;
			return false;
		}
		ctl->current_dbm = next;
		ctl->raises++;
	} else if (ctl->good >= ctl->windows) {
		const int8_t next = clamp_dbm(ctl, (int)ctl->current_dbm - ctl->step_db);

		if (next == ctl->current_dbm) {
			ctl->good = 0U;
			return false;
		}
		ctl->current_dbm = next;
		ctl->lowers++;
	} else {
		return false;
	}

	ctl->last_change_ms = now_ms;
	ctl->good = 0U;
	ctl->bad = 0U;
	if (out_dbm != NULL) {
		*out_dbm = ctl->current_dbm;
	}

	return true;
}
static struct power_ctl power_ctl;

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
	power_init(&power_ctl, ceiling_dbm, floor_dbm, 0U);
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
	if (power_update(&power_ctl, fresh, tlm_latest.rssi_dbm, tlm_latest.lq, now_ms, &next)) {
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
		*current_dbm = power_ctl.current_dbm;
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
