/*
 * Port layer: CRSF port, data port (passthrough) and config mode, including endpoint choice.
 *
 * Endpoints come from board aliases; selection and trimming: doc/08-firmware.md 1, 4. The
 * console does not go through here (it is the board's chosen `zephyr,console`).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/link/link.h"

/* ---- data port: passthrough byte stream (normal mode) -------------------- */

/* For bench debugging: state of one data endpoint (`zlrs ports` prints it) */
typedef struct {
	const char *name;
	bool ready;          /* the endpoint exists on the board and the device is ready */
	bool active;         /* this is the selected endpoint (see port_active) */
	uint32_t rx_total;   /* bytes received (evidence that the host is using it) */
	uint32_t rx_dropped; /* bytes dropped because this was not the active endpoint */
} zlrs_port_state_t;

/*
 * Read the state of both data port endpoints, returns how many were filled in.
 * The choice itself (DTR, host bytes seen) lives in port_active(); this only reports it.
 */
size_t zlrs_data_port_state(zlrs_port_state_t *out, size_t count);

/* baud = 0 keeps the board default (115200); non-zero overrides it (set by the config tool) */
int zlrs_data_port_init(uint32_t baud);

/* Pull data out of the RX ring buffer, returns the bytes actually read */
uint16_t zlrs_data_port_read(uint8_t *buf, uint16_t len);

/* Queue into the TX ring buffer and start the IRQ transmit, returns the bytes accepted */
uint16_t zlrs_data_port_write(const uint8_t *buf, uint16_t len);

/* Bytes never delivered: dropped when full plus those received on the inactive endpoint */
uint32_t zlrs_data_port_dropped(void);

/* Used during config mode: drop bytes from both endpoints (counted, never delivered) */
void zlrs_data_port_discard_rx(void);

/* Application-side actions the config protocol triggers; unregistered ones answer E_STATE */
typedef struct {
	int (*save_region)(uint8_t region);
	int (*save_power)(int8_t dbm);
	int (*save_role)(uint8_t role);
	int (*save_profile)(uint8_t profile);
	int (*new_dev_id)(void);
	int (*erase_pair)(void);
	int (*factory_reset)(void);
	int (*enter_recovery)(void);
	uint8_t (*region)(void);
	int8_t (*power)(void);
} zlrs_config_ops_t;

void zlrs_config_set_ops(const zlrs_config_ops_t *ops);

/*
 * The host sent its confirm command (see the upgrade flow in doc/08-firmware.md 6).
 * It is the only verdict on whether this image stays: without a confirm after an upgrade,
 * the next reset lets MCUboot roll back.
 */
bool zlrs_config_image_confirmed(void);

/* ---- CRSF port: handset <-> module (TX), receiver -> flight controller (RX) - */

int zlrs_crsf_port_init(void);

/* Is the port usable (false when the alias is missing or the device is not ready) */
bool zlrs_crsf_port_ready(void);

/*
 * Build a frame and queue it: the type picks the address (RC 0xC8 to the FC, stats 0xEA to
 * the handset). Returns the whole frame length, or 0 on failure.
 */
uint8_t zlrs_crsf_port_write(uint8_t type, const uint8_t *payload, uint8_t payload_len);

/* Read one frame from the RX buffer (called from the main loop); 0 when there is none */
uint8_t zlrs_crsf_port_read(uint8_t *type, uint8_t *payload, uint8_t max_len);

/* Port statistics: frames dropped because the RX buffer was full */
uint32_t zlrs_crsf_port_rx_dropped(void);

/* ---- config port: a dedicated CDC (doc/08-firmware.md 1) ------------------ */

int zlrs_config_port_init(void);
bool zlrs_config_port_ready(void);

/*
 * Poll the config port from the main loop without blocking:
 * an enter-config-mode command returns true (the ACK is out), anything else answers E_STATE.
 */
bool zlrs_config_port_poll_enter(void);

/* ---- config mode: entered by a command on the config CDC (08-firmware 2) -- */

/*
 * Run a blocking config session until the timeout or an EXIT command.
 * Commands act on the control link; failsafe values land in the application's own buffer.
 * Returns 0 = EXIT seen, 1 = timeout, -1 = the board has no data port alias.
 */
int zlrs_config_mode_run(zlrs_link_t *control, uint8_t *failsafe);
