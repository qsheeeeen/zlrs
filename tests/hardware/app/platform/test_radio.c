/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "fixtures.h"
#include "platform/radio.h"
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
static atomic_t raw_rx;
static atomic_t raw_bad;

void __real_zlrs_link_init(zlrs_link_t *link, const zlrs_link_cfg_t *cfg,
			   uint32_t (*random)(void *ctx), void *random_ctx,
			   const zlrs_link_events_t *events);
int __real_zlrs_radio_init(void);
void __real_zlrs_link_complete(zlrs_link_t *link, int status, uint16_t rx_len, int16_t rssi,
			       int8_t snr, uint32_t now_ms);

void __wrap_zlrs_link_init(zlrs_link_t *link, const zlrs_link_cfg_t *cfg,
			   uint32_t (*random)(void *ctx), void *random_ctx,
			   const zlrs_link_events_t *events)
{
	__real_zlrs_link_init(link, cfg, random, random_ctx, events);
	data_link = link;
	default_rx_ms = cfg->rx_timeout_ms;
	default_acq_ms = cfg->acq_timeout_ms;
}

void zlrs_test_radio_poll(zlrs_link_t *link)
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

static void raw_run(void)
{
	const bool tx = data_link->cfg.role == ZLRS_ROLE_TX;
	zlrs_radio_req_t req = {
		.freq_hz = 2441000000U,
		.mod = data_link->cfg.mod,
		.sync_slot = ZLRS_SYNC_SLOT_PAIRING,
		.sync_word = {'Z', 'L', 'R', 'S'},
		.is_tx = tx,
		.tx_power_dbm = data_link->cfg.tx_power_dbm,
		.size = tx ? 32U : 0U,
		.max_rx_size = tx ? 0U : ZLRS_FRAME_MAX_LEN,
		.timeout_ms = tx ? 0U : 100U,
		.buf = tx ? raw_link.tx_buf : raw_link.rx_buf,
	};
	const uint32_t started = zlrs_slot_timer_now_ms();
	uint32_t sequence = 0U;

	while (zlrs_slot_timer_now_ms() - started < CONFIG_ZLRS_TEST_RAW_MS) {
		for (uint8_t i = 0U; i < 32U; i++) {
			raw_link.tx_buf[i] = (uint8_t)(sequence + i);
		}
		raw_link.req_is_tx = tx;
		raw_link.req_pending = true;
		const uint32_t deadline = zlrs_slot_timer_now_us() + 120000U;

		zlrs_radio_slot(zlrs_slot_timer_now_us(), deadline);
		if (zlrs_radio_prepare(&raw_link, &req) == 0 && zlrs_radio_launch() == 0) {
			while (raw_link.req_pending &&
			       (int32_t)(deadline - zlrs_slot_timer_now_us()) > 0) {
				(void)zlrs_radio_drain();
				const int32_t remaining = deadline - zlrs_slot_timer_now_us();

				if (raw_link.req_pending && remaining > 0) {
					(void)zlrs_radio_wait_us((uint32_t)remaining);
				}
			}
		}
		if (raw_link.req_pending) {
			zlrs_radio_abort();
			raw_link.req_pending = false;
		}
		sequence++;
		zlrs_watchdog_feed();
	}
	/* Test startup ends before the production timer/superframe starts. */
	zlrs_radio_abort();
}

int __wrap_zlrs_radio_init(void)
{
	const int rc = __real_zlrs_radio_init();

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

void __wrap_zlrs_link_complete(zlrs_link_t *link, int status, uint16_t rx_len, int16_t rssi,
			       int8_t snr, uint32_t now_ms)
{
	if (link == &raw_link) {
		link->req_pending = false;
		if (status != ZLRS_RADIO_OK) {
			return;
		}
		if (link->req_is_tx) {
			atomic_inc(&raw_tx);
		} else {
			atomic_inc(&raw_rx);
			if (rx_len != 32U) {
				atomic_inc(&raw_bad);
			} else {
				for (uint8_t i = 1U; i < 32U; i++) {
					if (link->rx_buf[i] != (uint8_t)(link->rx_buf[0] + i)) {
						atomic_inc(&raw_bad);
					}
				}
			}
		}
		return;
	}
	__real_zlrs_link_complete(link, status, rx_len, rssi, snr, now_ms);
}
#endif

static void restore(struct k_work *work)
{
	ARG_UNUSED(work);
	sys_reboot(SYS_REBOOT_COLD);
}

K_WORK_DELAYABLE_DEFINE(restore_work, restore);

static int cmd_radio(const struct shell *sh, size_t argc, char **argv)
{
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
	shell_print(sh, "RAW ready=%u tx=%u rx=%u bad=%u duration_ms=%u",
		    (unsigned int)atomic_get(&raw_ready), (unsigned int)atomic_get(&raw_tx),
		    (unsigned int)atomic_get(&raw_rx), (unsigned int)atomic_get(&raw_bad),
		    CONFIG_ZLRS_TEST_RAW_MS);
	return 0;
}

SHELL_CMD_REGISTER(test_radio, NULL, "Radio test: status, start, rxwin <ms>, restore", cmd_radio);
