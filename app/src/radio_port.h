/*
 * Radio port layer: the concrete backends behind zlrs_radio_ops_t (contract: 07-architecture 6.2).
 * CONFIG_ZLRS_APP_RADIO picks one: radio_rac.c (RAC/LR2021) or radio_null.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "zlrs/link/link.h"

/* Operation tables handed to zlrs_link_init() */
const zlrs_radio_ops_t *zlrs_radio_control_ops(void);
const zlrs_radio_ops_t *zlrs_radio_data_ops(void);

/* Bring the backend up and wire completion into both links (negative on failure) */
int zlrs_radio_port_init(zlrs_link_t *control, zlrs_link_t *data);

/*
 * Wait for one transaction; negative on timeout.
 * Microseconds matter: at 500 Hz control a slot only budgets a few hundred microseconds.
 * With the radio off there is nothing to wait for, so it returns 0 at once.
 */
int zlrs_radio_port_wait_us(uint32_t timeout_us);

/* Hand finished transactions back to the link layer (called on the link thread) */
void zlrs_radio_port_drain(void);

/* Abort a pending transaction after a slot overrun, so a late completion cannot leak into the next */
void zlrs_radio_port_abort(void);

/* Same time base as the radio backend (k_uptime_get_32) */
uint32_t zlrs_radio_port_now_ms(void);
