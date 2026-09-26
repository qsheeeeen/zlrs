/*
 * Hardware watchdog: a crash or a hang resets the chip, which MCUboot's test/revert reacts to.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* Feed the watchdog from the main loop and the config-mode poll loop; empty when disabled */
void zlrs_watchdog_feed(void);

/* Disarm the watchdog. Required before rebooting into MCUboot serial recovery: on RP2350 it
 * can still run after SYSRESETREQ and would cut that recovery short. Empty when disabled. */
void zlrs_watchdog_disarm(void);

/* Re-arm after a long blocking operation such as a config session. Empty when disabled */
void zlrs_watchdog_arm(void);
