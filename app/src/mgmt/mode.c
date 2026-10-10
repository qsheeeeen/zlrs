/*
 * Management mode, see mgmt/mode.h: the session the management protocol runs in, and the device
 * commands it carries. The protocol itself (framing, dispatch) is core/mgmt; this is what each
 * command decodes wire arguments and calls the configuration/RF business owner.
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

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "data/data.h"
#include "config/config.h"
#include "rf/rf.h"
#include "platform/caps.h"
#include "platform/recovery.h"
#include "platform/slot_timer.h"
#include "platform/watchdog.h"
#include "storage/log.h"
#include "zlrs/mgmt/mgmt.h"

LOG_MODULE_REGISTER(zlrs_mgmt_mode, CONFIG_APP_LOG_LEVEL);

ZBUS_CHAN_DEFINE(zlrs_mode_chan, struct zlrs_mode, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

static struct zlrs_mode mode;
static bool mode_pending;
static atomic_t session_requested;

void zlrs_mgmt_request_session(void)
{
	atomic_set(&session_requested, 1);
}

static void publish_mode(void)
{
	if (mode_pending && zbus_chan_pub(&zlrs_mode_chan, &mode, K_NO_WAIT) == 0) {
		mode_pending = false;
	}
}

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
static K_SEM_DEFINE(cfg_rx_ready, 0, 1);
/* Only a host ENTER needs an ACK; shell entry must not send an unsolicited response. */
static bool cfg_enter_pending;
static uint8_t cfg_tx_line[ZLRS_MGMT_FRAME_MAX_LEN];
static uint8_t cfg_tx_pending;

static void cfg_port_isr(const struct device *dev, void *user_data)
{
	struct cfg_port *const p = user_data;

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t chunk[32];
			const int n = uart_fifo_read(dev, chunk, sizeof(chunk));

			if (n > 0) {
				const uint32_t written = ring_buf_put(&p->rx, chunk, (uint32_t)n);

				p->rx_dropped += (uint32_t)n - written;
				if (written > 0U) {
					k_sem_give(&cfg_rx_ready);
				}
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

	if (ring_buf_space_get(&cfg_port.tx) < len) {
		return 0U;
	}
	written = ring_buf_put(&cfg_port.tx, buf, len);
	if (written > 0U) {
		uart_irq_tx_enable(cfg_port.dev);
	}

	return (uint16_t)written;
}

/* Keep the completed action's entire reply until the CDC ring admits it. */
static void flush_reply(void)
{
	if (cfg_tx_pending != 0U && mgmt_write(cfg_tx_line, cfg_tx_pending) == cfg_tx_pending) {
		cfg_tx_pending = 0U;
	}
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
	publish_mode();
	if (atomic_cas(&session_requested, 1, 0)) {
		return true;
	}
	static zlrs_mgmt_parser_t parser;
	static bool parser_ready;
	uint8_t byte;

	if (!parser_ready) {
		zlrs_mgmt_parser_init(&parser);
		parser_ready = true;
	}

	/* Stop at ENTER so any following command stays queued for the ready session. */
	flush_reply();
	for (uint16_t i = 0U;
	     i < ZLRS_MGMT_CHUNK && cfg_tx_pending == 0U && mgmt_read(&byte, 1U) == 1U; i++) {
		if (zlrs_mgmt_feed(&parser, byte) != 1) {
			continue;
		}

		const uint8_t cmd = parser.cmd;
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
			len = zlrs_mgmt_encode(cfg_tx_line, sizeof(cfg_tx_line), ZLRS_MGMT_CMD_ACK,
					       payload, sizeof(payload));
			cfg_tx_pending = len;
			flush_reply();
		}

		/* Outside a session everything answers "wrong state"; EXIT is a no-op */
		payload[0] = cmd;
		payload[1] = (cmd == ZLRS_MGMT_CMD_EXIT) ? ZLRS_MGMT_OK : ZLRS_MGMT_E_STATE;

		len = zlrs_mgmt_encode(cfg_tx_line, sizeof(cfg_tx_line), ZLRS_MGMT_CMD_ACK, payload,
				       sizeof(payload));
		cfg_tx_pending = len;
		flush_reply();
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
/* ---- backend: land commands in NVS and the link layer --------------------- */

/* Wire decoding belongs here; configuration validation and persistence have one owner. */
static int config_result(int rc)
{
	if (rc == 0) {
		return ZLRS_MGMT_OK;
	}
	if (rc == -EINVAL) {
		return ZLRS_MGMT_E_PARAM;
	}
	return rc == -EIO ? ZLRS_MGMT_E_STORE : ZLRS_MGMT_E_STATE;
}

static int cfg_save_setting(void *ctx, uint8_t cmd, const uint8_t *value, uint8_t len)
{
	(void)ctx;
	enum zlrs_config_key key;
	int32_t decoded;
	switch (cmd) {
	case ZLRS_MGMT_CMD_SET_BAND:
		key = ZLRS_CONFIG_BAND;
		break;
	case ZLRS_MGMT_CMD_SET_BAUD:
		key = ZLRS_CONFIG_DATA_BAUD;
		break;
	case ZLRS_MGMT_CMD_SET_REGION:
		key = ZLRS_CONFIG_REGION;
		break;
	case ZLRS_MGMT_CMD_SET_POWER:
		key = ZLRS_CONFIG_POWER;
		break;
	case ZLRS_MGMT_CMD_SET_ROLE:
		key = ZLRS_CONFIG_ROLE;
		break;
	case ZLRS_MGMT_CMD_SET_PROFILE:
		key = ZLRS_CONFIG_PROFILE;
		break;
	default:
		return ZLRS_MGMT_E_UNKNOWN;
	}
	if (len != (key == ZLRS_CONFIG_DATA_BAUD ? 4U : 1U)) {
		return ZLRS_MGMT_E_PARAM;
	}
	if (key == ZLRS_CONFIG_DATA_BAUD) {
		decoded = (uint32_t)value[0] | (uint32_t)value[1] << 8 | (uint32_t)value[2] << 16 |
			  (uint32_t)value[3] << 24;
	} else {
		decoded = key == ZLRS_CONFIG_POWER ? (int8_t)value[0] : value[0];
	}
	return config_result(zlrs_config_set(key, decoded));
}

static int cfg_save_root_key(void *ctx, const uint8_t key[ZLRS_KEY_LEN])
{
	(void)ctx;
	return config_result(zlrs_config_set_root_key(key));
}

static int cfg_read_status(void *ctx, zlrs_mgmt_status_t *out)
{
	(void)ctx;
	const zlrs_rf_status_t rf = zlrs_rf_status();
	const zlrs_config_state_t config = zlrs_config_state();

	*out = (zlrs_mgmt_status_t){
		.version = ZLRS_MGMT_VERSION,
		.role = rf.role,
		.profile = rf.profile,
		.state = rf.state,
		.lq_percent = rf.lq_percent,
		.band = ZLRS_RF_BAND,
		.region = config.saved.region,
		.tx_power_dbm = config.saved.power_dbm,
		.data_baud = config.saved.data_baud,
	};
	if (rf.data_band == ZLRS_DATA_BAND_SUBG) {
		out->flags |= ZLRS_MGMT_FLAG_DATA_SUBG;
	}
	if (rf.configured) {
		out->flags |= ZLRS_MGMT_FLAG_KEY;
	}
	if (rf.paired) {
		out->flags |= ZLRS_MGMT_FLAG_PAIRED;
	}
	if (rf.session_valid) {
		out->flags |= ZLRS_MGMT_FLAG_SESSION;
	}
	memcpy(out->dev_id, rf.dev_id, ZLRS_DEV_ID_LEN);
	memcpy(out->peer_id, rf.peer_id, ZLRS_DEV_ID_LEN);
	return ZLRS_MGMT_OK;
}

static int cfg_new_dev_id(void *ctx)
{
	(void)ctx;
	return config_result(zlrs_config_new_identity());
}

static int cfg_pair(void *ctx)
{
	(void)ctx;
	return config_result(zlrs_rf_pair_start());
}

static int cfg_erase_pair(void *ctx)
{
	(void)ctx;
	return config_result(zlrs_config_erase_pairing());
}

static int cfg_factory_reset(void *ctx)
{
	(void)ctx;
	return config_result(zlrs_config_factory_reset());
}

static int cfg_enter_recovery(void *vctx)
{
	(void)vctx;

	/* Returns only when this image has no bootloader to reboot into */
	return (zlrs_platform_recovery_enter() == 0) ? ZLRS_MGMT_OK : ZLRS_MGMT_E_STATE;
}

/* Host confirms this upgrade; the whole flow depends on it (see doc/07-firmware.md) */
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

#if defined(CONFIG_ZLRS_APP_DEBUG)
static uint8_t cfg_debug_ack(uint8_t cmd, uint8_t status, uint8_t *out, size_t out_len)
{
	const uint8_t payload[2] = {cmd, status};

	return zlrs_mgmt_encode(out, out_len, ZLRS_MGMT_CMD_ACK, payload, sizeof(payload));
}

static uint8_t cfg_debug_report(const uint8_t *request, uint8_t request_len, uint8_t *out,
				size_t out_len)
{
	uint8_t body[ZLRS_MGMT_PAYLOAD_MAX_LEN];
	uint16_t offset;
	uint16_t total_len = 0U;
	uint8_t written = 0U;

	if (request_len != ZLRS_MGMT_DEBUG_REPORT_REQ_LEN) {
		return cfg_debug_ack(ZLRS_MGMT_CMD_DEBUG_REPORT, ZLRS_MGMT_E_PARAM, out, out_len);
	}

	offset = (uint16_t)request[1] | ((uint16_t)request[2] << 8);
	const int rc =
		zlrs_rf_debug_report(request[0], offset, &body[ZLRS_MGMT_DEBUG_REPORT_HEADER_LEN],
				     ZLRS_MGMT_DEBUG_REPORT_CHUNK_MAX, &total_len, &written);

	if (rc != ZLRS_MGMT_OK) {
		return cfg_debug_ack(ZLRS_MGMT_CMD_DEBUG_REPORT, (uint8_t)rc, out, out_len);
	}
	if (written > ZLRS_MGMT_DEBUG_REPORT_CHUNK_MAX || offset > total_len ||
	    (uint32_t)offset + written > total_len || (offset < total_len && written == 0U)) {
		return cfg_debug_ack(ZLRS_MGMT_CMD_DEBUG_REPORT, ZLRS_MGMT_E_STATE, out, out_len);
	}

	body[0] = request[0];
	body[1] = (uint8_t)(offset & 0xFFU);
	body[2] = (uint8_t)(offset >> 8);
	body[3] = (uint8_t)(total_len & 0xFFU);
	body[4] = (uint8_t)(total_len >> 8);

	return zlrs_mgmt_encode(out, out_len, ZLRS_MGMT_CMD_DEBUG_REPORT_RESP, body,
				(uint8_t)(ZLRS_MGMT_DEBUG_REPORT_HEADER_LEN + written));
}
#endif

/* ---- session ------------------------------------------------------------- */

int zlrs_mgmt_mode_run(void)
{
	const uint32_t timeout_ms = (uint32_t)CONFIG_ZLRS_APP_MGMT_TIMEOUT_S * 1000U;
	uint32_t last_activity_ms;
	bool exit_requested = false;
	/* Protocol callbacks use copied status and common business operations. */
	const zlrs_mgmt_backend_t backend = {
		.ctx = NULL,
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
	mode.session = true;
	mode_pending = true;
	publish_mode();

	/* ENTER acknowledges readiness, including the queued logs. The watchdog is already
	 * disarmed, and the idle timeout starts only after this synchronous flush. */
	mgmt_log_flush();
	if (cfg_enter_pending) {
		const uint8_t len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_ENTER, NULL, 0U,
						       cfg_tx_line, sizeof(cfg_tx_line));

		cfg_tx_pending = len;
		flush_reply();
		cfg_enter_pending = false;
	}
	last_activity_ms = k_uptime_get_32();

	while (!exit_requested && (uint32_t)(k_uptime_get_32() - last_activity_ms) < timeout_ms) {
		uint8_t byte;
		bool any = false;

		zlrs_watchdog_feed();
		publish_mode();
		zlrs_config_poll();
		zlrs_link_data_poll();
		(void)log_process();

		/* Radio stopped in management mode: data port bytes are dropped and counted */
		zlrs_link_data_discard_rx();

		flush_reply();
		for (uint16_t i = 0U;
		     i < ZLRS_MGMT_CHUNK && cfg_tx_pending == 0U && mgmt_read(&byte, 1U) == 1U;
		     i++) {
			any = true;

			const int result = zlrs_mgmt_feed(&cfg_parser, byte);

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

			uint8_t len;

#if defined(CONFIG_ZLRS_APP_DEBUG)
			if (cfg_parser.cmd == ZLRS_MGMT_CMD_DEBUG_REPORT) {
				len = cfg_debug_report(ZLRS_MGMT_PAYLOAD(&cfg_parser),
						       cfg_parser.payload_len, cfg_tx_line,
						       sizeof(cfg_tx_line));
			} else
#endif
			{
				len = zlrs_mgmt_dispatch(
					&backend, cfg_parser.cmd, ZLRS_MGMT_PAYLOAD(&cfg_parser),
					cfg_parser.payload_len, cfg_tx_line, sizeof(cfg_tx_line));
			}

			if (len > 0U) {
				cfg_tx_pending = len;
				flush_reply();
			}
		}

		if (!any) {
			/* Incoming bytes wake this wait immediately; the timeout only bounds
			 * housekeeping latency when the host is idle. */
			(void)k_sem_take(&cfg_rx_ready, K_MSEC(CFG_EXIT_POLL_MS));
		}
	}

	LOG_INF("management mode finished");

	mode.session = false;
	mode_pending = true;
	publish_mode();

	zlrs_watchdog_arm();

	return exit_requested ? 0 : 1;
}

#else /* !CONFIG_ZLRS_MGMT */

bool zlrs_mgmt_mode_image_confirmed(void)
{
	return false;
}

int zlrs_mgmt_init(void)
{
	return -1;
}

bool zlrs_mgmt_poll_enter(void)
{
	publish_mode();
	return false;
}

/* Management protocol compiled out: the application still calls in, this says "not available" */
int zlrs_mgmt_mode_run(void)
{
	return -1;
}

#endif /* CONFIG_ZLRS_MGMT */
