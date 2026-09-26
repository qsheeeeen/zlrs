/*
 * What this hardware is, derived from devicetree (doc/08-firmware.md 4).
 *
 * This header is the only place in the application that reads aliases and the
 * radio-design node, and the only place that may name them. Everything else
 * asks these macros, so a new board is a devicetree change, never a C change.
 *
 * A feature the hardware cannot carry fails the build here instead of
 * misbehaving in the field.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include "zlrs/region/region.h"

/* ---- ports: one UART plus one CDC per logical port, both optional ---- */
#define ZLRS_PORT_CTRL_FIXED                                                                      \
	(DT_NODE_EXISTS(DT_ALIAS(zlrs_control_uart)) ||                                           \
	 DT_NODE_EXISTS(DT_ALIAS(zlrs_control_cdc)))
#define ZLRS_PORT_DATA_FIXED                                                                      \
	(DT_NODE_EXISTS(DT_ALIAS(zlrs_data_uart)) || DT_NODE_EXISTS(DT_ALIAS(zlrs_data_cdc)))
#define ZLRS_PORT_CONFIG DT_NODE_EXISTS(DT_ALIAS(zlrs_config_cdc))

/* ---- peripherals ---- */
#define ZLRS_HAS_BUTTON DT_NODE_EXISTS(DT_ALIAS(zlrs_button))
#define ZLRS_HAS_LED DT_NODE_EXISTS(DT_ALIAS(zlrs_led))
#define ZLRS_HAS_WDT DT_NODE_EXISTS(DT_ALIAS(watchdog0))
#define ZLRS_HAS_NVS IS_ENABLED(CONFIG_NVS)
#define ZLRS_HAS_USB IS_ENABLED(CONFIG_ZLRS_USB)

/* ---- radio design: band and the frequency coverage the PA network is built for ---- */
#define ZLRS_RF_BAND DT_PROP(DT_NODELABEL(zlrs_rf), band)
#define ZLRS_RF_LOW_HZ DT_PROP(DT_NODELABEL(zlrs_rf), low_hz)
#define ZLRS_RF_HIGH_HZ DT_PROP(DT_NODELABEL(zlrs_rf), high_hz)
/* Region a fresh board boots with; the config protocol overwrites it in NVS */
#define ZLRS_DEFAULT_REGION DT_PROP(DT_NODELABEL(zlrs_rf), default_region)
#define ZLRS_RF_DESIGN                                                                             \
	(&(const zlrs_rf_design_t){                                                                \
		.band = ZLRS_RF_BAND, .low_hz = ZLRS_RF_LOW_HZ, .high_hz = ZLRS_RF_HIGH_HZ})

/* ---- capabilities the software insists on ---- */
#if !ZLRS_HAS_NVS
#error "zlrs keeps keys, pairing and failsafe values in NVS; a board without NVS is not supported"
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_LINK_CONTROL) && !ZLRS_PORT_CTRL_FIXED
#error "control link enabled but this board has no control port (zlrs-control-uart/-cdc)"
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_LINK_DATA) && !ZLRS_PORT_DATA_FIXED
#error "data link enabled but this board has no data port (zlrs-data-uart/-cdc)"
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_WATCHDOG) && !ZLRS_HAS_WDT
#error "watchdog enabled but this board has no watchdog device"
#endif
