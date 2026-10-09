/* SPDX-License-Identifier: Apache-2.0 */
#include "zlrs/crsf/device.h"

#include <limits.h>
#include <string.h>

#define PARAM_CHUNK_LEN  56U
#define PARAM_BUFFER_LEN 192U

static void put32(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)(value >> 24);
	out[1] = (uint8_t)(value >> 16);
	out[2] = (uint8_t)(value >> 8);
	out[3] = (uint8_t)value;
}

static uint32_t get32(const uint8_t *data)
{
	return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
}

/* All strings here are device-owned, bounded literals; cap nevertheless limits metadata. */
static uint16_t string(uint8_t *out, uint16_t cap, const char *text)
{
	uint16_t len = 0U;

	while (len + 1U < cap && text[len] != '\0') {
		out[len] = (uint8_t)text[len];
		len++;
	}
	out[len++] = 0U;
	return len;
}

static bool emit(zlrs_crsf_device_t *d, uint8_t type, const uint8_t *payload, uint8_t len)
{
	return d->ops.send != NULL && d->ops.send(d->ops.ctx, type, payload, len);
}

void zlrs_crsf_device_init(zlrs_crsf_device_t *d, uint8_t address,
			   const zlrs_crsf_device_ops_t *ops)
{
	memset(d, 0, sizeof(*d));
	d->address = address;
	d->name = address == ZLRS_CRSF_ADDR_RX ? "ZLRS RX" : "ZLRS TX";
	d->version = "ZLRS";
	d->firmware = 0x00010000U;
	d->values[ZLRS_CRSF_PARAM_RATE] = 150;
	d->values[ZLRS_CRSF_PARAM_POWER] = 10;
	d->values[ZLRS_CRSF_PARAM_TELEMETRY] = 1;
	d->max_power_dbm = 33;
	d->min_power_dbm = -17;
	d->ops = *ops;
}

bool zlrs_crsf_device_info(zlrs_crsf_device_t *d, uint8_t destination)
{
	uint8_t payload[ZLRS_CRSF_PAYLOAD_MAX_LEN];
	uint16_t len = 2U;

	payload[0] = destination;
	payload[1] = d->address;
	len += string(payload + len, 44U, d->name);
	put32(payload + len, d->serial);
	put32(payload + len + 4U, d->hardware);
	put32(payload + len + 8U, d->firmware);
	len += 12U;
	payload[len++] = ZLRS_CRSF_PARAM_COUNT - 1U;
	payload[len++] = 0U;
	return emit(d, ZLRS_CRSF_TYPE_DEVICE_INFO, payload, (uint8_t)len);
}

static uint16_t parameter(zlrs_crsf_device_t *d, uint8_t id, uint8_t *out)
{
	static const char *const names[ZLRS_CRSF_PARAM_COUNT] = {
		"ROOT",           "Rate target (reboot)",
		"Power (reboot)", "Region (reboot)",
		"Bind",           "Model ID",
		"Model match",    "Telemetry",
		"Mode (reboot)",  "Firmware",
		"Apply / reboot"};
	uint16_t len = 0U;
	uint8_t type;

	out[len++] = 0U; /* parent root */
	if (id >= ZLRS_CRSF_PARAM_COUNT) {
		out[len++] = 0x7FU;
		return len;
	}
	if (id == ZLRS_CRSF_PARAM_ROOT) {
		type = 11U;
	} else if (id == ZLRS_CRSF_PARAM_BIND || id == ZLRS_CRSF_PARAM_REBOOT) {
		type = 13U;
	} else if (id == ZLRS_CRSF_PARAM_VERSION) {
		type = 12U;
	} else if (id == ZLRS_CRSF_PARAM_REGION || id == ZLRS_CRSF_PARAM_MODEL_MATCH ||
		   id == ZLRS_CRSF_PARAM_TELEMETRY || id == ZLRS_CRSF_PARAM_MODE) {
		type = 9U;
	} else {
		type = 8U; /* standard FLOAT: int32 with zero decimal places */
	}
	out[len++] = type;
	len += string(out + len, 40U, names[id]);
	if (type == 11U) {
		for (uint8_t child = 1U; child < ZLRS_CRSF_PARAM_COUNT; child++) {
			out[len++] = child;
		}
		out[len++] = 0xFFU;
	} else if (type == 12U) {
		len += string(out + len, 100U, d->version);
	} else if (type == 13U) {
		const bool bind = id == ZLRS_CRSF_PARAM_BIND;

		out[len++] = bind ? d->bind_status : d->reboot_status;
		out[len++] = 10U; /* poll timeout, 100 ms units */
		len += string(out + len, 40U, bind ? "Pairing" : "Restart to apply settings?");
	} else if (type == 9U) {
		const char *options =
			id == ZLRS_CRSF_PARAM_REGION
				? "FCC915;EU868;CN470;CN433"
				: (id == ZLRS_CRSF_PARAM_MODE ? "Mixed;DATA24" : "Off;On");

		len += string(out + len, 80U, options);
		out[len++] = (uint8_t)d->values[id];
		out[len++] = 0U;
		out[len++] = id == ZLRS_CRSF_PARAM_REGION ? 3U : 1U;
		out[len++] = id == ZLRS_CRSF_PARAM_TELEMETRY ? 1U : 0U;
		out[len++] = 0U; /* empty unit */
	} else {
		const int32_t minimum = id == ZLRS_CRSF_PARAM_POWER
						? d->min_power_dbm
						: (id == ZLRS_CRSF_PARAM_RATE ? 4 : 0);
		const int32_t maximum = id == ZLRS_CRSF_PARAM_POWER
						? d->max_power_dbm
						: (id == ZLRS_CRSF_PARAM_RATE ? 1000 : 255);
		const int32_t initial =
			id == ZLRS_CRSF_PARAM_POWER ? 10 : (id == ZLRS_CRSF_PARAM_RATE ? 150 : 0);

		put32(out + len, (uint32_t)d->values[id]);
		put32(out + len + 4U, (uint32_t)minimum);
		put32(out + len + 8U, (uint32_t)maximum);
		put32(out + len + 12U, (uint32_t)initial);
		len += 16U;
		out[len++] = 0U;
		put32(out + len, 1U);
		len += 4U;
		len += string(out + len, 8U,
			      id == ZLRS_CRSF_PARAM_POWER
				      ? "dBm"
				      : (id == ZLRS_CRSF_PARAM_RATE ? "Hz" : ""));
	}
	return len;
}

static bool entry(zlrs_crsf_device_t *d, uint8_t destination, uint8_t id, uint8_t chunk)
{
	uint8_t data[PARAM_BUFFER_LEN];
	uint8_t payload[ZLRS_CRSF_PAYLOAD_MAX_LEN];
	const uint16_t total = parameter(d, id, data);
	const uint16_t offset = (uint16_t)chunk * PARAM_CHUNK_LEN;

	if (offset >= total) {
		return true; /* Invalid chunk: no response, host can restart its read. */
	}
	const uint16_t remaining = total - offset;
	const uint16_t take = remaining < PARAM_CHUNK_LEN ? remaining : PARAM_CHUNK_LEN;

	payload[0] = destination;
	payload[1] = d->address;
	payload[2] = id;
	payload[3] = (uint8_t)((total - offset - take + PARAM_CHUNK_LEN - 1U) / PARAM_CHUNK_LEN);
	memcpy(payload + 4U, data + offset, take);
	return emit(d, ZLRS_CRSF_TYPE_PARAMETER_ENTRY, payload, (uint8_t)(take + 4U));
}

static bool command_reply(zlrs_crsf_device_t *d, uint8_t destination, uint8_t group,
			  uint8_t command, const uint8_t *data, uint8_t len)
{
	uint8_t payload[ZLRS_CRSF_PAYLOAD_MAX_LEN];

	payload[0] = destination;
	payload[1] = d->address;
	payload[2] = group;
	payload[3] = command;
	memcpy(payload + 4U, data, len);
	payload[4U + len] = zlrs_crsf_command_crc(payload, (uint8_t)(4U + len));
	return emit(d, ZLRS_CRSF_TYPE_COMMAND, payload, (uint8_t)(5U + len));
}

static bool write_parameter(zlrs_crsf_device_t *d, const uint8_t *p, uint8_t len)
{
	const uint8_t id = p[2];
	if (id == ZLRS_CRSF_PARAM_BIND || id == ZLRS_CRSF_PARAM_REBOOT) {
		if (len != 4U) {
			return true;
		}
		uint8_t *status = id == ZLRS_CRSF_PARAM_BIND ? &d->bind_status : &d->reboot_status;
		const uint8_t action = p[3];

		if (id == ZLRS_CRSF_PARAM_REBOOT && action == 1U) {
			*status = 3U;
		} else if ((id == ZLRS_CRSF_PARAM_BIND && action == 1U) ||
			   (id == ZLRS_CRSF_PARAM_REBOOT && action == 4U && *status == 3U) ||
			   action == 5U) {
			if (d->ops.command != NULL &&
			    d->ops.command(d->ops.ctx, 0x10U,
					   id == ZLRS_CRSF_PARAM_BIND
						   ? (action == 5U ? 2U : 1U)
						   : (action == 5U ? 0x81U : 0x80U),
					   NULL, 0U)) {
				*status = action == 5U ? 0U : 2U;
			}
		}
		return entry(d, p[1], id, 0U);
	}
	const bool wide = id == ZLRS_CRSF_PARAM_RATE || id == ZLRS_CRSF_PARAM_POWER ||
			  id == ZLRS_CRSF_PARAM_MODEL;
	if (len != (wide ? 7U : 4U)) {
		return true;
	}
	const int32_t value = wide ? (int32_t)get32(p + 3U) : p[3];

	if (id == ZLRS_CRSF_PARAM_ROOT || id >= ZLRS_CRSF_PARAM_VERSION || d->ops.write == NULL ||
	    !d->ops.write(d->ops.ctx, id, value)) {
		return entry(d, p[1], id, 0U); /* Return unchanged definition on rejected writes. */
	}
	d->values[id] = value;
	uint8_t reply[7];

	memcpy(reply, p, len);
	reply[0] = p[1];
	reply[1] = d->address;
	return emit(d, ZLRS_CRSF_TYPE_PARAMETER_WRITE, reply, len);
}

enum zlrs_crsf_dispatch zlrs_crsf_device_handle(zlrs_crsf_device_t *d, uint8_t type,
						const uint8_t *p, uint8_t len)
{
	if (!zlrs_crsf_extended(type)) {
		return ZLRS_CRSF_ROUTE;
	}
	if (p == NULL || len < 2U) {
		return ZLRS_CRSF_INVALID;
	}
	if (p[0] != 0U && p[0] != d->address) {
		return ZLRS_CRSF_ROUTE;
	}
	bool sent = true;
	if (type == ZLRS_CRSF_TYPE_DEVICE_PING) {
		sent = zlrs_crsf_device_info(d, p[1]);
	} else if (type == ZLRS_CRSF_TYPE_PARAMETER_READ && len >= 4U) {
		sent = entry(d, p[1], p[2], p[3]);
	} else if (type == ZLRS_CRSF_TYPE_PARAMETER_WRITE && len >= 4U) {
		sent = write_parameter(d, p, len);
	} else if (type == ZLRS_CRSF_TYPE_COMMAND) {
		if (!zlrs_crsf_command_valid(p, len)) {
			return ZLRS_CRSF_INVALID;
		}
		const uint8_t group = p[2];
		const uint8_t command = p[3];
		if (group == 0xFFU || (group == 0x0AU && command == 0x71U) ||
		    (group == 0x10U && command == 0x07U)) {
			return ZLRS_CRSF_HANDLED; /* Replies must not create reply loops. */
		}
		if (group == 0x0AU && command == 0x70U && len == 10U) {
			const uint8_t reply[] = {
				p[4], d->ops.speed != NULL &&
					      d->ops.speed(d->ops.ctx, p[4], get32(p + 5U))};

			sent = command_reply(d, p[1], group, 0x71U, reply, sizeof(reply));
		} else if (group == 0x10U && command == 0x06U && len == 5U) {
			const uint8_t model = (uint8_t)d->values[ZLRS_CRSF_PARAM_MODEL];

			sent = command_reply(d, p[1], group, 0x07U, &model, 1U);
		} else {
			const bool model = group == 0x10U && command == 0x05U && len == 6U;
			bool accepted = false;

			if (model || (group == 0x10U && len == 5U &&
				      (command == 1U || command == 2U || command == 0x0AU ||
				       command == 0x0BU))) {
				accepted = d->ops.command != NULL &&
					   d->ops.command(d->ops.ctx, group, command, p + 4U,
							  len - 5U);
			}
			if (accepted && model) {
				d->values[ZLRS_CRSF_PARAM_MODEL] = p[4];
			}
			const uint8_t ack[] = {command, accepted ? 1U : 0U, 0U};

			/* ACK command ID is 0xff; data starts with original ID and subcommand. */
			sent = command_reply(d, p[1], 0xFFU, group, ack, sizeof(ack));
		}
	} else {
		return p[0] == 0U ? ZLRS_CRSF_ROUTE : ZLRS_CRSF_HANDLED;
	}
	return sent ? ZLRS_CRSF_HANDLED : ZLRS_CRSF_RETRY;
}

bool zlrs_crsf_device_timing(zlrs_crsf_device_t *d, uint32_t interval_us, int32_t offset_us)
{
	uint8_t payload[11] = {ZLRS_CRSF_ADDR_HANDSET, d->address, 0x10U};

	if (interval_us == 0U || interval_us > UINT32_MAX / 10U || offset_us > INT32_MAX / 10 ||
	    offset_us < INT32_MIN / 10) {
		return false;
	}
	put32(payload + 3U, interval_us * 10U);
	put32(payload + 7U, (uint32_t)(offset_us * 10));
	return emit(d, ZLRS_CRSF_TYPE_RADIO, payload, sizeof(payload));
}
