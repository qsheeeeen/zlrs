/*
 * Rate adaptation of the control link: drop a tier when the LQ falls, climb back when it recovers.
 *
 * The policy is core/link's (`zlrs_rate_update`, host-tested); this is its application — the tier
 * table this build carries, the link it switches, and the once-a-second call. Thresholds still
 * wait for measured packet-error curves, see doc/13-roadmap.md.
 *
 * Naming: `<dir>/<file>.c` carries `zlrs_<dir>_<file>_*()`, so this is `zlrs_link_rate_*()`.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "zlrs/link/link.h"

/* Arm it from the tier the control link starts on (`mod->rate_bps` picks the row) */
void zlrs_link_rate_init(const zlrs_mod_cfg_t *mod);

/* Once-a-second verdict: switch the control link's tier when the LQ says so */
void zlrs_link_rate_tick(zlrs_link_t *control, uint32_t now_ms);
