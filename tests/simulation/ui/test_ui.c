/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include "ui.h"
#include "rf.h"
#include "mgmt.h"
#include "serial.h"
ZBUS_CHAN_DEFINE(zlrs_rf_chan, zlrs_rf_status_t, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(zlrs_mode_chan, struct zlrs_mode, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(zlrs_serial_chan, struct zlrs_serial_status, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
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
}
ZTEST_SUITE(ui_input, NULL, NULL, NULL, NULL, NULL);
