/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/zbus/zbus.h>
#include "zlrs/proto/limits.h"
#include "zlrs/proto/tlm.h"

/* Boot capabilities copied by value; the protocol frontend never borrows link storage. */
typedef struct {
	uint32_t control_rate_hz;
	uint32_t frame_period_us;
	int16_t rssi_reference_dbm;
	bool enabled;
	bool control_enabled;
	bool role_tx;
} zlrs_link_crsf_service_cfg_t;

struct zlrs_crsf_status {
	bool ready;
	bool input_valid;
	uint8_t queued;
	uint32_t bad_inputs;
	uint32_t dropped;
};
ZBUS_CHAN_DECLARE(zlrs_crsf_chan);

void zlrs_link_crsf_service_init(const zlrs_link_crsf_service_cfg_t *cfg);
/* Radio callbacks admit complete frames; device actions wait until poll after RF slots. */
bool zlrs_link_crsf_service_receive(void *ctx, const uint8_t *frame, uint8_t len);
void zlrs_link_crsf_service_channels(const uint8_t *payload, uint8_t len);
void zlrs_link_crsf_service_telemetry(const zlrs_tlm_t *tlm);
/* Every slot checks RC freshness; the first also samples input and computes Shot timing. */
void zlrs_link_crsf_service_prepare(uint32_t frame_us, uint32_t prepare_us, uint32_t period_us,
				    bool first_slot);
void zlrs_link_crsf_service_poll(uint32_t now_ms);
/* Management and flash operations invalidate old control input before RF resumes. */
void zlrs_link_crsf_service_reset(void);
bool zlrs_link_crsf_service_input_seen(void);
uint32_t zlrs_link_crsf_service_bad_inputs(void);
