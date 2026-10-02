/*
 * Management mode, see mgmt/mode.h: the session the management protocol runs in, and the device
 * commands it carries. The protocol itself (framing, dispatch) is core/mgmt; this is what each
 * command does to *this* device, and the context it works on comes from the application when a
 * session opens.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mgmt/mode.h"

#include <zephyr/app_version.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/storage/flash_map.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "link/data.h"
#include "platform/caps.h"
#include "platform/recovery.h"
#include "platform/watchdog.h"
#include "state/sys_bus.h"
#include "storage/log.h"
#include "storage/settings.h"
#include "zlrs/mgmt/mgmt.h"
#include "zlrs/region/region.h"

LOG_MODULE_REGISTER(zlrs_mgmt_mode, CONFIG_APP_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZLRS_MGMT)

#define CFG_EXIT_POLL_MS 50U
#define ZLRS_MGMT_CHUNK  32U

#define CFG_PORT_RX_RING_LEN 512U
#define CFG_PORT_TX_RING_LEN 512U

struct cfg_port {
	const struct device *dev;
	struct ring_buf rx;
	struct ring_buf tx;
	uint8_t rx_mem[CFG_PORT_RX_RING_LEN];
	uint8_t tx_mem[CFG_PORT_TX_RING_LEN];
	uint32_t rx_dropped;
	bool ready;
};

static struct cfg_port cfg_port;
/* Only a host ENTER needs an ACK; shell entry must not send an unsolicited response. */
static bool cfg_enter_pending;

static void cfg_port_isr(const struct device *dev, void *user_data)
{
	struct cfg_port *const p = user_data;

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t chunk[32];
			const int n = uart_fifo_read(dev, chunk, sizeof(chunk));

			if (n > 0 && ring_buf_put(&p->rx, chunk, (uint32_t)n) != (uint32_t)n) {
				p->rx_dropped++;
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *claim = NULL;
			const uint32_t claimed = ring_buf_get_claim(&p->tx, &claim, 64U);

			if (claimed == 0U) {
				uart_irq_tx_disable(dev);
				continue;
			}

			const int sent = uart_fifo_fill(dev, claim, (int)claimed);

			ring_buf_get_finish(&p->tx, (sent > 0) ? (uint32_t)sent : 0U);
		}
	}
}

int zlrs_mgmt_init(void)
{
#if ZLRS_PORT_MGMT
	const struct device *const dev = DEVICE_DT_GET(DT_ALIAS(zlrs_mgmt_cdc));

	if (!device_is_ready(dev)) {
		LOG_ERR("management port %s not ready", dev->name);
		return -1;
	}

	ring_buf_init(&cfg_port.rx, sizeof(cfg_port.rx_mem), cfg_port.rx_mem);
	ring_buf_init(&cfg_port.tx, sizeof(cfg_port.tx_mem), cfg_port.tx_mem);
	cfg_port.dev = dev;

	/* Check it: without IRQ support it is -ENOSYS/-ENOTSUP, and ignoring that loses commands */
	if (uart_irq_callback_user_data_set(dev, cfg_port_isr, &cfg_port) != 0) {
		LOG_ERR("management port %s: RX setup failed", dev->name);
		cfg_port.dev = NULL;
		return -1;
	}

	uart_irq_rx_enable(dev);
	cfg_port.ready = true;

	LOG_INF("management port ready (%s)", dev->name);

	return 0;
#else
	LOG_WRN("no zlrs-mgmt-cdc alias: management port disabled");

	return -1;
#endif
}

static uint16_t mgmt_read(uint8_t *buf, uint16_t len)
{
	if (!cfg_port.ready) {
		return 0U;
	}

	return (uint16_t)ring_buf_get(&cfg_port.rx, buf, len);
}

static uint16_t mgmt_write(const uint8_t *buf, uint16_t len)
{
	uint32_t written;

	if (!cfg_port.ready) {
		return 0U;
	}

	written = ring_buf_put(&cfg_port.tx, buf, len);
	if (written > 0U) {
		uart_irq_tx_enable(cfg_port.dev);
	}

	return (uint16_t)written;
}

static void mgmt_log_flush(void)
{
	log_flush();

	/* With no log thread, log_flush() drains messages but does not emit its completion
	 * event. Notify the backends here so the file backend synchronizes before ENTER ACK. */
	if (!IS_ENABLED(CONFIG_LOG_PROCESS_THREAD)) {
		STRUCT_SECTION_FOREACH(log_backend, backend) {
			if (log_backend_is_active(backend)) {
				log_backend_notify(backend, LOG_BACKEND_EVT_PROCESS_THREAD_DONE,
						   NULL);
			}
		}
	}
}

bool zlrs_mgmt_poll_enter(void)
{
	static zlrs_mgmt_parser_t parser;
	static bool parser_ready;
	uint8_t byte;

	if (!parser_ready) {
		zlrs_mgmt_parser_init(&parser);
		parser_ready = true;
	}

	/* Stop at ENTER so any following command stays queued for the ready session. */
	for (uint16_t i = 0U; i < ZLRS_MGMT_CHUNK && mgmt_read(&byte, 1U) == 1U; i++) {
		if (zlrs_mgmt_feed(&parser, byte) != 1) {
			continue;
		}

		const uint8_t cmd = parser.cmd;
		uint8_t reply[ZLRS_MGMT_FRAME_MAX_LEN];
		uint8_t payload[2];
		uint8_t len;

		if (cmd == ZLRS_MGMT_CMD_ENTER) {
			cfg_enter_pending = true;
			return true;
		}

		/* Recovery needs no setting up: reboot once acknowledged, the main loop never returns */
		if (cmd == ZLRS_MGMT_CMD_RECOVERY && zlrs_platform_recovery_enter() == 0) {
			payload[0] = cmd;
			payload[1] = ZLRS_MGMT_OK;
			len = zlrs_mgmt_encode(reply, sizeof(reply), ZLRS_MGMT_CMD_ACK, payload,
					       sizeof(payload));
			(void)mgmt_write(reply, len);
		}

		/* Outside a session everything answers "wrong state"; EXIT is a no-op */
		payload[0] = cmd;
		payload[1] = (cmd == ZLRS_MGMT_CMD_EXIT) ? ZLRS_MGMT_OK : ZLRS_MGMT_E_STATE;

		len = zlrs_mgmt_encode(reply, sizeof(reply), ZLRS_MGMT_CMD_ACK, payload,
				       sizeof(payload));
		(void)mgmt_write(reply, len);
	}

	return false;
}

/* The host sent its confirm command (touched from the link thread only) */
static bool cfg_image_confirmed;

bool zlrs_mgmt_mode_image_confirmed(void)
{
	return cfg_image_confirmed;
}

/* ---- session ------------------------------------------------------------- */

static zlrs_mgmt_parser_t cfg_parser;
static uint8_t cfg_tx_line[ZLRS_MGMT_FRAME_MAX_LEN];
/* ---- backend: land commands in NVS and the link layer --------------------- */

/*
 * The simple settings, one row each. Everything else about them is the same: the value is checked,
 * written to its NVS record and (for the two that the running device uses right away) applied. So
 * this table is the only per-setting code in the file, and the storage side has the matching row
 * that says which record it is (app/src/storage/settings.c).
 */
struct cfg_setting {
	uint8_t cmd;
	enum zlrs_setting setting;
	const char *name;
	const char *effect; /* what the host should expect after the ACK */
	int (*check)(const zlrs_mgmt_ctx_t *ctx, const uint8_t *value, uint8_t len);
	void (*applied)(const zlrs_mgmt_ctx_t *ctx, const uint8_t *value, uint8_t len);
};

static uint32_t le32(const uint8_t *value)
{
	return (uint32_t)value[0] | ((uint32_t)value[1] << 8) | ((uint32_t)value[2] << 16) |
	       ((uint32_t)value[3] << 24);
}

static int check_baud(const zlrs_mgmt_ctx_t *ctx, const uint8_t *value, uint8_t len)
{
	const uint32_t baud = le32(value);

	ARG_UNUSED(ctx);
	ARG_UNUSED(len);

	return (baud >= 9600U && baud <= 4000000U) ? ZLRS_MGMT_OK : ZLRS_MGMT_E_PARAM;
}

static int check_region(const zlrs_mgmt_ctx_t *ctx, const uint8_t *value, uint8_t len)
{
	const zlrs_region_t *const want = zlrs_region_get(value[0]);

	ARG_UNUSED(len);

	if (want == NULL) {
		return ZLRS_MGMT_E_PARAM;
	}

	/* The region's sub-G hardware must match the board (-5 = SKU mismatch); refuse otherwise */
	return (zlrs_region_validate(want, ZLRS_RF_DESIGN, ctx->tx_power_dbm) == -5)
		       ? ZLRS_MGMT_E_PARAM
		       : ZLRS_MGMT_OK;
}

static int check_power(const zlrs_mgmt_ctx_t *ctx, const uint8_t *value, uint8_t len)
{
	const zlrs_region_t *const region = zlrs_region_get(ctx->region_id);

	ARG_UNUSED(len);

	if (region == NULL) {
		return ZLRS_MGMT_E_STATE;
	}

	return ((int8_t)value[0] <= region->max_tx_power_dbm) ? ZLRS_MGMT_OK : ZLRS_MGMT_E_PARAM;
}

static int check_profile(const zlrs_mgmt_ctx_t *ctx, const uint8_t *value, uint8_t len)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(len);

	/* P0 is a debug-image move and P2 is not implemented: the image decides, not the host */
	return zlrs_mgmt_mode_profile_allowed(value[0]) ? ZLRS_MGMT_OK : ZLRS_MGMT_E_PARAM;
}

static int check_mode(const zlrs_mgmt_ctx_t *ctx, const uint8_t *value, uint8_t len)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(len);

	if (value[0] == ZLRS_MODE_DATA24) {
		return IS_ENABLED(CONFIG_ZLRS_APP_LINK_DATA) ? ZLRS_MGMT_OK : ZLRS_MGMT_E_PARAM;
	}
	return IS_ENABLED(CONFIG_ZLRS_APP_LINK_CONTROL) ? ZLRS_MGMT_OK : ZLRS_MGMT_E_PARAM;
}

/* The two settings the running device uses at once, not on the next boot */
static void apply_failsafe(const zlrs_mgmt_ctx_t *ctx, const uint8_t *value, uint8_t len)
{
	if (ctx->failsafe != NULL) {
		memcpy(ctx->failsafe, value, len);
	}
	/* The new values travel to the peer with the next pairing */
	zlrs_link_set_failsafe(ctx->control, value, len);
}

static const struct cfg_setting cfg_settings[] = {
	{.cmd = ZLRS_MGMT_CMD_SET_MODE,
	 .setting = ZLRS_SETTING_MODE,
	 .name = "operating mode",
	 .effect = "it takes effect on the next boot",
	 .check = check_mode,
	 .applied = NULL},
	{.cmd = ZLRS_MGMT_CMD_SET_FAILSAFE,
	 .setting = ZLRS_SETTING_FAILSAFE,
	 .name = "failsafe values",
	 .effect = "they go to the peer with the next pairing",
	 .check = NULL,
	 .applied = apply_failsafe},
	{.cmd = ZLRS_MGMT_CMD_SET_BAUD,
	 .setting = ZLRS_SETTING_BAUD,
	 .name = "data baud",
	 .effect = "it takes effect on the next boot",
	 .check = check_baud,
	 .applied = NULL},
	{.cmd = ZLRS_MGMT_CMD_SET_REGION,
	 .setting = ZLRS_SETTING_REGION,
	 .name = "region",
	 .effect = "it takes effect on the next boot",
	 .check = check_region,
	 .applied = NULL},
	{.cmd = ZLRS_MGMT_CMD_SET_POWER,
	 .setting = ZLRS_SETTING_POWER,
	 .name = "tx power",
	 .effect = "it takes effect on the next boot",
	 .check = check_power,
	 .applied = NULL},
	{.cmd = ZLRS_MGMT_CMD_SET_ROLE,
	 .setting = ZLRS_SETTING_ROLE,
	 .name = "role",
	 .effect = "it takes effect on the next boot",
	 .check = NULL, /* TX and RX are the whole range, and the protocol rejects anything else */
	 .applied = NULL},
	{.cmd = ZLRS_MGMT_CMD_SET_PROFILE,
	 .setting = ZLRS_SETTING_PROFILE,
	 .name = "security profile",
	 .effect = "it takes effect on the next boot",
	 .check = check_profile,
	 .applied = NULL},
};

static const struct cfg_setting *setting_row(uint8_t cmd)
{
	for (size_t i = 0U; i < ARRAY_SIZE(cfg_settings); i++) {
		if (cfg_settings[i].cmd == cmd) {
			return &cfg_settings[i];
		}
	}

	return NULL;
}

/*
 * One path for every simple setting: which row it is, whether the value is usable, the NVS write,
 * and the effect on the running device. The value arrives as the host sent it and is never decoded
 * twice: check() may read it, save() stores it as it is.
 */
static int cfg_save_setting(void *vctx, uint8_t cmd, const uint8_t *value, uint8_t len)
{
	const zlrs_mgmt_ctx_t *ctx = vctx;
	const struct cfg_setting *const row = setting_row(cmd);

	if (row == NULL) {
		return ZLRS_MGMT_E_UNKNOWN;
	}
	if (len != zlrs_storage_settings_size(row->setting)) {
		return ZLRS_MGMT_E_PARAM;
	}

	if (row->check != NULL) {
		const int rc = row->check(ctx, value, len);

		if (rc != ZLRS_MGMT_OK) {
			return rc;
		}
	}

	if (zlrs_storage_settings_save(row->setting, value, len) != 0) {
		return ZLRS_MGMT_E_STORE;
	}

	if (row->applied != NULL) {
		row->applied(ctx, value, len);
	}

	LOG_INF("%s written (%s)", row->name, row->effect);

	return ZLRS_MGMT_OK;
}

static int cfg_save_root_key(void *vctx, const uint8_t key[ZLRS_KEY_LEN])
{
	const zlrs_mgmt_ctx_t *ctx = vctx;

	if (zlrs_storage_settings_save(ZLRS_SETTING_ROOT_KEY, key, ZLRS_KEY_LEN) != 0) {
		return ZLRS_MGMT_E_STORE;
	}

	/* Takes effect immediately, no reboot needed */
	zlrs_link_set_root_key(ctx->control, key, k_uptime_get_32());
	zlrs_link_set_root_key(ctx->data, key, k_uptime_get_32());

	LOG_INF("root key written");

	return ZLRS_MGMT_OK;
}

static int cfg_read_status(void *vctx, zlrs_mgmt_status_t *out)
{
	const zlrs_mgmt_ctx_t *ctx = vctx;
	const zlrs_link_t *const link = ctx->active;
	uint8_t stored_region;
	int8_t stored_power;
	uint32_t baud;

	out->version = ZLRS_MGMT_VERSION;
	out->role = link->cfg.role;
	out->profile = link->cfg.profile;
	out->state = link->state;
	out->flags = ctx->mode == ZLRS_MODE_DATA24 ? ZLRS_MGMT_FLAG_DATA24 : 0U;
	if (link->configured) {
		out->flags |= ZLRS_MGMT_FLAG_KEY;
	}
	if (link->paired) {
		out->flags |= ZLRS_MGMT_FLAG_PAIRED;
		memcpy(out->peer_id, link->peer_id, ZLRS_DEV_ID_LEN);
	}
	if (link->session_valid) {
		out->flags |= ZLRS_MGMT_FLAG_SESSION;
	}
	memcpy(out->dev_id, link->dev_id, ZLRS_DEV_ID_LEN);
	out->lq_percent = link->stats.lq_percent;
	out->band = ZLRS_RF_BAND;

	/* Report the stored value first (just written, not yet live), the live one otherwise */
	out->region =
		(zlrs_storage_settings_load(ZLRS_SETTING_REGION, &stored_region,
					    sizeof(stored_region)) == (int)sizeof(stored_region))
			? stored_region
			: ctx->region_id;
	out->tx_power_dbm =
		(zlrs_storage_settings_load(ZLRS_SETTING_POWER, &stored_power,
					    sizeof(stored_power)) == (int)sizeof(stored_power))
			? stored_power
			: ctx->tx_power_dbm;
	if (zlrs_storage_settings_load(ZLRS_SETTING_BAUD, &baud, sizeof(baud)) !=
	    (int)sizeof(baud)) {
		baud = 0U;
	}
	out->data_baud = baud;

	return ZLRS_MGMT_OK;
}

/*
 * Fresh device id on demand: the id is part of the key derivation, so the pairing record that was
 * built on the old one has to go with it (the peer would reject the new id anyway).
 */
static int cfg_new_dev_id(void *vctx)
{
	const zlrs_mgmt_ctx_t *ctx = vctx;
	uint8_t fresh[ZLRS_DEV_ID_LEN];

	if (ctx->device_id == NULL || !zlrs_storage_settings_new_dev_id(fresh)) {
		return ZLRS_MGMT_E_STATE;
	}
	if (zlrs_storage_settings_save(ZLRS_SETTING_DEV_ID, fresh, sizeof(fresh)) != 0) {
		return ZLRS_MGMT_E_STORE;
	}
	(void)zlrs_storage_settings_erase(ZLRS_SETTING_PAIRING);

	memcpy(ctx->device_id, fresh, ZLRS_DEV_ID_LEN);
	zlrs_link_set_dev_id(ctx->control, ctx->device_id);
	zlrs_link_set_dev_id(ctx->data, ctx->device_id);
	LOG_INF("dev id %02x%02x%02x%02x (new, pairing dropped)", fresh[0], fresh[1], fresh[2],
		fresh[3]);

	return ZLRS_MGMT_OK;
}

static int cfg_pair(void *vctx)
{
	const zlrs_mgmt_ctx_t *ctx = vctx;

	if (!ctx->active->configured) {
		return ZLRS_MGMT_E_STATE;
	}

	zlrs_link_press(ctx->active, k_uptime_get_32());
	LOG_INF("pairing triggered by management tool");

	return ZLRS_MGMT_OK;
}

static int cfg_erase_pair(void *vctx)
{
	(void)vctx;

	if (zlrs_storage_settings_erase(ZLRS_SETTING_PAIRING) != 0) {
		return ZLRS_MGMT_E_STORE;
	}

	LOG_INF("pairing erased (takes effect on next boot)");

	return ZLRS_MGMT_OK;
}

static int cfg_factory_reset(void *vctx)
{
	(void)vctx;

	if (zlrs_storage_settings_erase_all() != 0) {
		return ZLRS_MGMT_E_STORE;
	}

	LOG_WRN("factory reset: NVS cleared (takes effect on next boot)");

	return ZLRS_MGMT_OK;
}

static int cfg_enter_recovery(void *vctx)
{
	(void)vctx;

	/* Returns only when this image has no bootloader to reboot into */
	return (zlrs_platform_recovery_enter() == 0) ? ZLRS_MGMT_OK : ZLRS_MGMT_E_STATE;
}

/* Host confirms this upgrade; the whole flow depends on it (see doc/08-firmware.md 6) */
static int cfg_confirm_image(void *ctx)
{
	(void)ctx;

	cfg_image_confirmed = true;
	LOG_INF("host confirmed this build");

	return ZLRS_MGMT_OK;
}

/* Hand the build string to the host so it can check which version is running */
static int cfg_read_build(void *ctx, char *out, size_t out_len)
{
	(void)ctx;

	if (out == NULL || out_len == 0U) {
		return ZLRS_MGMT_E_PARAM;
	}

	(void)snprintf(out, out_len, "%s", APP_VERSION_STRING);

	return ZLRS_MGMT_OK;
}

/*
 * MCUboot hash of the running image: read IMAGE_TLV_SHA256 from this image's TLV area.
 * The update tool tells "the image I just sent" from "already rolled back": build strings
 * can match, hashes cannot. Only MCUboot's public format is used, no internal headers.
 */
static int cfg_read_image_hash(void *ctx, uint8_t out[ZLRS_MGMT_IMAGE_HASH_LEN])
{
	(void)ctx;

#ifdef CONFIG_BOOTLOADER_MCUBOOT
#define MCUBOOT_IMAGE_MAGIC    0x96f3b83dU
#define MCUBOOT_TLV_INFO_MAGIC 0x6907U
#define MCUBOOT_TLV_SHA256     0x10U
	struct {
		uint32_t magic;
		uint32_t load_addr;
		uint16_t hdr_size;
		uint16_t protect_tlv_size;
		uint32_t img_size;
	} __packed hdr;
	const struct flash_area *fa;
	uint32_t off, tlv_end;

	if (flash_area_open(PARTITION_ID(slot0_partition), &fa) != 0) {
		return ZLRS_MGMT_E_STATE;
	}

	if (flash_area_read(fa, 0U, &hdr, sizeof(hdr)) != 0 || hdr.magic != MCUBOOT_IMAGE_MAGIC) {
		flash_area_close(fa);
		return ZLRS_MGMT_E_STATE;
	}

	/* The TLV area starts with an info header: magic(0x6907) + length (including these 4 bytes) */
	off = (uint32_t)hdr.hdr_size + hdr.img_size + hdr.protect_tlv_size;

	{
		uint8_t info[4];
		uint16_t magic;
		uint16_t total;

		if (flash_area_read(fa, off, info, sizeof(info)) != 0) {
			flash_area_close(fa);
			return ZLRS_MGMT_E_STATE;
		}

		magic = (uint16_t)info[0] | ((uint16_t)info[1] << 8);
		total = (uint16_t)info[2] | ((uint16_t)info[3] << 8);

		if (magic != MCUBOOT_TLV_INFO_MAGIC) {
			flash_area_close(fa);
			return ZLRS_MGMT_E_STATE;
		}

		tlv_end = off + total;
		off += 4U;
	}

	while (off + 4U + ZLRS_MGMT_IMAGE_HASH_LEN <= tlv_end) {
		uint8_t tlv[4];
		uint16_t len;
		uint8_t type;

		if (flash_area_read(fa, off, tlv, sizeof(tlv)) != 0) {
			break;
		}
		type = tlv[0];
		len = (uint16_t)tlv[2] | ((uint16_t)tlv[3] << 8);

		if (type == MCUBOOT_TLV_SHA256 && len == ZLRS_MGMT_IMAGE_HASH_LEN) {
			const int rc = flash_area_read(fa, off + 4U, out, ZLRS_MGMT_IMAGE_HASH_LEN);

			flash_area_close(fa);
			return (rc == 0) ? ZLRS_MGMT_OK : ZLRS_MGMT_E_STATE;
		}

		/* 0xFF padding follows the last TLV; type=0xFF means stop */
		if (type == 0xFFU) {
			break;
		}
		off += 4U + len;
	}

	flash_area_close(fa);
#endif

	return ZLRS_MGMT_E_STATE;
}

/*
 * The log store lives in its own file (app/src/storage/log.c): it is the part of the management
 * protocol that touches flash, and it works on boards without a log partition too (E_STATE).
 */
static int cfg_log_info(void *ctx, zlrs_mgmt_log_info_t *out)
{
	ARG_UNUSED(ctx);

	return zlrs_storage_log_info(out);
}

static int cfg_log_read(void *ctx, uint8_t index, uint32_t offset, uint8_t len, uint8_t *out,
			uint8_t *out_len)
{
	ARG_UNUSED(ctx);

	return zlrs_storage_log_read(index, offset, len, out, out_len);
}

/* ---- session ------------------------------------------------------------- */

int zlrs_mgmt_mode_run(const zlrs_mgmt_ctx_t *ctx)
{
	const uint32_t timeout_ms = (uint32_t)CONFIG_ZLRS_APP_MGMT_TIMEOUT_S * 1000U;
	uint32_t last_activity_ms;
	bool exit_requested = false;
	/* The commands work on the caller's live state; the table carries it through to each one */
	const zlrs_mgmt_backend_t backend = {
		.ctx = (void *)ctx,
		.save_root_key = cfg_save_root_key,
		.read_status = cfg_read_status,
		.save_setting = cfg_save_setting,
		.new_dev_id = cfg_new_dev_id,
		.pair = cfg_pair,
		.erase_pair = cfg_erase_pair,
		.factory_reset = cfg_factory_reset,
		.enter_recovery = cfg_enter_recovery,
		.confirm_image = cfg_confirm_image,
		.read_build = cfg_read_build,
		.read_image_hash = cfg_read_image_hash,
		.log_info = cfg_log_info,
		.log_read = cfg_log_read,
	};

	if (!cfg_port.ready) {
		LOG_WRN("no management port: management mode unavailable");
		return -1;
	}

	/* No superframe to race here, so keep the logic as plain as possible */
	zlrs_mgmt_parser_init(&cfg_parser);

	LOG_INF("management mode: %u s idle timeout, exit on EXIT command",
		(unsigned int)CONFIG_ZLRS_APP_MGMT_TIMEOUT_S);

	/* Disarm the watchdog for the session: writing a phrase runs 100000 PBKDF2 rounds (tens of
	 * seconds) and an 8 s watchdog would reset mid-way. A management session is attended:
	 * re-arm on exit. */
	zlrs_watchdog_disarm();

	/* A mode, not an LED state: the LED thread decides how it looks (app/src/ui/led.c) */
	zlrs_sys_publish_mode(&(struct zlrs_mode){.session = true});

	/* ENTER acknowledges readiness, including the queued logs. The watchdog is already
	 * disarmed, and the idle timeout starts only after this synchronous flush. */
	mgmt_log_flush();
	if (cfg_enter_pending) {
		const uint8_t len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_ENTER, NULL, 0U,
						       cfg_tx_line, sizeof(cfg_tx_line));

		(void)mgmt_write(cfg_tx_line, len);
		cfg_enter_pending = false;
	}
	last_activity_ms = k_uptime_get_32();

	while (!exit_requested && (uint32_t)(k_uptime_get_32() - last_activity_ms) < timeout_ms) {
		uint8_t chunk[ZLRS_MGMT_CHUNK];
		bool any = false;

		zlrs_watchdog_feed();
		(void)log_process();

		/* Radio stopped in management mode: data port bytes are dropped and counted */
		zlrs_link_data_discard_rx();

		const uint16_t got = mgmt_read(chunk, sizeof(chunk));
		for (uint16_t i = 0U; i < got; i++) {
			any = true;

			const int result = zlrs_mgmt_feed(&cfg_parser, chunk[i]);

			if (result != 1) {
				continue;
			}

			last_activity_ms = k_uptime_get_32();

			if (cfg_parser.cmd == ZLRS_MGMT_CMD_EXIT) {
				exit_requested = true;
			}

			/* Repeated ENTER has the same log-flush guarantee in an existing session. */
			if (cfg_parser.cmd == ZLRS_MGMT_CMD_ENTER) {
				mgmt_log_flush();
				last_activity_ms = k_uptime_get_32();
			}

			const uint8_t len = zlrs_mgmt_dispatch(
				&backend, cfg_parser.cmd, ZLRS_MGMT_PAYLOAD(&cfg_parser),
				cfg_parser.payload_len, cfg_tx_line, sizeof(cfg_tx_line));

			if (len > 0U) {
				(void)mgmt_write(cfg_tx_line, len);
			}
		}

		if (!any) {
			k_msleep(CFG_EXIT_POLL_MS);
		}
	}

	LOG_INF("management mode finished");

	zlrs_sys_publish_mode(&(struct zlrs_mode){.session = false});

	zlrs_watchdog_arm();

	return exit_requested ? 0 : 1;
}

#else /* !CONFIG_ZLRS_MGMT */

int zlrs_mgmt_init(void)
{
	return -1;
}

bool zlrs_mgmt_poll_enter(void)
{
	return false;
}

/* Management protocol compiled out: the application still calls in, this says "not available" */
int zlrs_mgmt_mode_run(const zlrs_mgmt_ctx_t *ctx)
{
	(void)ctx;

	return -1;
}

#endif /* CONFIG_ZLRS_MGMT */

bool zlrs_mgmt_mode_profile_allowed(uint8_t profile)
{
	if (profile == ZLRS_PROFILE_P1) {
		return true;
	}
	if (profile == ZLRS_PROFILE_P0) {
		/* P0 carries no tag at all, so only a bench image may select it (doc/10-security.md 8) */
		return IS_ENABLED(CONFIG_ZLRS_APP_DEBUG);
	}

	return false; /* P2 is not implemented */
}
