/* SPDX-License-Identifier: Apache-2.0 */
/* Test-only native Input injection; GPIO electrical operation is not simulated. */
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zcbor_decode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <zcbor_encode.h>
#include "rf.h"
#include "ui.h"

static void press(struct k_work *work)
{
	ARG_UNUSED(work);
	input_report_key(DEVICE_DT_GET(DT_NODELABEL(zlrs_buttons)), INPUT_KEY_0, 1, true,
			 K_FOREVER);
}

static void release(struct k_work *work)
{
	ARG_UNUSED(work);
	input_report_key(DEVICE_DT_GET(DT_NODELABEL(zlrs_buttons)), INPUT_KEY_0, 0, true,
			 K_FOREVER);
}

static K_WORK_DELAYABLE_DEFINE(press_work, press);
static K_WORK_DELAYABLE_DEFINE(release_work, release);

static int status(struct smp_streamer *ctxt)
{
	struct zlrs_input input = {0};
	zlrs_rf_event_t rf = {0};
	zbus_chan_read(&zlrs_input_chan, &input, K_MSEC(10));
	zbus_chan_read(&zlrs_rf_chan, &rf, K_MSEC(10));
	zcbor_state_t *zs = ctxt->writer->zs;
	bool ok = zcbor_tstr_put_lit(zs, "result") && zcbor_int32_put(zs, 0) &&
		  zcbor_tstr_put_lit(zs, "uptime_ms") && zcbor_int64_put(zs, k_uptime_get()) &&
		  zcbor_tstr_put_lit(zs, "allowed") &&
		  zcbor_bool_put(zs, zlrs_button_pairing_allowed()) &&
		  zcbor_tstr_put_lit(zs, "holds") && zcbor_uint32_put(zs, input.button_holds) &&
		  zcbor_tstr_put_lit(zs, "state") && zcbor_uint32_put(zs, rf.state);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int schedule(struct smp_streamer *ctxt)
{
	uint32_t at_ms = 0, hold_ms = 1200;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("at_ms", zcbor_uint32_decode, &at_ms),
		ZCBOR_MAP_DECODE_KEY_DECODER("hold_ms", zcbor_uint32_decode, &hold_ms),
	};
	uint64_t now = k_uptime_get();
	if (zcbor_map_decode_bulk(ctxt->reader->zs, fields, ARRAY_SIZE(fields), &decoded) != 0 ||
	    at_ms < now || at_ms - now > 60000 || hold_ms < 1100 || hold_ms > 5000) {
		return MGMT_ERR_EINVAL;
	}
	k_work_reschedule(&press_work, K_MSEC(at_ms - now));
	k_work_reschedule(&release_work, K_MSEC(at_ms - now + hold_ms));
	return status(ctxt);
}
static const struct mgmt_handler handlers[] = {{.mh_read = status, .mh_write = schedule}};
static struct mgmt_group group = {
	.mg_handlers = handlers, .mg_handlers_count = ARRAY_SIZE(handlers), .mg_group_id = 67};

static void register_control(void)
{
	mgmt_register_group(&group);
}

MCUMGR_HANDLER_DEFINE(zlrs_button_test, register_control);
