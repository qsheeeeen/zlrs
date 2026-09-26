/*
 * The button: its own thread, one gesture — hold it past the threshold (doc/11-ux.md).
 *
 * Same shape as ui/led.c: the thread owns the hardware and what it learns goes on the bus as a
 * fact (state/sys_bus.h), so the link state machine still only ever runs on the link thread
 * (doc/07-architecture.md 6). What it reports is a counter, not a one-shot event: a consumer that
 * reads the channel later still sees that a press happened.
 *
 * Why a thread at all: the hold is a timing measurement the main loop should not have to babysit,
 * and a 20 ms sample is the debounce a mechanical button needs anyway. No short press and no
 * unpair gesture: hold again to re-pair, the new record replaces the old.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "platform/caps.h"
#include "state/sys_bus.h"

LOG_MODULE_REGISTER(zlrs_ui_button, CONFIG_APP_LOG_LEVEL);

/* One sample per this: the debounce a mechanical button needs, and finer than the 1 s hold */
#define BUTTON_POLL_MS 20U

/*
 * Preemptive like the LED thread (doc/07-architecture.md 6), and below it: a button sample is a
 * GPIO read, so it can wait for the strip to be written.
 */
#define BUTTON_THREAD_PRIORITY   K_PRIO_PREEMPT(8)
#define BUTTON_THREAD_STACK_SIZE 1024U
#define BUTTON_THREAD_NAME       "zlrs_button"

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(zlrs_button), gpios);

static void button_thread(void *p1, void *p2, void *p3)
{
	struct zlrs_input input = {0};
	bool down = false;
	bool fired = false;
	uint32_t since_ms = 0U;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_thread_name_set(k_current_get(), BUTTON_THREAD_NAME);

	if (!ZLRS_HAS_BUTTON) {
		LOG_WRN("no button on this board: pairing comes from the config tool");
		return;
	}

	if (!gpio_is_ready_dt(&button) || gpio_pin_configure_dt(&button, GPIO_INPUT) != 0) {
		LOG_WRN("button unavailable");
		return;
	}

	/* Start from what the channel holds, so the first hold is reported as a change */
	(void)zbus_chan_read(&zlrs_input_chan, &input, K_NO_WAIT);

	for (;;) {
		const uint32_t now_ms = k_uptime_get_32();
		const int pressed = gpio_pin_get_dt(&button);

		if (pressed > 0 && !down) {
			down = true;
			fired = false;
			since_ms = now_ms;
		} else if (pressed > 0 && down && !fired &&
			   (uint32_t)(now_ms - since_ms) >= CONFIG_ZLRS_APP_PAIR_HOLD_MS) {
			fired = true;
			input.button_holds++;
			LOG_INF("pairing requested (button held %u ms)",
				(unsigned int)(now_ms - since_ms));
			zlrs_sys_publish_input(&input);
		} else if (pressed == 0 && down) {
			down = false;
		}

		k_msleep(BUTTON_POLL_MS);
	}
}

K_THREAD_DEFINE(zlrs_button_tid, BUTTON_THREAD_STACK_SIZE, button_thread, NULL, NULL, NULL,
		BUTTON_THREAD_PRIORITY, 0, 0);
