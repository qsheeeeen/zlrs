/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include "ui.h"

static uint32_t holds(void)
{
	struct zlrs_input input;
	zassert_ok(zbus_chan_read(&zlrs_input_chan, &input, K_MSEC(20)));
	return input.button_holds;
}

static void pressed(bool value)
{
	zassert_ok(gpio_emul_input_set(DEVICE_DT_GET(DT_NODELABEL(gpio_test)), 0, value));
}

ZTEST(ui_input, test_hold_and_publication_retry)
{
	k_msleep(40);
	pressed(true);
	k_msleep(200);
	pressed(false);
	k_msleep(60);
	zassert_equal(holds(), 0);
	/* Force an actual write-lock failure while the gesture completes. */
	zassert_ok(zbus_chan_claim(&zlrs_input_chan, K_NO_WAIT));
	pressed(true);
	k_msleep(1300);
	zassert_equal(
		((const struct zlrs_input *)zbus_chan_const_msg(&zlrs_input_chan))->button_holds,
		0);
	zassert_ok(zbus_chan_finish(&zlrs_input_chan));
	k_msleep(60);
	zassert_equal(holds(), 1);
	k_msleep(1200);
	zassert_equal(holds(), 1);
	pressed(false);
	k_msleep(60);
	pressed(true);
	k_msleep(1100);
	pressed(false);
	k_msleep(60);
	zassert_equal(holds(), 2);
	/* A hold begun before the cutoff but completed afterwards must be ignored. */
	int64_t remaining = ZLRS_BUTTON_PAIR_WINDOW_MS - 500 - k_uptime_get();
	zassert_true(remaining > 0, "button tests must run in the boot window");
	k_msleep(remaining);
	pressed(true);
	k_msleep(1200);
	zassert_false(zlrs_button_pairing_allowed());
	zassert_equal(holds(), 2);
	pressed(false);
	k_msleep(60);
	pressed(true);
	k_msleep(1200);
	pressed(false);
	k_msleep(60);
	zassert_equal(holds(), 2, "a new hold after the cutoff must stay disabled");
}

ZTEST_SUITE(ui_input, NULL, NULL, NULL, NULL, NULL);

/* Exercise boot-time input before the LED suite advances virtual uptime. */
void test_main(void)
{
	ztest_run_test_suite(ui_input, false, 1, 1, NULL);
	ztest_run_test_suite(led_patterns, false, 1, 1, NULL);
}
