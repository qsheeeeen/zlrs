/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "zlrs/crsf/crsf.h"

enum zlrs_crsf_parameter {
	ZLRS_CRSF_PARAM_ROOT,
	ZLRS_CRSF_PARAM_RATE,
	ZLRS_CRSF_PARAM_POWER,
	ZLRS_CRSF_PARAM_REGION,
	ZLRS_CRSF_PARAM_BIND,
	ZLRS_CRSF_PARAM_MODEL,
	ZLRS_CRSF_PARAM_MODEL_MATCH,
	ZLRS_CRSF_PARAM_TELEMETRY,
	ZLRS_CRSF_PARAM_MODE,
	ZLRS_CRSF_PARAM_VERSION,
	ZLRS_CRSF_PARAM_REBOOT,
	ZLRS_CRSF_PARAM_COUNT
};

enum zlrs_crsf_dispatch {
	ZLRS_CRSF_ROUTE,
	ZLRS_CRSF_HANDLED,
	ZLRS_CRSF_RETRY,
	ZLRS_CRSF_INVALID
};

/* The protocol owns encoding and validation; application callbacks perform device actions.
 * A successful write callback must persist the value before the protocol confirms it.
 */
typedef struct {
	bool (*send)(void *ctx, uint8_t type, const uint8_t *payload, uint8_t len);
	bool (*write)(void *ctx, uint8_t parameter, int32_t value);
	bool (*command)(void *ctx, uint8_t group, uint8_t command, const uint8_t *data,
			uint8_t len);
	bool (*speed)(void *ctx, uint8_t port, uint32_t baud);
	void *ctx;
} zlrs_crsf_device_ops_t;

typedef struct {
	zlrs_crsf_device_ops_t ops;
	const char *name;
	const char *version;
	uint32_t serial;
	uint32_t hardware;
	uint32_t firmware;
	int32_t values[ZLRS_CRSF_PARAM_COUNT];
	uint8_t address;
	uint8_t bind_status;
	uint8_t reboot_status;
	int8_t max_power_dbm;
} zlrs_crsf_device_t;

void zlrs_crsf_device_init(zlrs_crsf_device_t *device, uint8_t address,
			   const zlrs_crsf_device_ops_t *ops);
enum zlrs_crsf_dispatch zlrs_crsf_device_handle(zlrs_crsf_device_t *device, uint8_t type,
						const uint8_t *payload, uint8_t len);
bool zlrs_crsf_device_info(zlrs_crsf_device_t *device, uint8_t destination);
bool zlrs_crsf_device_timing(zlrs_crsf_device_t *device, uint32_t interval_us, int32_t offset_us);
