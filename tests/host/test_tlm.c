/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test.h"
#include <string.h>

#include "zlrs/proto/tlm.h"

/* Telemetry payload: fixed length, fixed field order, wrong length is rejected */
void test_tlm(void)
{
	zlrs_tlm_t in;
	zlrs_tlm_t out;
	uint8_t buf[ZLRS_TLM_LEN];
	uint8_t short_buf[ZLRS_TLM_LEN - 1U] = {0};

	printf("tlm payload\n");

	in.lq_percent = 96U;
	in.rssi_neg_dbm = 71U;
	in.snr_db = -4;
	in.rf_mode = 1U;
	in.tx_power = 3U;
	in.state = 4U;
	in.flags = ZLRS_TLM_FLAG_PAIRED | ZLRS_TLM_FLAG_SESSION;

	zlrs_tlm_encode(buf, &in);
	CHECK(zlrs_test_hex_eq(buf, sizeof(buf), "6047fc0103040300"));

	CHECK(zlrs_tlm_decode(buf, sizeof(buf), &out));
	CHECK(memcmp(&in, &out, sizeof(in)) == 0);

	/* A short payload is rejected and the outputs stay untouched */
	memset(&out, 0, sizeof(out));
	CHECK(!zlrs_tlm_decode(short_buf, sizeof(short_buf), &out));
	CHECK(out.lq_percent == 0U);
	CHECK(!zlrs_tlm_decode(buf, 0U, &out));
}
