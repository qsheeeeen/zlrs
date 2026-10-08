/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/crsf/crsf.h"

typedef struct {
	uint8_t frame[ZLRS_CRSF_FRAME_MAX_LEN];
	uint8_t len;
	uint32_t received_ms;
	uint32_t received_us;
} zlrs_link_crsf_input_t;

int zlrs_link_crsf_init(bool handset);
bool zlrs_link_crsf_ready(void);
uint8_t zlrs_link_crsf_write(uint8_t type, const uint8_t *payload, uint8_t len);
/* Periodic measurements/channels retain their latest unsent value instead of filling the queue. */
uint8_t zlrs_link_crsf_write_latest(uint8_t type, const uint8_t *payload, uint8_t len);
bool zlrs_link_crsf_write_frame(const uint8_t *frame, uint8_t len);
bool zlrs_link_crsf_read(zlrs_link_crsf_input_t *input);
bool zlrs_link_crsf_read_rc(zlrs_link_crsf_input_t *input);
void zlrs_link_crsf_flush(void);
void zlrs_link_crsf_poll(void);
/* The proposal reply is sent at the old baud before switching an idle UART. */
bool zlrs_link_crsf_speed(uint8_t port, uint32_t baud);
uint32_t zlrs_link_crsf_rx_dropped(void);
