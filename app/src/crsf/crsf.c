/*
 * CRSF application service: device actions, routing, RC freshness and periodic reports.
 * Runs on the RF owner; serial interrupts remain in crsf/serial.c, protocol codecs in core/crsf.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "crsf/crsf.h"

#include <string.h>
#include <zephyr/app_version.h>
#include <zephyr/kernel.h>

#include "crsf/serial.h"
#include "platform/caps.h"
#include "platform/slot_timer.h"
#include "config/config.h"
#include "rf/rf.h"
#include "runtime/runtime.h"
#include "zlrs/crsf/device.h"
#include "zlrs/mgmt/mgmt.h"
#include "zlrs/region/region.h"

ZBUS_CHAN_DEFINE(zlrs_crsf_chan, struct zlrs_crsf_status, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

static struct {
	zlrs_link_crsf_service_cfg_t cfg;
	zlrs_crsf_device_t device;
	zlrs_tlm_t peer_tlm;
	uint32_t peer_tlm_ms;
	uint32_t rc_ms;
	uint32_t rc_us;
	uint32_t tlm_ms;
	uint32_t timing_ms;
	uint32_t bad_inputs;
	int32_t timing_offset_us;
	bool peer_tlm_seen;
	bool rc_seen;
	bool air_reply;
	struct {
		uint8_t frame[ZLRS_CRSF_FRAME_MAX_LEN];
		uint8_t len;
	} air_inputs[8];
	uint8_t air_head;
	uint8_t air_count;
	zlrs_link_crsf_input_t pending;
	bool have_pending;
	uint8_t reply[64];
	uint8_t reply_len;
	bool reply_air;
} service;

static void expire_input(uint32_t now_ms);

static bool is_tx(void)
{
	return service.cfg.role_tx;
}

bool zlrs_link_crsf_service_receive(void *ctx, const uint8_t *frame, uint8_t len)
{
	(void)ctx;
	if (!zlrs_crsf_frame_valid(frame, len)) {
		service.bad_inputs++;
		return true;
	}
	if (service.air_count == ARRAY_SIZE(service.air_inputs)) {
		return false;
	}
	const uint8_t tail =
		(uint8_t)((service.air_head + service.air_count) % ARRAY_SIZE(service.air_inputs));

	memcpy(service.air_inputs[tail].frame, frame, len);
	service.air_inputs[tail].len = len;
	service.air_count++;
	return true;
}

static bool crsf_emit(void *ctx, uint8_t type, const uint8_t *payload, uint8_t len)
{
	(void)ctx;
	if (type == ZLRS_CRSF_TYPE_RADIO && !service.air_reply) {
		return zlrs_link_crsf_write_latest(type, payload, len) != 0U;
	}
	uint8_t frame[64];
	const uint8_t size = zlrs_crsf_encode(frame, ZLRS_CRSF_ADDR_FC, type, payload, len);
	if (size == 0U || service.reply_len != 0U) {
		return false;
	}
	if (service.air_reply ? zlrs_rf_send_service(frame, size)
			      : zlrs_link_crsf_write_frame(frame, size)) {
		return true;
	}
	/* The action is already complete. Retain its reply, rather than executing it again. */
	memcpy(service.reply, frame, size);
	service.reply_len = size;
	service.reply_air = service.air_reply;
	return true;
}

static bool crsf_write_parameter(void *ctx, uint8_t id, int32_t value)
{
	(void)ctx;
	enum zlrs_config_key key;
	switch (id) {
	case ZLRS_CRSF_PARAM_RATE:
		key = ZLRS_CONFIG_CONTROL_RATE;
		break;
	case ZLRS_CRSF_PARAM_POWER:
		key = ZLRS_CONFIG_POWER;
		break;
	case ZLRS_CRSF_PARAM_REGION:
		key = ZLRS_CONFIG_REGION;
		break;
	case ZLRS_CRSF_PARAM_MODEL:
		key = ZLRS_CONFIG_MODEL;
		break;
	case ZLRS_CRSF_PARAM_MODEL_MATCH:
		key = ZLRS_CONFIG_MODEL_MATCH;
		break;
	case ZLRS_CRSF_PARAM_TELEMETRY:
		key = ZLRS_CONFIG_TELEMETRY;
		break;
	case ZLRS_CRSF_PARAM_MODE:
		key = ZLRS_CONFIG_MODE;
		break;
	default:
		return false;
	}
	return zlrs_config_set(key, value, true) == 0;
}

static bool crsf_command(void *ctx, uint8_t group, uint8_t command, const uint8_t *data,
			 uint8_t len)
{
	(void)ctx;
	if (group != 0x10U) {
		return false;
	}
	switch (command) {
	case 1U:
		return zlrs_rf_pair_start() == 0;
	case 2U:
		return zlrs_rf_pair_cancel() == 0;
	case 5U:
		if (len != 1U) {
			return false;
		}
		/* The handset owns model selection; do not write flash on every startup. */
		return zlrs_config_set(ZLRS_CONFIG_MODEL, data[0], false) == 0;
	case 0x0AU:
	case 0x0BU:
		return zlrs_config_set(ZLRS_CONFIG_TELEMETRY, command == 0x0AU, false) == 0;
	case 0x80U:
		zlrs_runtime_request_reboot(2000U);
		return true;
	case 0x81U:
		zlrs_runtime_cancel_reboot();
		return true;
	default:
		return false;
	}
}

static bool crsf_speed(void *ctx, uint8_t port, uint32_t baud)
{
	(void)ctx;
	return !service.air_reply && zlrs_link_crsf_speed(port, baud);
}

static void crsf_device_refresh(void)
{
	const zlrs_config_state_t config = zlrs_config_state();
	const zlrs_rf_status_t rf = zlrs_rf_status();

	service.device.serial = (uint32_t)rf.dev_id[0] << 24 | (uint32_t)rf.dev_id[1] << 16 |
				(uint32_t)rf.dev_id[2] << 8 | rf.dev_id[3];
	service.device.values[ZLRS_CRSF_PARAM_RATE] = config.saved.control_rate != 0U
							      ? config.saved.control_rate
							      : service.cfg.control_rate_hz;
	service.device.values[ZLRS_CRSF_PARAM_POWER] = config.saved.power_dbm;
	service.device.min_power_dbm = ZLRS_CONFIG_POWER_MIN_DBM;
	service.device.max_power_dbm = MIN(ZLRS_CONFIG_POWER_MAX_DBM,
					   zlrs_region_get(config.saved.region)->max_tx_power_dbm);
	service.device.values[ZLRS_CRSF_PARAM_REGION] = config.saved.region;
	service.device.values[ZLRS_CRSF_PARAM_MODEL] = config.active.model;
	service.device.values[ZLRS_CRSF_PARAM_MODEL_MATCH] = config.active.model_match;
	service.device.values[ZLRS_CRSF_PARAM_TELEMETRY] = config.active.telemetry;
	service.device.values[ZLRS_CRSF_PARAM_MODE] = config.saved.mode;
	service.device.bind_status = rf.state == ZLRS_STATE_PAIRING ? 2U : 0U;
}

static void crsf_device_setup(void)
{
	const zlrs_crsf_device_ops_t ops = {
		.send = crsf_emit,
		.write = crsf_write_parameter,
		.command = crsf_command,
		.speed = crsf_speed,
	};

	zlrs_crsf_device_init(&service.device,
			      is_tx() ? ZLRS_CRSF_ADDR_TX_MODULE : ZLRS_CRSF_ADDR_RX, &ops);
	service.device.hardware = ZLRS_RF_BAND;
	service.device.version = APP_VERSION_STRING " | RC,TLM,parameters,model,MSP,CRSF Shot,baud";
	service.device.firmware = APP_VERSION_NUMBER;
	crsf_device_refresh();
}

static bool crsf_dispatch_frame(const uint8_t *frame, uint8_t len, bool from_air)
{
	if (service.reply_len != 0U) {
		return false;
	}
	if (!zlrs_crsf_frame_valid(frame, len)) {
		return true; /* Drop malformed CRSF without repeatedly retrying an invalid command. */
	}
	const uint8_t type = frame[2];
	const uint8_t *payload = frame + 3U;
	const uint8_t size = len - 4U;

	if (type == ZLRS_CRSF_TYPE_COMMAND && !zlrs_crsf_command_valid(payload, size)) {
		return true;
	}
	crsf_device_refresh();
	service.air_reply = from_air;
	const enum zlrs_crsf_dispatch result =
		zlrs_crsf_device_handle(&service.device, type, payload, size);
	service.air_reply = false;
	if (result == ZLRS_CRSF_RETRY) {
		return false;
	}
	if (result == ZLRS_CRSF_INVALID) {
		return true;
	}
	const bool broadcast_ping = type == ZLRS_CRSF_TYPE_DEVICE_PING && payload[0] == 0U;

	if (result == ZLRS_CRSF_HANDLED && !broadcast_ping) {
		return true;
	}
	if (from_air) {
		/* Local responses never loop back onto the radio. Preserve the extended header. */
		if (is_tx() && type < 0x28U && !zlrs_config_state().active.telemetry) {
			return true;
		}
		return zlrs_link_crsf_write_frame(frame, len);
	}
	if (!is_tx() && type < 0x28U && !zlrs_config_state().active.telemetry) {
		return true;
	}
	return zlrs_rf_send_service(frame, len);
}

static bool crsf_handle_input(const zlrs_link_crsf_input_t *input, uint32_t now_ms)
{
	const uint8_t type = input->frame[2];
	const uint8_t len = input->len - 4U;

	if (type != ZLRS_CRSF_TYPE_RC_CHANNELS) {
		return crsf_dispatch_frame(input->frame, input->len, false);
	}
	if (!is_tx() || !service.cfg.control_enabled || len < ZLRS_CRSF_RC_PAYLOAD_LEN) {
		service.bad_inputs++;
		return true;
	}
	if ((uint32_t)(now_ms - input->received_ms) > CONFIG_ZLRS_APP_CRSF_TIMEOUT_MS) {
		return true;
	}
	const uint8_t known = MIN(len, ZLRS_RC_PAYLOAD_MAX_LEN);

	zlrs_rf_control_input(input->frame + 3U, known);
	service.rc_ms = input->received_ms;
	service.rc_us = input->received_us;
	service.rc_seen = true;
	return true;
}

/*
 * TX: link statistics for the handset (0x14).
 * Uplink numbers come from RX telemetry, downlink ones are measured here — each side reports
 */
static void crsf_send_link_stats(const zlrs_tlm_t *tlm)
{
	zlrs_crsf_link_stats_t stats;
	uint8_t payload[ZLRS_CRSF_LINK_STATS_LEN];

	if (!service.cfg.enabled) {
		return;
	}

	const zlrs_rf_status_t rf = zlrs_rf_status();

	memset(&stats, 0, sizeof(stats));
	stats.uplink_rssi_ant1 = tlm->rssi_neg_dbm;
	stats.uplink_rssi_ant2 = tlm->rssi_ant2_neg_dbm;
	stats.uplink_lq = tlm->lq_percent;
	stats.uplink_snr_db = tlm->snr_db;
	stats.active_antenna = tlm->active_antenna;
	stats.rf_mode = zlrs_crsf_rf_mode(service.cfg.control_rate_hz);
	stats.uplink_tx_power = zlrs_crsf_tx_power_index(rf.tx_power_dbm);
	stats.downlink_rssi = zlrs_crsf_rssi_from_dbm(rf.rssi_dbm);
	stats.downlink_lq = rf.control_state == ZLRS_STATE_CONNECTED && service.peer_tlm_seen &&
					    (uint32_t)(zlrs_slot_timer_now_ms() -
						       service.peer_tlm_ms) <= rf.peer_timeout_ms
				    ? rf.control_lq
				    : 0U;
	stats.downlink_snr_db = rf.snr_db;

	zlrs_crsf_link_stats_encode(payload, &stats);
	(void)zlrs_link_crsf_write_latest(ZLRS_CRSF_TYPE_LINK_STATISTICS, payload, sizeof(payload));
	/* Detailed standard report preserves the RX's own power and actual frame rate. */
	uint8_t tx_payload[ZLRS_TLM_LEN];
	zlrs_tlm_encode(tx_payload, tlm);
	if (rf.peer_power_dbm >= 0) {
		(void)zlrs_link_crsf_write_latest(ZLRS_CRSF_TYPE_LINK_STATISTICS_TX, tx_payload,
						  sizeof(tx_payload));
	}
	if (rf.tx_power_dbm >= 0) {
		const uint8_t downlink[7] = {
			stats.downlink_rssi,
			zlrs_crsf_rssi_percent(rf.rssi_dbm, -100),
			stats.downlink_lq,
			(uint8_t)stats.downlink_snr_db,
			(uint8_t)rf.tx_power_dbm,
			0U,
			0U,
		};

		(void)zlrs_link_crsf_write_latest(ZLRS_CRSF_TYPE_LINK_STATISTICS_RX, downlink,
						  sizeof(downlink));
	}
}

/* RX: rebuild the received RC payload into a CRSF frame for the flight controller */
void zlrs_link_crsf_service_channels(const uint8_t *payload, uint8_t len)
{
	if (!service.cfg.enabled ||
	    (len != ZLRS_CRSF_RC_PAYLOAD_LEN && len != ZLRS_RC_PAYLOAD_MAX_LEN)) {
		return;
	}

	(void)zlrs_link_crsf_write_latest(ZLRS_CRSF_TYPE_RC_CHANNELS, payload, len);
}

/* RX: report the uplink quality measured on this side back to the TX */
static void crsf_report_tlm(uint32_t now_ms)
{
	const zlrs_rf_status_t rf = zlrs_rf_status();

	zlrs_tlm_t tlm;
	uint8_t payload[ZLRS_TLM_LEN];

	if ((uint32_t)(now_ms - service.tlm_ms) < CONFIG_ZLRS_APP_TLM_PERIOD_MS) {
		return;
	}
	service.tlm_ms = now_ms;

	memset(&tlm, 0, sizeof(tlm));
	tlm.lq_percent = rf.control_state == ZLRS_STATE_CONNECTED ? rf.control_lq : 0U;
	tlm.rssi_neg_dbm = zlrs_crsf_rssi_from_dbm(rf.rssi_dbm);
	tlm.snr_db = rf.snr_db;
	/* Relative RSSI uses a modem reference, independently of packet success rate. */
	tlm.rssi_percent = zlrs_crsf_rssi_percent(rf.rssi_dbm, service.cfg.rssi_reference_dbm);
	tlm.rf_power_dbm = rf.tx_power_dbm > 0 ? rf.tx_power_dbm : 0U;
	tlm.fps =
		service.cfg.control_rate_hz / 10U > 255U ? 255U : service.cfg.control_rate_hz / 10U;

	zlrs_tlm_encode(payload, &tlm);
	zlrs_rf_telemetry(payload, sizeof(payload));
	zlrs_crsf_link_stats_t stats = {
		.uplink_rssi_ant1 = tlm.rssi_neg_dbm,
		.uplink_lq = tlm.lq_percent,
		.uplink_snr_db = tlm.snr_db,
		.rf_mode = zlrs_crsf_rf_mode(service.cfg.control_rate_hz),
		.uplink_tx_power = zlrs_crsf_tx_power_index(rf.peer_power_dbm),
	};
	uint8_t link_payload[ZLRS_CRSF_LINK_STATS_LEN];

	zlrs_crsf_link_stats_encode(link_payload, &stats);
	(void)zlrs_link_crsf_write_latest(ZLRS_CRSF_TYPE_LINK_STATISTICS, link_payload,
					  sizeof(link_payload));
	/* The standard unsigned dBm field cannot describe negative bench powers. */
	if (rf.tx_power_dbm >= 0) {
		(void)zlrs_link_crsf_write_latest(ZLRS_CRSF_TYPE_LINK_STATISTICS_TX, payload,
						  sizeof(payload));
	}
}

/* CRSF task in the main loop: read frames, cut stale RC input, and report telemetry */
void zlrs_link_crsf_service_poll(uint32_t now_ms)
{
	zlrs_link_crsf_input_t input;

	if (!service.cfg.enabled || !zlrs_link_crsf_ready()) {
		return;
	}
	if (service.reply_len != 0U &&
	    (service.reply_air ? zlrs_rf_send_service(service.reply, service.reply_len)
			       : zlrs_link_crsf_write_frame(service.reply, service.reply_len))) {
		service.reply_len = 0U;
	}
	/* Admit a retained baud reply before serial poll can commit the pending UART switch. */
	zlrs_link_crsf_poll();
	/* Channel freshness does not wait for configuration/telemetry backpressure. */
	if (zlrs_link_crsf_read_rc(&input)) {
		(void)crsf_handle_input(&input, zlrs_slot_timer_now_ms());
	}
	while (service.reply_len == 0U && service.air_count != 0U) {
		if (!crsf_dispatch_frame(service.air_inputs[service.air_head].frame,
					 service.air_inputs[service.air_head].len, true)) {
			break;
		}
		service.air_head =
			(uint8_t)((service.air_head + 1U) % ARRAY_SIZE(service.air_inputs));
		service.air_count--;
	}
	while (service.reply_len == 0U &&
	       (service.have_pending || zlrs_link_crsf_read(&service.pending))) {
		/* Re-read after a settings write; flash can advance the clock substantially. */
		now_ms = zlrs_slot_timer_now_ms();
		service.have_pending = !crsf_handle_input(&service.pending, now_ms);
		if (service.have_pending) {
			break;
		}
	}
	now_ms = zlrs_slot_timer_now_ms();
	if (is_tx()) {
		expire_input(now_ms);
		if (service.cfg.control_enabled && (uint32_t)(now_ms - service.timing_ms) >= 100U) {
			zlrs_tlm_t report = service.peer_tlm;
			const zlrs_rf_status_t rf = zlrs_rf_status();

			if (!service.peer_tlm_seen || rf.control_state != ZLRS_STATE_CONNECTED ||
			    (uint32_t)(now_ms - service.peer_tlm_ms) > rf.peer_timeout_ms) {
				report.lq_percent = 0U;
			}
			crsf_send_link_stats(&report);
			(void)zlrs_crsf_device_timing(&service.device, service.cfg.frame_period_us,
						      service.rc_seen ? service.timing_offset_us
								      : 0);
			service.timing_ms = now_ms;
		}
	} else if (service.cfg.control_enabled) {
		crsf_report_tlm(now_ms);
	}
	const struct zlrs_crsf_status status = {
		.ready = true,
		.input_valid = service.rc_seen,
		.queued = service.air_count + service.have_pending + (service.reply_len != 0U),
		.bad_inputs = service.bad_inputs,
		.dropped = zlrs_link_crsf_rx_dropped(),
	};
	static struct zlrs_crsf_status last;
	static bool published;
	if ((!published || memcmp(&last, &status, sizeof(last)) != 0) &&
	    zbus_chan_pub(&zlrs_crsf_chan, &status, K_NO_WAIT) == 0) {
		last = status;
		published = true;
	}
}

void zlrs_link_crsf_service_init(const zlrs_link_crsf_service_cfg_t *cfg)
{
	service.cfg = *cfg;
	if (service.cfg.enabled) {
		(void)zlrs_link_crsf_init(is_tx());
		crsf_device_setup();
	}
}

void zlrs_link_crsf_service_telemetry(const zlrs_tlm_t *tlm)
{
	service.peer_tlm = *tlm;
	service.peer_tlm_ms = zlrs_slot_timer_now_ms();
	service.peer_tlm_seen = true;
}

static void expire_input(uint32_t now_ms)
{
	if (is_tx() && service.rc_seen &&
	    (uint32_t)(now_ms - service.rc_ms) > CONFIG_ZLRS_APP_CRSF_TIMEOUT_MS) {
		service.rc_seen = false;
		zlrs_rf_control_input(NULL, 0U);
	}
}

void zlrs_link_crsf_service_prepare(uint32_t frame_us, uint32_t prepare_us, uint32_t period_us,
				    bool first_slot)
{
	if (first_slot && service.cfg.enabled) {
		zlrs_link_crsf_input_t latest;

		if (zlrs_link_crsf_read_rc(&latest)) {
			(void)crsf_handle_input(&latest, zlrs_slot_timer_now_ms());
		}
		/* Measure the actual handset reception against the RC preparation deadline. */
		if (is_tx() && service.rc_seen) {
			int32_t offset = (int32_t)(frame_us - prepare_us - service.rc_us);

			offset %= (int32_t)period_us;
			if (offset > (int32_t)period_us / 2) {
				offset -= (int32_t)period_us;
			} else if (offset < -(int32_t)period_us / 2) {
				offset += (int32_t)period_us;
			}
			service.timing_offset_us = offset;
		}
	}
	expire_input(zlrs_slot_timer_now_ms());
}

void zlrs_link_crsf_service_reset(void)
{
	service.rc_seen = false;
	zlrs_link_crsf_flush();
	zlrs_rf_control_input(NULL, 0U);
}

bool zlrs_link_crsf_service_input_seen(void)
{
	return service.rc_seen;
}

uint32_t zlrs_link_crsf_service_bad_inputs(void)
{
	return service.bad_inputs;
}
