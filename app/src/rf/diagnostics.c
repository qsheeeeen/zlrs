/* SPDX-License-Identifier: Apache-2.0 */
#include "rf/diagnostics.h"
#include "rf/rf.h"
#include "power.h"
#include "platform/lr2021.h"
#include "platform/slot_timer.h"
#include "zlrs/mgmt/mgmt.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
#define MGMT_DEBUG_REPORT_CAPACITY 4096U

struct mgmt_debug_report_writer {
	size_t len;
	bool failed;
};

/* One management owner. Static storage avoids adding a large snapshot to its stack. */
static zlrs_rf_diagnostics_t snapshot;
static char mgmt_debug_report[MGMT_DEBUG_REPORT_CAPACITY];
static uint16_t mgmt_debug_report_len;
static uint8_t mgmt_debug_report_id;

static void mgmt_debug_append(struct mgmt_debug_report_writer *writer, const char *format, ...)
{
	va_list args;
	const size_t remaining = sizeof(mgmt_debug_report) - writer->len;
	int written;

	if (writer->failed || remaining == 0U) {
		writer->failed = true;
		return;
	}

	va_start(args, format);
	written = vsnprintf(&mgmt_debug_report[writer->len], remaining, format, args);
	va_end(args);

	if (written < 0 || (size_t)written >= remaining) {
		writer->failed = true;
		return;
	}
	writer->len += (size_t)written;
}

static const char *mgmt_debug_state(uint8_t state)
{
	switch (state) {
	case ZLRS_STATE_UNCONFIGURED:
		return "unconfigured";
	case ZLRS_STATE_UNPAIRED:
		return "unpaired";
	case ZLRS_STATE_PAIRING:
		return "pairing";
	case ZLRS_STATE_SEARCHING:
		return "searching";
	case ZLRS_STATE_CONNECTED:
		return "connected";
	default:
		return "unknown";
	}
}

static const char *mgmt_debug_bands(const zlrs_link_cfg_t *cfg)
{
	const bool primary_24 = cfg->domain == ZLRS_DOMAIN_ISM2400;
	const bool second_24 = cfg->second_domain == ZLRS_DOMAIN_ISM2400;

	if (cfg->second_domain == 0xFFU || cfg->second_domain == cfg->domain) {
		return primary_24 ? "2.4 GHz" : "sub-G";
	}
	return (primary_24 || second_24) ? "2.4 GHz + sub-G" : "sub-G";
}

static void mgmt_debug_frequency(struct mgmt_debug_report_writer *writer, uint32_t hz)
{
	if (hz >= 1000000000U) {
		mgmt_debug_append(writer, "%u.%03u GHz", hz / 1000000000U,
				  (hz % 1000000000U) / 1000000U);
	} else {
		mgmt_debug_append(writer, "%u.%03u MHz", hz / 1000000U, (hz % 1000000U) / 1000U);
	}
}

static void mgmt_debug_phy(struct mgmt_debug_report_writer *writer, const zlrs_mod_cfg_t *mod)
{
	if (mod->modulation == ZLRS_MOD_FLRC) {
		mgmt_debug_append(writer, "FLRC, %u.%03u Mbps", mod->rate_bps / 1000000U,
				  (mod->rate_bps / 1000U) % 1000U);
	} else {
		mgmt_debug_append(writer, "LoRa, SF%u / %u kHz", (unsigned int)mod->sf,
				  mod->bw_hz / 1000U);
	}
}

static void mgmt_debug_link(struct mgmt_debug_report_writer *writer, const char *name,
			    const zlrs_rf_link_snapshot_t *link)
{
	const zlrs_link_cfg_t *const cfg = &link->config;
	const zlrs_link_info_t info = link->info;

	const zlrs_link_stats_t *const stats = &link->stats;
	const zlrs_link_diagnostics_t diagnostics = link->diagnostics;
	const uint32_t frequency_hz = diagnostics.frequency_hz;

	mgmt_debug_append(writer, "\n[%s LINK]\n", name);
	mgmt_debug_append(writer, "  Hopping bands      : %s\n", mgmt_debug_bands(cfg));
	mgmt_debug_append(writer, "  Link state / role  : %-12s / %s\n",
			  mgmt_debug_state(info.state), cfg->role == ZLRS_ROLE_TX ? "TX" : "RX");
	mgmt_debug_append(writer, "  Pairing / session  : %s / %s   Profile: P%u\n",
			  info.paired ? "paired" : "not paired",
			  info.session_valid ? "active" : "inactive", (unsigned int)cfg->profile);
	mgmt_debug_append(writer, "  Frequency          : ");
	mgmt_debug_frequency(writer, frequency_hz);
	mgmt_debug_append(writer, "   Hop: %u   Frame: %u\n", (unsigned int)diagnostics.hop_index,
			  diagnostics.frame_index);
	mgmt_debug_append(writer, "  PHY / TX setpoint  : ");
	mgmt_debug_phy(writer, &cfg->mod);
	mgmt_debug_append(writer, "   /   %d dBm\n", (int)cfg->tx_power_dbm);
	mgmt_debug_append(writer, "  RF frames          : TX %u   RX %u   Failed %u\n",
			  stats->tx_frames, stats->rx_frames, stats->radio_fail);
	if (stats->rx_frames != 0U && stats->last_rssi != 0) {
		mgmt_debug_append(writer,
				  "  Signal             : RSSI %d dBm   Link quality %u%%\n",
				  (int)stats->last_rssi, (unsigned int)stats->lq_percent);
	} else if (stats->rx_frames != 0U) {
		mgmt_debug_append(writer, "  Signal             : RSSI n/a   Link quality %u%%\n",
				  (unsigned int)stats->lq_percent);
	} else {
		mgmt_debug_append(writer, "  Signal             : no received frames yet\n");
	}
	mgmt_debug_append(writer, "  RF bytes           : TX %u B   RX %u B\n", stats->tx_bytes,
			  stats->rx_bytes);
	mgmt_debug_append(writer,
			  "  DATA               : %u packets   %u user B   full %u   ACK-only %u\n",
			  stats->tx_data_frames, stats->tx_data_bytes, stats->tx_full_data_frames,
			  stats->tx_ack_only_frames);
	mgmt_debug_append(writer, "  ACK                : %u DATA ACKs   %u stale   %u ACK waits\n",
			  stats->tx_data_ack_frames, stats->tx_staged_stale_ack_frames,
			  stats->tx_arq_wait_stalls);
	mgmt_debug_append(writer,
			  "  ACK timing         : retry age %u/%u ms   confirmation %u/%u ms\n",
			  stats->tx_retry_age_ms_last, stats->tx_retry_age_ms_max,
			  stats->tx_ack_delay_ms_last, stats->tx_ack_delay_ms_max);
	mgmt_debug_append(writer, "  ACK totals         : %u ms across %u entries\n",
			  stats->tx_ack_delay_ms_sum, stats->tx_ack_entries);
	mgmt_debug_append(writer, "  DATA slots TX      : %u / %u\n", stats->slot_tx[0],
			  stats->slot_tx[1]);
	mgmt_debug_append(writer, "  DATA slots RX/hit  : %u / %u   |   %u / %u\n",
			  stats->slot_rx[0], stats->slot_rx[1], stats->slot_hit[0],
			  stats->slot_hit[1]);
	mgmt_debug_append(writer, "  DATA slot failures : %u / %u\n", stats->slot_fail[0],
			  stats->slot_fail[1]);
	mgmt_debug_append(writer, "  Pairing TX/RX      : %u/%u/%u  /  %u/%u/%u   rejected %u\n",
			  stats->pair_req_tx, stats->pair_resp_tx, stats->pair_cfm_tx,
			  stats->pair_req_rx, stats->pair_resp_rx, stats->pair_cfm_rx,
			  stats->pair_rejects);
	mgmt_debug_append(writer,
			  "  Session TX/RX      : SYNC %u/%u   response %u/%u   confirm %u/%u\n",
			  stats->tx_syncs, stats->rx_syncs, stats->sess_resp_tx,
			  stats->sess_resp_rx, stats->sess_cfm_tx, stats->sess_cfm_rx);
	mgmt_debug_append(writer,
			  "  ARQ / receive      : retries %u   long retry runs %u   dup %u   "
			  "out-of-order %u\n",
			  stats->tx_retries, stats->tx_arq_retry_runs, stats->rx_dup,
			  stats->rx_out_of_order);
	mgmt_debug_append(writer, "  Data loss          : dropped %u B\n", stats->data_drop_bytes);
	mgmt_debug_append(writer, "  Queues             : TX %u   ARQ %u\n",
			  (unsigned int)diagnostics.queue_count,
			  (unsigned int)diagnostics.arq_used);
	mgmt_debug_append(writer, "  Sequence / ACK     : TX %u   RX %u   pending %u\n",
			  (unsigned int)diagnostics.arq_seq, (unsigned int)diagnostics.arq_rx_seq,
			  (unsigned int)diagnostics.arq_ack_pending);
	mgmt_debug_append(
		writer, "  Decode errors      : tag %u   frame %u   hop advances %u   resync %u\n",
		stats->rx_bad_tag, stats->rx_bad_frame, stats->hop_advances, stats->hop_resync);
	mgmt_debug_append(writer, "  Cadence            : data 1/%u   sync 1/%u\n",
			  (unsigned int)cfg->data_every_n, (unsigned int)cfg->resync_every_n);
}

static void mgmt_debug_rate(struct mgmt_debug_report_writer *writer)
{
	const zlrs_rf_rate_sample_t *const sample = &snapshot.rate;
	const uint32_t span = sample->span_ms != 0U ? sample->span_ms : 1U;
	const uint32_t windows_x10 = (sample->wins * 10000U + span / 2U) / span;
	const uint32_t heard_x10 = (sample->hits * 10000U + span / 2U) / span;
	const uint32_t hit_x10 =
		sample->wins != 0U ? (sample->hits * 1000U + sample->wins / 2U) / sample->wins : 0U;
	const uint32_t tx_x10 = (sample->tx * 10000U + span / 2U) / span;
	mgmt_debug_append(writer, "Link      Windows/s  Heard/s  Hit rate    TX/s  State\n");
	mgmt_debug_append(writer, "Data      %6u.%u %6u.%u %7u.%u%% %5u.%u %s\n", windows_x10 / 10U,
			  windows_x10 % 10U, heard_x10 / 10U, heard_x10 % 10U, hit_x10 / 10U,
			  hit_x10 % 10U, tx_x10 / 10U, tx_x10 % 10U,
			  mgmt_debug_state(sample->state));
}

static void mgmt_debug_radio(struct mgmt_debug_report_writer *writer)
{
	zlrs_lr2021_diag_t diag;
	struct zlrs_slot_timer_stats timer;
	const uint32_t protocol_ms = zlrs_slot_timer_now_ms();
	const uint32_t kernel_ms = k_uptime_get_32();
	const uint32_t max_airtime_us =
		zlrs_lr2021_airtime_us(&snapshot.data_mod, ZLRS_FRAME_MAX_LEN);

	zlrs_lr2021_diag(&diag);
	zlrs_slot_timer_stats(&timer);
	mgmt_debug_append(writer, "[FRAME PLAN]\n");
	mgmt_debug_append(writer, "  Frame period       : %u.%03u ms\n",
			  snapshot.frame_period_us / 1000U, snapshot.frame_period_us % 1000U);
	mgmt_debug_append(writer, "  Slot budget        : %u.%03u ms   Preparation: %u.%03u ms\n",
			  snapshot.slot_budget_us / 1000U, snapshot.slot_budget_us % 1000U,
			  snapshot.slot_prepare_us / 1000U, snapshot.slot_prepare_us % 1000U);
	mgmt_debug_append(writer, "  Data MTU           : %u B   Full-frame airtime: %u.%03u ms\n",
			  (unsigned int)snapshot.data_mtu, max_airtime_us / 1000U,
			  max_airtime_us % 1000U);
	mgmt_debug_append(writer, "  Launch guard       : %u us\n",
			  (unsigned int)CONFIG_ZLRS_APP_TX_GUARD_US);

	mgmt_debug_append(writer, "\n[CLOCK / TIMER]\n");
	mgmt_debug_append(writer, "  TIMER1 / kernel    : %u / %u ms   Delta: %+d ms\n",
			  protocol_ms, kernel_ms, (int32_t)(protocol_ms - kernel_ms));
	mgmt_debug_append(writer, "  Boundaries         : %u   IRQ max lateness: %u us\n",
			  timer.boundaries, timer.irq_late_us_max);
	mgmt_debug_append(writer, "  Missed / skipped   : %u boundaries   %u frames   %u slots\n",
			  timer.missed_boundaries, snapshot.skipped_frames, snapshot.missed_slots);
	mgmt_debug_append(
		writer, "  Prepare wait       : skipped %u   late %u us   IRQ %u us   wake %u us\n",
		snapshot.prepare_wait_skipped, snapshot.prepare_wait_late_us_max,
		snapshot.prepare_irq_late_us_max, snapshot.prepare_wake_us_max);
	mgmt_debug_append(
		writer, "  Phase correction   : error %+d us   applied %+d us   hits %u\n",
		(int)snapshot.phase_err_us, (int)snapshot.phase_last_us, snapshot.phase_hits);

	mgmt_debug_append(writer, "\n[RADIO DRIVER]\n");
	mgmt_debug_append(writer, "  RAL transactions   : %u   TX done %u   RX packets %u\n",
			  diag.transactions, diag.tx_done, diag.rx_packets);
	mgmt_debug_append(writer, "  Overrun / late     : %u / %u   Empty IRQ: %u\n", diag.overruns,
			  diag.late_starts, diag.empty_irq);
	mgmt_debug_append(writer, "  Setup last / max   : %u / %u us   TX %u us   RX %u us\n",
			  diag.submit_us_last, diag.submit_us_max, diag.tx_us_last,
			  diag.rx_us_last);
	mgmt_debug_append(writer, "  IRQ status         : 0x%08x   Stale IRQs: %u (0x%08x)\n",
			  diag.irq_last, diag.stale_irq, diag.stale_irq_bits);

	mgmt_debug_append(writer, "\n[TRANSMIT TIMING]\n");
	mgmt_debug_append(writer, "  Late launch        : %u attempts   last / max %u / %u us\n",
			  diag.prepare_late_starts + diag.trigger_late_starts,
			  diag.launch_late_us_last, diag.launch_late_us_max);
	mgmt_debug_append(writer, "  Late by phase      : prepare %u   trigger %u\n",
			  diag.prepare_late_starts, diag.trigger_late_starts);
	mgmt_debug_append(writer, "  Launch IRQ delay   : last / max %u / %u us   Late IRQ %u\n",
			  diag.launch_irq_late_us_last, diag.launch_irq_late_us_max,
			  diag.late_trigger_irq);
	mgmt_debug_append(writer, "  Launch wake delay  : last / max %u / %u us   Late wake %u\n",
			  diag.launch_wake_us_last, diag.launch_wake_us_max,
			  diag.late_trigger_wake);
	mgmt_debug_append(writer, "  Preparation cost   : entry %u us   build %u us   SPI %u us\n",
			  snapshot.late_prepare_entry_us, snapshot.late_prepare_build_us,
			  snapshot.late_prepare_spi_us);
	mgmt_debug_append(
		writer, "  Launch command     : last / max %u / %u us   FIFO lead %u us\n",
		diag.launch_cmd_us_last, diag.launch_cmd_us_max, diag.preload_lead_us_last);
	mgmt_debug_append(writer, "  Jitter / startup   : %u / %u us   TX startup %u us\n",
			  diag.launch_jitter_us_last, diag.launch_jitter_us_max,
			  diag.tx_startup_us_last);
	mgmt_debug_append(writer, "  TX command         : last / max %u / %u us\n",
			  diag.tx_launch_cmd_us_last, diag.tx_launch_cmd_us_max);
	mgmt_debug_append(
		writer,
		"  TX setup phases    : standby/IRQ %u   modem %u   FIFO %u   FS/BUSY %u us\n",
		diag.tx_prepare_us_last[0], diag.tx_prepare_us_last[1], diag.tx_prepare_us_last[2],
		diag.tx_prepare_us_last[3]);
	mgmt_debug_append(writer, "  FIFO preload       : %u packets   %u used   %u / %u us\n",
			  diag.tx_preloaded, diag.tx_preload_used, diag.tx_preload_us_last,
			  diag.tx_preload_us_max);
	mgmt_debug_append(writer, "  FIFO tail          : %u   %u B   errors %u   underflows %u\n",
			  diag.tx_fifo_tails, diag.tx_fifo_bytes, diag.tx_fifo_errors,
			  diag.tx_fifo_underflows);
	mgmt_debug_append(writer, "  FIFO ready         : last / max %u / %u us\n",
			  diag.tx_fifo_ready_us_last, diag.tx_fifo_ready_us_max);

	mgmt_debug_append(writer, "\n[RECEIVE TIMING]\n");
	mgmt_debug_append(writer,
			  "  Decode errors      : header %u   CRC %u   length %u   payload %u\n",
			  diag.rx_header_errors, diag.rx_crc_errors, diag.rx_length_errors,
			  diag.rx_payload_errors);
	mgmt_debug_append(writer,
			  "  Window outcomes    : expired %u   incomplete %u   restarted %u\n",
			  diag.rx_expired, diag.rx_incomplete, diag.rx_restarted);
	mgmt_debug_append(writer, "  RX finish          : last / max %u / %u us\n",
			  snapshot.rx_finish_us_last, snapshot.rx_finish_us_max);
	mgmt_debug_append(writer, "  Link processing    : last / max %u / %u us\n",
			  snapshot.rx_protocol_us_last, snapshot.rx_protocol_us_max);
	mgmt_debug_append(writer, "  FIFO reads         : %u packets   %u B   %u / %u us\n",
			  diag.rx_packets, diag.rx_fifo_bytes, diag.rx_fifo_us_last,
			  diag.rx_fifo_us_max);
	mgmt_debug_append(writer, "  IRQ service        : last / max %u / %u us   TX expired %u\n",
			  diag.irq_service_us_last, diag.irq_service_us_max, diag.tx_expired);

	mgmt_debug_append(writer, "\n[BUILD / RADIO HEALTH]\n");
	mgmt_debug_append(writer, "  Serial / protocol  : %u / %u us\n",
			  snapshot.serial_pump_us_last, snapshot.protocol_build_us_last);
	mgmt_debug_append(writer, "  Packet build       : last / max %u / %u us\n",
			  snapshot.packet_build_us_last, snapshot.packet_build_us_max);
	mgmt_debug_append(writer, "  FS reused          : %u   TX startup max: %u us\n",
			  diag.fs_reused, diag.tx_startup_us_max);
	mgmt_debug_append(writer, "  Chip errors        : 0x%04x\n",
			  (unsigned int)diag.chip_errors);
	if (diag.chip_error_freq_hz != 0U) {
		mgmt_debug_append(writer, "  Last error channel : ");
		mgmt_debug_frequency(writer, diag.chip_error_freq_hz);
		mgmt_debug_append(writer, "\n");
	} else {
		mgmt_debug_append(writer, "  Last error channel : none recorded\n");
	}
}

static int mgmt_debug_render(uint8_t report_id, struct mgmt_debug_report_writer *writer)
{
	switch (report_id) {
	case ZLRS_MGMT_DEBUG_REPORT_LINK:
		mgmt_debug_link(writer, "DATA", &snapshot.link);
		return ZLRS_MGMT_OK;
	case ZLRS_MGMT_DEBUG_REPORT_RATE:
		mgmt_debug_rate(writer);
		return ZLRS_MGMT_OK;
	case ZLRS_MGMT_DEBUG_REPORT_RADIO:
		mgmt_debug_radio(writer);
		return ZLRS_MGMT_OK;
	case ZLRS_MGMT_DEBUG_REPORT_POWER: {
		mgmt_debug_append(writer, "[POWER LIMITS]\n");
		mgmt_debug_append(writer, "  Configured target  : %+d dBm\n",
				  (int)snapshot.tx_power_dbm);
		mgmt_debug_append(writer, "  Active data band   : %s\n",
				  mgmt_debug_bands(&snapshot.link.config));
		mgmt_debug_append(writer, "  Radio setpoint     : %+d dBm\n",
				  (int)snapshot.link.config.tx_power_dbm);
		mgmt_debug_append(writer,
				  "  Dynamic power      : %+d dBm (up %u, down %u, fallback %u)\n",
				  (int)snapshot.power.current_dbm, snapshot.power.raises,
				  snapshot.power.lowers, snapshot.power.fallbacks);
		mgmt_debug_append(writer, "  Peer feedback      : RSSI %d dBm, LQ %u%%\n",
				  (int)snapshot.link.info.peer_rssi_dbm,
				  (unsigned int)snapshot.link.info.peer_lq_percent);
		mgmt_debug_append(writer, "  Region cap         : %s / %+d dBm\n",
				  snapshot.region != NULL ? snapshot.region->name : "unknown",
				  snapshot.region != NULL ? (int)snapshot.region->max_tx_power_dbm
							  : 0);
		mgmt_debug_append(writer, "  RF output is not measured.\n");
		return ZLRS_MGMT_OK;
	}
	default:
		return ZLRS_MGMT_E_PARAM;
	}
}

int zlrs_rf_debug_report(uint8_t report_id, uint16_t offset, uint8_t *out, uint8_t capacity,
			 uint16_t *total_len, uint8_t *written)
{
	if (out == NULL || total_len == NULL || written == NULL ||
	    capacity > ZLRS_MGMT_DEBUG_REPORT_CHUNK_MAX) {
		return ZLRS_MGMT_E_PARAM;
	}

	if (offset == 0U) {
		zlrs_rf_diagnostics_capture(&snapshot);
		struct mgmt_debug_report_writer writer = {0};
		const int rc = mgmt_debug_render(report_id, &writer);

		if (rc != ZLRS_MGMT_OK) {
			return rc;
		}
		if (writer.failed || writer.len > UINT16_MAX) {
			return ZLRS_MGMT_E_STATE;
		}
		mgmt_debug_report_len = (uint16_t)writer.len;
		mgmt_debug_report_id = report_id;
	} else if (report_id != mgmt_debug_report_id || offset > mgmt_debug_report_len) {
		return ZLRS_MGMT_E_PARAM;
	}

	const uint16_t remaining = mgmt_debug_report_len - offset;
	const uint8_t chunk_len = (remaining < capacity) ? (uint8_t)remaining : capacity;

	memcpy(out, &mgmt_debug_report[offset], chunk_len);
	*total_len = mgmt_debug_report_len;
	*written = chunk_len;

	return ZLRS_MGMT_OK;
}
#endif
