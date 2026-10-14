/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "fixtures.h"
#include "platform/lr2021.h"
#include "platform/slot_timer.h"
#include "platform/watchdog.h"

static zlrs_link_t *data_link;
static uint16_t default_rx_ms;
static uint16_t default_acq_ms;
static atomic_t window_request;
K_SEM_DEFINE(window_done, 0, 1);
K_SEM_DEFINE(raw_start, 0, 1);
static atomic_t raw_ready;
static atomic_t raw_tx;
static atomic_t raw_tx_failed;
static atomic_t raw_rx;
static atomic_t raw_expected;
static atomic_t raw_bad;

/* Fixed 1 MHz bins for the current 2.4 GHz DATA domain. Empty receive windows
 * include idle peer slots; they are not a packet-loss denominator. */
static struct {
	atomic_t tx;
	atomic_t tx_failed;
	atomic_t rx;
	atomic_t empty;
	atomic_t crc;
} channels[79];
static uint32_t channel_crc_previous;

void __real_zlrs_link_init(zlrs_link_t *link, const zlrs_link_cfg_t *cfg,
			   bool (*random)(void *ctx, uint32_t *out), void *random_ctx,
			   const zlrs_link_events_t *events);
int __real_zlrs_lr2021_init(void);
void __real_zlrs_link_complete(zlrs_link_t *link, int status, uint16_t rx_len, int16_t rssi,
			       int8_t snr, uint32_t now_ms);

void __wrap_zlrs_link_init(zlrs_link_t *link, const zlrs_link_cfg_t *cfg,
			   bool (*random)(void *ctx, uint32_t *out), void *random_ctx,
			   const zlrs_link_events_t *events)
{
	__real_zlrs_link_init(link, cfg, random, random_ctx, events);
	data_link = link;
	default_rx_ms = cfg->rx_timeout_ms;
	default_acq_ms = cfg->acq_timeout_ms;
}

void zlrs_test_radio_apply_window_request(zlrs_link_t *link)
{
	const int request = atomic_set(&window_request, 0);

	if (request != 0) {
		const uint16_t ms = (uint16_t)(request - 1);

		link->cfg.rx_timeout_ms = ms == 0U ? default_rx_ms : ms;
		link->cfg.acq_timeout_ms = ms == 0U ? default_acq_ms : ms;
		k_sem_give(&window_done);
	}
}

#if CONFIG_ZLRS_TEST_RAW_MS > 0
static zlrs_link_t raw_link;
static uint32_t raw_last_sequence;
static bool raw_rx_started;
void __wrap_zlrs_link_complete(zlrs_link_t *, int, uint16_t, int16_t, int8_t, uint32_t);

static void raw_run(void)
{
	const bool tx = (data_link->cfg.role == ZLRS_ROLE_LEADER) !=
			IS_ENABLED(CONFIG_ZLRS_TEST_RAW_REVERSE);
	zlrs_radio_req_t req = {
		.freq_hz = CONFIG_ZLRS_TEST_RAW_FREQ_MHZ * 1000000U,
		.mod = data_link->cfg.mod,
		.sync_slot = ZLRS_SYNC_SLOT_DATA,
		.is_tx = tx,
		.tx_power_dbm = data_link->cfg.tx_power_dbm,
		.size = tx ? CONFIG_ZLRS_TEST_RAW_PAYLOAD_BYTES : 0U,
		.max_rx_size = tx ? 0U : ZLRS_FRAME_MAX_LEN,
		.timeout_ms = tx ? 0U : 100U,
		.buf = tx ? raw_link.tx_buf : raw_link.rx_buf,
	};
	memcpy(req.sync_word, data_link->sync_word, sizeof(req.sync_word));
	const uint32_t started = zlrs_slot_timer_now_ms();
	uint32_t sequence = 0U;

	/* RAW runs during radio init, before RF starts its clock. Enable the normal
	 * launch notification so the owner fills a long packet's FIFO tail at SET_TX,
	 * rather than sleeping until TX_DONE or a FIFO underflow. */
	zlrs_slot_timer_start(120000U, 0U, zlrs_lr2021_wake);
	while (zlrs_slot_timer_now_ms() - started < CONFIG_ZLRS_TEST_RAW_MS) {
		sys_put_le32(sequence, raw_link.tx_buf);
		for (uint16_t i = 4U; i < CONFIG_ZLRS_TEST_RAW_PAYLOAD_BYTES; i++) {
			raw_link.tx_buf[i] = (uint8_t)(sequence + i);
		}
		raw_link.req_is_tx = tx;
		raw_link.req_pending = true;
		const uint32_t deadline = zlrs_slot_timer_now_us() + 120000U;
		const uint32_t target = zlrs_slot_timer_now_us() +
					CONFIG_ZLRS_APP_DATA_24_PREPARE_US +
					CONFIG_ZLRS_APP_TX_GUARD_US;

		/* Prepare before the timer-controlled TX target, as the application does. */
		zlrs_lr2021_slot(target, deadline);
		if (zlrs_lr2021_prepare(&req) == 0 &&
		    (tx ? zlrs_lr2021_launch_at(target) : zlrs_lr2021_launch()) == 0) {
			while (raw_link.req_pending &&
			       (int32_t)(deadline - zlrs_slot_timer_now_us()) > 0) {
				zlrs_lr2021_result_t result;
				if (zlrs_lr2021_process_irq(&result)) {
					__wrap_zlrs_link_complete(
						&raw_link,
						result.status == 0 ? ZLRS_RADIO_OK
								   : ZLRS_RADIO_FAIL,
						result.size, result.rssi_dbm, result.snr_db,
						result.completed_ms);
				}
				const int32_t remaining = deadline - zlrs_slot_timer_now_us();

				if (raw_link.req_pending && remaining > 0) {
					(void)zlrs_lr2021_wait_us((uint32_t)remaining);
				}
			}
		}
		if (raw_link.req_pending) {
			zlrs_lr2021_abort();
			raw_link.req_pending = false;
			if (tx) {
				atomic_inc(&raw_tx_failed);
			}
		}
		sequence++;
		zlrs_watchdog_feed();
	}
	/* Test startup ends before the production timer/superframe starts. */
	zlrs_lr2021_abort();
	zlrs_slot_timer_stop();
	zlrs_lr2021_diag_t diag;
	zlrs_lr2021_diag(&diag);
	channel_crc_previous = diag.rx_crc_errors;
}

int __wrap_zlrs_lr2021_init(void)
{
	const int rc = __real_zlrs_lr2021_init();

	if (rc == 0 && data_link != NULL) {
		atomic_set(&raw_ready, 1);
		const int64_t deadline = k_uptime_get() + 45000;

		while (k_sem_take(&raw_start, K_MSEC(20)) != 0) {
			zlrs_watchdog_feed();
			if (k_uptime_get() >= deadline) {
				sys_reboot(SYS_REBOOT_COLD);
			}
		}
		atomic_clear(&raw_ready);
		raw_run();
	}
	return rc;
}
#endif

void __wrap_zlrs_link_complete(zlrs_link_t *link, int status, uint16_t rx_len, int16_t rssi,
			       int8_t snr, uint32_t now_ms)
{
#if CONFIG_ZLRS_TEST_RAW_MS > 0
	if (link == &raw_link) {
		link->req_pending = false;
		if (status != ZLRS_RADIO_OK) {
			if (link->req_is_tx) {
				atomic_inc(&raw_tx_failed);
			}
			return;
		}
		if (link->req_is_tx) {
			atomic_inc(&raw_tx);
		} else {
			if (rx_len != CONFIG_ZLRS_TEST_RAW_PAYLOAD_BYTES) {
				atomic_inc(&raw_bad);
				return;
			}
			const uint32_t sequence = sys_get_le32(link->rx_buf);
			for (uint16_t i = 4U; i < CONFIG_ZLRS_TEST_RAW_PAYLOAD_BYTES; i++) {
				if (link->rx_buf[i] != (uint8_t)(sequence + i)) {
					atomic_inc(&raw_bad);
					return;
				}
			}
			const uint32_t delta = raw_rx_started ? sequence - raw_last_sequence : 1U;
			if (delta == 0U) {
				atomic_inc(&raw_bad);
				return;
			}
			raw_rx_started = true;
			raw_last_sequence = sequence;
			atomic_add(&raw_expected, delta);
			atomic_inc(&raw_rx);
		}
		return;
	}
#endif
	zlrs_lr2021_diag_t diag;
	zlrs_lr2021_diag(&diag);
	const uint32_t crc = diag.rx_crc_errors - channel_crc_previous;
	const uint32_t frequency = link->req.freq_hz;

	channel_crc_previous = diag.rx_crc_errors;
	if (frequency >= 2402000000U && frequency <= 2480000000U &&
	    frequency % 1000000U == 0U) {
		const uint8_t index = (uint8_t)(frequency / 1000000U - 2402U);

		atomic_add(&channels[index].crc, crc);
		if (link->req_is_tx) {
			atomic_inc(status == ZLRS_RADIO_OK ? &channels[index].tx
							: &channels[index].tx_failed);
		} else {
			atomic_inc(status == ZLRS_RADIO_OK && rx_len > 0U
					   ? &channels[index].rx
					   : &channels[index].empty);
		}
	}
	__real_zlrs_link_complete(link, status, rx_len, rssi, snr, now_ms);
}

static void restore(struct k_work *work)
{
	ARG_UNUSED(work);
	sys_reboot(SYS_REBOOT_COLD);
}

K_WORK_DELAYABLE_DEFINE(restore_work, restore);

static int cmd_radio(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2U && strcmp(argv[1], "channels") == 0) {
		for (uint8_t i = 0U; i < ARRAY_SIZE(channels); i++) {
			shell_print(sh,
				    "CHANNEL frequency_mhz=%u tx=%u tx_failed=%u rx=%u empty=%u crc=%u",
				    2402U + i, (unsigned int)atomic_get(&channels[i].tx),
				    (unsigned int)atomic_get(&channels[i].tx_failed),
				    (unsigned int)atomic_get(&channels[i].rx),
				    (unsigned int)atomic_get(&channels[i].empty),
				    (unsigned int)atomic_get(&channels[i].crc));
		}
		return 0;
	}
	if (argc == 2U && strcmp(argv[1], "restore") == 0) {
		shell_print(sh, "restoring previous confirmed firmware");
		k_work_schedule(&restore_work, K_MSEC(100));
		return 0;
	}
	if (argc == 2U && strcmp(argv[1], "start") == 0) {
		if (!atomic_get(&raw_ready)) {
			return -EINVAL;
		}
		k_sem_give(&raw_start);
		return 0;
	}
	if (argc == 3U && strcmp(argv[1], "rxwin") == 0) {
		char *end;
		const unsigned long ms = strtoul(argv[2], &end, 0);

		if (argv[2][0] == '-' || *end != '\0' || ms > 500UL) {
			return -EINVAL;
		}
		k_sem_reset(&window_done);
		if (!atomic_cas(&window_request, 0, (int)ms + 1)) {
			return -EBUSY;
		}
		if (k_sem_take(&window_done, K_SECONDS(2)) != 0) {
			(void)atomic_cas(&window_request, (int)ms + 1, 0);
			return -ETIMEDOUT;
		}
		return 0;
	}
	if (argc != 1U && !(argc == 2U && strcmp(argv[1], "status") == 0)) {
		return -EINVAL;
	}
	shell_print(sh,
		    "RAW ready=%u tx=%u tx_failed=%u rx=%u expected=%u bad=%u "
		    "frequency_mhz=%u payload_bytes=%u duration_ms=%u",
		    (unsigned int)atomic_get(&raw_ready), (unsigned int)atomic_get(&raw_tx),
		    (unsigned int)atomic_get(&raw_tx_failed), (unsigned int)atomic_get(&raw_rx),
		    (unsigned int)atomic_get(&raw_expected), (unsigned int)atomic_get(&raw_bad),
		    CONFIG_ZLRS_TEST_RAW_FREQ_MHZ, CONFIG_ZLRS_TEST_RAW_PAYLOAD_BYTES,
		    CONFIG_ZLRS_TEST_RAW_MS);
	return 0;
}

SHELL_CMD_REGISTER(test_radio, NULL, "Radio test: status, channels, start, rxwin <ms>, restore",
		   cmd_radio);
