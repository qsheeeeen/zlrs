/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include "fixtures.h"
#include "rf.h"

static struct zlrs_test_report report;

void zlrs_test_print(struct zlrs_test_report *out, const char *format, ...)
{
	const size_t available = sizeof(out->text) - out->used;
	va_list args;

	va_start(args, format);
	const int count = vsnprintf(out->text + out->used, available, format, args);
	va_end(args);
	if (count < 0 || (size_t)count + 1U >= available) {
		out->truncated = true;
		return;
	}
	out->used += count;
	out->text[out->used++] = '\n';
	out->text[out->used] = '\0';
}

static int rf_status(void)
{
	zlrs_rf_status_t status;
	const int rc = zbus_chan_read(&zlrs_rf_chan, &status, K_MSEC(10));
	static const char *const states[] = {
		"unconfigured", "unpaired", "pairing", "searching", "connected",
	};

	if (rc != 0) {
		return rc;
	}
	zlrs_test_print(&report, "Role : %s\nState : %s\nModulation : %s\nBand mode : %s\n"
		       "Capture : %s\nTransition : %s",
		       status.role == ZLRS_ROLE_LEADER ? "Leader" : "Follower",
		       status.state < ARRAY_SIZE(states) ? states[status.state] : "unknown",
		       zlrs_mod_phy_name(status.data_phy),
		       status.band_mode == ZLRS_DATA_BAND_AUTO ? "auto" : "fixed",
		       status.band_acquiring ? "searching" : "ready",
		       status.band_planned ? "scheduled" : "none");
	return 0;
}

static int control(struct smp_streamer *ctxt)
{
	uint32_t op = UINT32_MAX, arg = 0U;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("op", zcbor_uint32_decode, &op),
		ZCBOR_MAP_DECODE_KEY_DECODER("arg", zcbor_uint32_decode, &arg),
	};

	if (zcbor_map_decode_bulk(ctxt->reader->zs, fields, ARRAY_SIZE(fields), &decoded) != 0 ||
	    op > TEST_RF_STATUS) {
		return MGMT_ERR_EINVAL;
	}
	report.used = 0U;
	report.text[0] = '\0';
	report.truncated = false;
	int rc;

	if (op >= TEST_DATA_STATUS && op <= TEST_DATA_STOP) {
		rc = zlrs_test_data_control(&report, op, arg);
	} else if (op == TEST_PHY_PIN || op == TEST_PHY_STATUS) {
		rc = zlrs_test_phy_control(&report, op, arg);
	} else if (op == TEST_PHY_REQUEST) {
		rc = arg <= ZLRS_PHY_LORA_SF7 ? zlrs_rf_phy_request(arg) : -EINVAL;
		if (rc == 0) {
			zlrs_test_print(&report, "queued profile %u: %s", arg, zlrs_mod_phy_name(arg));
		}
	} else if (op == TEST_RF_STATUS) {
		rc = rf_status();
	} else {
		rc = zlrs_test_radio_control(&report, op, arg);
	}
	zcbor_state_t *zse = ctxt->writer->zs;
	const bool ok = !report.truncated &&
		zcbor_tstr_put_lit(zse, "result") && zcbor_int32_put(zse, rc) &&
		zcbor_tstr_put_lit(zse, "report") && zcbor_tstr_put_term(zse, report.text,
								 sizeof(report.text));

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static const struct mgmt_handler handlers[] = {{.mh_write = control}};
static struct mgmt_group group = {
	.mg_handlers = handlers,
	.mg_handlers_count = ARRAY_SIZE(handlers),
	.mg_group_id = 65,
};

static void register_control(void)
{
	mgmt_register_group(&group);
}

MCUMGR_HANDLER_DEFINE(zlrs_radio_test, register_control);
