/*
 * Control link I/O: the CRSF side of it (handset on the TX, flight controller on the RX).
 *
 * The link itself lives in core/link and the CRSF codec in core/crsf; this is the host/peer side
 * of the control link — endpoint, RX frames, TX ring (doc/07-architecture.md 5). Which device that
 * the implementation binds directly to devicetree UART/CDC devices.
 *
 * Naming: `<dir>/<file>.c` carries `zlrs_<dir>_<file>_*()`, so this is `zlrs_link_crsf_*()`.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

int zlrs_link_crsf_init(void);

/* Is the endpoint usable (false when the alias is missing or the device is not ready) */
bool zlrs_link_crsf_ready(void);

/*
 * Build a frame and queue it: the type picks the address (RC 0xC8 to the FC, stats 0xEA to
 * the handset). Returns the whole frame length, or 0 on failure.
 */
uint8_t zlrs_link_crsf_write(uint8_t type, const uint8_t *payload, uint8_t payload_len);

/* Read one frame from the RX buffer (called from the main loop); 0 when there is none */
uint8_t zlrs_link_crsf_read(uint8_t *type, uint8_t *payload, uint8_t max_len);

/* Statistics: frames dropped because the RX buffer was full */
uint32_t zlrs_link_crsf_rx_dropped(void);
