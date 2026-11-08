/* SPDX-License-Identifier: Apache-2.0 */
#include "diagnostics.h"
#include "platform/lr2021.h"
#include "platform/slot_timer.h"
#include "power.h"
#include "rf.h"
#include <errno.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
#define DIAGNOSTIC_REPORT_CAPACITY 4096U

/* Rendering is synchronous on the paused management owner. */
static const zlrs_rf_state_t *snapshot;

static struct {
	char text[DIAGNOSTIC_REPORT_CAPACITY];
	size_t len;
	bool failed;
} report;

static void diagnostic_append(const char *format, ...)
{
	va_list args;
	const size_t remaining = sizeof(report.text) - report.len;
	int written;

	if (report.failed || remaining == 0U) {
		report.failed = true;
		return;
	}

	va_start(args, format);
	written = vsnprintf(&report.text[report.len], remaining, format, args);
	va_end(args);

	if (written < 0 || (size_t)written >= remaining) {
		report.failed = true;
		return;
	}
	report.len += (size_t)written;
}

static const char *diagnostic_state(uint8_t state)
{
	static const char *const names[] = {
		[ZLRS_STATE_UNCONFIGURED] = "unconfigured", [ZLRS_STATE_UNPAIRED] = "unpaired",
		[ZLRS_STATE_PAIRING] = "pairing",           [ZLRS_STATE_SEARCHING] = "searching",
		[ZLRS_STATE_CONNECTED] = "connected",
	};
	return state < ARRAY_SIZE(names) && names[state] != NULL ? names[state] : "unknown";
}

static const char *diagnostic_bands(const zlrs_link_cfg_t *cfg)
{
	return cfg->region == ZLRS_REGION_ISM2400 ? "2.4 GHz" : "sub-G";
}

static void diagnostic_maintenance(void)
{
	const uint64_t elapsed =
		snapshot->paused
			? k_cyc_to_us_floor64(k_cycle_get_64() - snapshot->pause_started_cycles)
			: 0U;
	diagnostic_append("\n[MAINTENANCE / SINCE RF INIT]\n");
	diagnostic_append("  Pauses             : %u   Total %llu us   Max %llu us\n",
			  snapshot->maintenance.count,
			  (unsigned long long)(snapshot->maintenance.total_us + elapsed),
			  (unsigned long long)MAX(snapshot->maintenance.max_us, elapsed));
	diagnostic_append("  Skipped after pause: %u frames   Without pause: %u frames\n",
			  snapshot->maintenance.skipped_after_maintenance,
			  snapshot->maintenance.skipped_without_maintenance);
}

static void diagnostic_frequency(uint32_t hz)
{
	if (hz >= 1000000000U) {
		diagnostic_append("%u.%03u GHz", hz / 1000000000U, (hz % 1000000000U) / 1000000U);
	} else {
		diagnostic_append("%u.%03u MHz", hz / 1000000U, (hz % 1000000U) / 1000U);
	}
}

static void diagnostic_frame_work(const zlrs_link_stats_t *stats)
{
	diagnostic_append(
		"  Frame work         : %u builds   %u TX tags   cancelled %u   failed %u\n",
		stats->tx_frame_builds, stats->tx_auth_tags, stats->tx_cancelled_frames,
		stats->tx_failed_frames);
}

static void diagnostic_link(const char *name, const zlrs_link_status_t *link)
{
	const zlrs_link_cfg_t *const cfg = &link->cfg;

	const zlrs_link_stats_t *const stats = &link->stats;
	const uint32_t frequency_hz = zlrs_fhss_current_hz(&link->fhss);

	diagnostic_append("\n[%s LINK]\n", name);
	diagnostic_append("  Hopping bands      : %s\n", diagnostic_bands(cfg));
	diagnostic_append("  Link state / role  : %-12s / %s\n", diagnostic_state(link->state),
			  cfg->role == ZLRS_ROLE_LEADER ? "Leader" : "Follower");
	diagnostic_append("  Pairing / session  : %s / %s   Profile: P%u\n",
			  link->paired ? "paired" : "not paired",
			  link->session_valid ? "active" : "inactive", (unsigned int)cfg->profile);
	diagnostic_append("  Frequency          : ");
	diagnostic_frequency(frequency_hz);
	diagnostic_append("   Hop: %u   Frame: %u\n", (unsigned int)link->fhss.index,
			  link->frame_index);
	diagnostic_append("  PHY / TX setpoint  : %s   /   %d dBm\n", zlrs_phy_name(cfg->phy),
			  (int)cfg->tx_power_dbm);

	diagnostic_append("  RF frames          : TX %u   RX %u   Failed %u\n", stats->tx_frames,
			  stats->rx_frames, stats->radio_fail);
	if (stats->rx_frames != 0U && stats->lq_percent > 100U) {
		diagnostic_append("  Signal             : RSSI %d dBm   Link quality pending\n",
				  (int)stats->last_rssi);
	} else if (stats->rx_frames != 0U && stats->last_rssi != 0) {
		diagnostic_append("  Signal             : RSSI %d dBm   Link quality %u%%\n",
				  (int)stats->last_rssi, (unsigned int)stats->lq_percent);
	} else if (stats->rx_frames != 0U) {
		diagnostic_append("  Signal             : RSSI n/a   Link quality %u%%\n",
				  (unsigned int)stats->lq_percent);
	} else {
		diagnostic_append("  Signal             : no received frames yet\n");
	}
	diagnostic_frame_work(stats);
	diagnostic_append("  RF bytes           : TX %u B   RX %u B\n", stats->tx_bytes,
			  stats->rx_bytes);
	diagnostic_append("  DATA               : %u packets   %u user B   full %u   ACK-only %u\n",
			  stats->tx_data_frames, stats->tx_data_bytes, stats->tx_full_data_frames,
			  stats->tx_ack_only_frames);
	diagnostic_append("  ACK                : %u DATA ACKs   %u ACK waits\n",
			  stats->tx_data_ack_frames, stats->tx_arq_wait_stalls);
	diagnostic_append("  ACK timing         : retry age %u/%u ms   confirmation %u/%u ms\n",
			  stats->tx_retry_age_ms.last, stats->tx_retry_age_ms.max,
			  stats->tx_ack_delay_ms.last, stats->tx_ack_delay_ms.max);
	diagnostic_append("  ACK totals         : %u ms across %u entries\n",
			  stats->tx_ack_delay_ms_sum, stats->tx_ack_entries);
	diagnostic_append("  DATA slots TX      : %u / %u\n", stats->slot_tx[0], stats->slot_tx[1]);
	diagnostic_append("  DATA slots RX/hit  : %u / %u   |   %u / %u\n", stats->slot_rx[0],
			  stats->slot_rx[1], stats->slot_hit[0], stats->slot_hit[1]);
	diagnostic_append("  DATA slot failures : %u / %u\n", stats->slot_fail[0],
			  stats->slot_fail[1]);
	diagnostic_append("  Pairing TX/RX      : %u/%u/%u  /  %u/%u/%u   rejected %u\n",
			  stats->pair_req_tx, stats->pair_resp_tx, stats->pair_cfm_tx,
			  stats->pair_req_rx, stats->pair_resp_rx, stats->pair_cfm_rx,
			  stats->pair_rejects);
	diagnostic_append("  Session TX/RX      : SYNC %u/%u   response %u/%u   confirm %u/%u\n",
			  stats->tx_syncs, stats->rx_syncs, stats->sess_resp_tx,
			  stats->sess_resp_rx, stats->sess_cfm_tx, stats->sess_cfm_rx);
	diagnostic_append("  ARQ / receive      : retries %u   long retry runs %u   dup %u   "
			  "out-of-order %u\n",
			  stats->tx_retries, stats->tx_arq_retry_runs, stats->rx_dup,
			  stats->rx_out_of_order);
	diagnostic_append("  Data loss          : dropped %u B\n", stats->data_drop_bytes);
	diagnostic_append("  Queues             : TX %u   ARQ %u\n",
			  (unsigned int)link->queue_count, (unsigned int)link->arq_used);
	diagnostic_append("  Sequence / ACK     : TX %u   RX %u   pending %u\n",
			  (unsigned int)link->arq_seq, (unsigned int)link->arq_rx_seq,
			  (unsigned int)link->arq_ack_pending);
	diagnostic_append(
		"  Decode errors      : tag %u   frame %u   hop advances %u   resync %u\n",
		stats->rx_bad_tag, stats->rx_bad_frame, stats->hop_advances, stats->hop_resync);
	diagnostic_append("  Periodic SYNC      : every %u frames\n",
			  (unsigned int)cfg->resync_every_n);
}

static void diagnostic_rate(void)
{
	const zlrs_rf_rate_sample_t *const sample = &snapshot->rate;
	const uint32_t span = sample->span_ms != 0U ? sample->span_ms : 1U;
	const uint32_t windows_x10 = (sample->wins * 10000U + span / 2U) / span;
	const uint32_t heard_x10 = (sample->hits * 10000U + span / 2U) / span;
	const uint32_t hit_x10 =
		sample->wins != 0U ? (sample->hits * 1000U + sample->wins / 2U) / sample->wins : 0U;
	const uint32_t tx_x10 = (sample->tx * 10000U + span / 2U) / span;
	diagnostic_append("Link      Windows/s  Heard/s  Hit rate    TX/s  State\n");
	diagnostic_append("Data      %6u.%u %6u.%u %7u.%u%% %5u.%u %s\n", windows_x10 / 10U,
			  windows_x10 % 10U, heard_x10 / 10U, heard_x10 % 10U, hit_x10 / 10U,
			  hit_x10 % 10U, tx_x10 / 10U, tx_x10 % 10U,
			  diagnostic_state(sample->state));
}

static void diagnostic_radio(void)
{
	const zlrs_lr2021_diag_t *diag;
	struct zlrs_slot_timer_stats timer;
	const uint32_t protocol_ms = zlrs_slot_timer_now_ms();
	const uint32_t kernel_ms = k_uptime_get_32();
	const uint32_t max_airtime_us = zlrs_lr2021_airtime_us(
		snapshot->link->cfg.phy, snapshot->link->cfg.data_mtu + ZLRS_FRAME_HDR_LEN +
						 ZLRS_DATA_PREFIX_LEN +
						 zlrs_profile_tag_len(snapshot->link->cfg.profile));

	diag = zlrs_lr2021_diag();
	zlrs_slot_timer_stats(&timer);
	diagnostic_append("[FRAME PLAN]\n");
	diagnostic_append("  Modulation         : %s\n", zlrs_phy_name(snapshot->link->cfg.phy));
	diagnostic_append("  Frame period       : %u.%03u ms\n", snapshot->frame_period_us / 1000U,
			  snapshot->frame_period_us % 1000U);
	diagnostic_append("  Slot budget        : %u.%03u ms   Preparation: %u.%03u ms\n",
			  (snapshot->frame_period_us / ZLRS_SLOT_COUNT) / 1000U,
			  (snapshot->frame_period_us / ZLRS_SLOT_COUNT) % 1000U,
			  snapshot->slot_prepare_us / 1000U, snapshot->slot_prepare_us % 1000U);
	diagnostic_append("  Data MTU           : %u B   Full-frame airtime: %u.%03u ms\n",
			  (unsigned int)snapshot->link->cfg.data_mtu, max_airtime_us / 1000U,
			  max_airtime_us % 1000U);
	diagnostic_append("  Launch guard       : %u us\n",
			  (unsigned int)CONFIG_ZLRS_APP_TX_GUARD_US);

	diagnostic_maintenance();
	diagnostic_append("\n[CLOCK / TIMER / LIFETIME MAXIMA]\n");
	diagnostic_append("  TIMER1 / kernel    : %u / %u ms   Delta: %+d ms\n", protocol_ms,
			  kernel_ms, (int32_t)(protocol_ms - kernel_ms));
	diagnostic_append("  Boundaries         : %u   IRQ max lateness: %u us\n", timer.boundaries,
			  timer.irq_late_us_max);
	diagnostic_append("  Missed / skipped   : %u boundaries   %u frames   %u slots\n",
			  timer.missed_boundaries, snapshot->skipped_frames,
			  snapshot->missed_slots);
	diagnostic_append("  Phase correction   : error %+d us   applied %+d us   hits %u\n",
			  (int)snapshot->phase_err_us, (int)snapshot->phase_last_us,
			  snapshot->phase_hits);

	diagnostic_append("\n[RADIO DRIVER]\n");
	diagnostic_append("  RAL transactions   : %u   TX done %u   RX packets %u\n",
			  diag->transactions, diag->tx_done, diag->rx_packets);
	diagnostic_append("  Overrun / late     : %u / %u   Empty IRQ: %u\n", diag->overruns,
			  diag->late_starts, diag->empty_irq);
	diagnostic_append("  Setup last / max   : %u / %u us   TX %u us   RX %u us\n",
			  diag->submit_us.last, diag->submit_us.max, diag->tx_us_last,
			  diag->rx_us_last);
	diagnostic_append("  IRQ status         : 0x%08x   Stale IRQs: %u (0x%08x)\n",
			  diag->irq_last, diag->stale_irq, diag->stale_irq_bits);

	diagnostic_append("\n[TRANSMIT TIMING]\n");
	diagnostic_frame_work(&snapshot->link->stats);
	diagnostic_append("  Frame finalization : last / max %u / %u us   RX accept %u / %u us\n",
			  snapshot->work.tx_finalize_us.last, snapshot->work.tx_finalize_us.max,
			  snapshot->work.rx_accept_us.last, snapshot->work.rx_accept_us.max);
	diagnostic_append("  Full preparation   : last / max %u / %u us\n",
			  snapshot->work.tx_prepare_us.last, snapshot->work.tx_prepare_us.max);
	diagnostic_append("  Hash contention    : TX skips %u   RX drops %u\n",
			  snapshot->work.hash_busy_tx, snapshot->work.hash_busy_rx);
	diagnostic_append("  Late launch        : %u attempts   last / max %u / %u us\n",
			  diag->prepare_late_starts + diag->trigger_late_starts,
			  diag->launch_late_us.last, diag->launch_late_us.max);
	diagnostic_append("  Late by phase      : prepare %u   trigger %u\n",
			  diag->prepare_late_starts, diag->trigger_late_starts);
	diagnostic_append("  Launch IRQ delay   : last / max %u / %u us   Late IRQ %u\n",
			  diag->launch_irq_late_us.last, diag->launch_irq_late_us.max,
			  diag->late_trigger_irq);
	diagnostic_append("  Preparation cost   : entry %u us   build %u us   SPI %u us\n",
			  snapshot->late_prepare_entry_us, snapshot->late_prepare_build_us,
			  snapshot->late_prepare_spi_us);
	diagnostic_append("  Launch command     : last / max %u / %u us   FIFO lead %u us\n",
			  diag->launch_cmd_us.last, diag->launch_cmd_us.max,
			  diag->tx_ready_lead_us_last);
	diagnostic_append("  Jitter / startup   : %u / %u us   TX startup %u us\n",
			  diag->launch_jitter_us.last, diag->launch_jitter_us.max,
			  diag->tx_startup_us.last);
	diagnostic_append("  TX command         : last / max %u / %u us\n",
			  diag->tx_launch_cmd_us.last, diag->tx_launch_cmd_us.max);
	diagnostic_append(
		"  TX setup phases    : standby/IRQ %u   modem %u   FIFO %u   FS/BUSY %u us\n",
		diag->tx_stage_us[0], diag->tx_stage_us[1], diag->tx_stage_us[2],
		diag->tx_stage_us[3]);
	diagnostic_append("  FIFO loads         : %u   %u B   errors %u   underflows %u\n",
			  diag->tx_fifo_loads, diag->tx_fifo_bytes, diag->tx_fifo_errors,
			  diag->tx_fifo_underflows);
	diagnostic_append("  FIFO load time     : last / max %u / %u us\n",
			  diag->tx_fifo_load_us.last, diag->tx_fifo_load_us.max);

	diagnostic_append("\n[RECEIVE TIMING]\n");
	diagnostic_append("  Decode errors      : header %u   CRC %u   length %u   payload %u\n",
			  diag->rx_header_errors, diag->rx_crc_errors, diag->rx_length_errors,
			  diag->rx_payload_errors);
	diagnostic_append("  Window outcomes    : expired %u   incomplete %u   restarted %u\n",
			  diag->rx_expired, diag->rx_incomplete, diag->rx_restarted);
	diagnostic_append("  FIFO reads         : %u packets   %u B   %u / %u us\n",
			  diag->rx_packets, diag->rx_fifo_bytes, diag->rx_fifo_us.last,
			  diag->rx_fifo_us.max);
	diagnostic_append("  IRQ service        : last / max %u / %u us   TX expired %u\n",
			  diag->irq_service_us.last, diag->irq_service_us.max, diag->tx_expired);

	diagnostic_append("\n[BUILD / RADIO HEALTH]\n");
	diagnostic_append("  FS reused          : %u   TX startup max: %u us\n", diag->fs_reused,
			  diag->tx_startup_us.max);
	diagnostic_append("  Chip errors        : 0x%04x\n", (unsigned int)diag->chip_errors);
	if (diag->chip_error_freq_hz != 0U) {
		diagnostic_append("  Last error channel : ");
		diagnostic_frequency(diag->chip_error_freq_hz);
		diagnostic_append("\n");
	} else {
		diagnostic_append("  Last error channel : none recorded\n");
	}
}

int zlrs_diagnostics_report(zlrs_diagnostic_report_t report_id, const char **text)
{
	if ((unsigned int)report_id > ZLRS_DIAGNOSTIC_POWER) {
		return -EINVAL;
	}
	snapshot = zlrs_rf_state();
	report.len = 0;
	report.failed = false;
	switch (report_id) {
	case ZLRS_DIAGNOSTIC_LINK:
		diagnostic_link("DATA", snapshot->link);
		break;
	case ZLRS_DIAGNOSTIC_RATE:
		diagnostic_rate();
		break;
	case ZLRS_DIAGNOSTIC_RADIO:
		diagnostic_radio();
		break;
	case ZLRS_DIAGNOSTIC_POWER: {
		diagnostic_append("[POWER LIMITS]\n");
		diagnostic_append("  Effective range    : %+d..%+d dBm\n",
				  (int)snapshot->power.floor_dbm, (int)snapshot->power.ceiling_dbm);
		diagnostic_append("  Active data band   : %s\n",
				  diagnostic_bands(&snapshot->link->cfg));
		diagnostic_append("  Radio setpoint     : %+d dBm\n",
				  (int)snapshot->link->cfg.tx_power_dbm);
		diagnostic_append("  Dynamic power      : %+d dBm (up %u, down %u, fallback %u)\n",
				  (int)snapshot->power.current_dbm, snapshot->power.raises,
				  snapshot->power.lowers, snapshot->power.fallbacks);
		if (snapshot->link->peer_lq_percent <= 100U) {
			diagnostic_append("  Peer feedback      : RSSI %d dBm, LQ %u%%\n",
					  (int)snapshot->link->peer_rssi_dbm,
					  (unsigned int)snapshot->link->peer_lq_percent);
		} else {
			diagnostic_append("  Peer feedback      : RSSI %d dBm, LQ pending\n",
					  (int)snapshot->link->peer_rssi_dbm);
		}
		diagnostic_append("  Quality generation : local %u, peer %u (RAM only)\n",
				  snapshot->link->stats.lq_generation,
				  snapshot->link->peer_lq_generation);
		diagnostic_append("  Filtered RSSI      : %d dBm%s\n",
				  (int)(snapshot->power.rssi_q8 / 256),
				  snapshot->power.filtering ? "" : " (settling)");
		diagnostic_append("  SubG region cap    : %s / %+d dBm\n",
				  snapshot->region != NULL ? snapshot->region->name : "unknown",
				  snapshot->region != NULL ? (int)snapshot->region->max_tx_power_dbm
							   : 0);
		diagnostic_append("  Control mode       : %s\n",
				  snapshot->power_fixed ? "debug fixed" : "automatic");
		diagnostic_append("  RF output is not measured.\n");
		break;
	}
	default:
		return -EINVAL;
	}
	if (report.failed) {
		return -ENOSPC;
	}
	*text = report.text;
	return 0;
}
#endif
