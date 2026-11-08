/* SPDX-License-Identifier: Apache-2.0 */
#include "../../support/peer.h"
#include <zephyr/ztest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <native_rtc.h>
#include "link/link.h"

#define SAMPLES 7
static volatile uint32_t checksum;

static void require(bool ok, const char *what)
{
	zassert_true(ok, "%s", what);
}

static uint64_t clock_ns(void)
{
	uint32_t nsec;
	uint64_t sec;
	native_rtc_gettime(RTC_CLOCK_PSEUDOHOSTREALTIME, &nsec, &sec);
	return sec * 1000000000ULL + nsec;
}

enum phase {
	HEADER,
	TAG,
	PLAN,
	FINALIZE,
	COMPLETE
};

static const char *const names[] = {"header", "tag", "plan", "finalize", "complete"};

static double measure(enum phase phase, uint16_t payload, unsigned long iterations,
		      zlrs_profile_t profile)
{
	const zlrs_link_cfg_t cfg = {.role = ZLRS_ROLE_LEADER,
				     .region = ZLRS_REGION_ISM2400,
				     .profile = profile,
				     .mod = {.rate_bps = 2600000},
				     .data_mtu = 499,
				     .arq = 1};
	static zlrs_link_t link;
	zlrs_link_init(&link, &cfg, NULL, NULL, NULL);
	/* Isolate steady-state costs; handshake correctness is covered by host link tests. */
	link.configured = link.paired = link.session_valid = true;
	link.state = ZLRS_STATE_CONNECTED;
	uint8_t bytes[499];
	memset(bytes, 0x42, sizeof(bytes));
	require(zlrs_test_write(&link, bytes, payload) == payload, "queue payload");
	const uint8_t tag_len = zlrs_profile_tag_len(profile);
	const uint16_t len = zlrs_frame_len(ZLRS_DATA_PREFIX_LEN + payload, tag_len);
	zlrs_frame_t frame = {.type = ZLRS_PKT_DATA_L2F, .seq = 1};
	frame.body.data.bytes = bytes;
	uint8_t encoded[ZLRS_FRAME_MAX_LEN], tag[ZLRS_TAG_LEN];
	require(zlrs_test_encode(&frame, encoded, sizeof(encoded), len, tag_len) == len,
		"encode packet");
	if (phase == PLAN || phase == FINALIZE) {
		link.arq_ack_pending = payload == 0;
		require(zlrs_link_advance_slot(&link, ZLRS_SLOT_DATA_A, 1000), "advance plan");
	}
	if (phase == COMPLETE) {
		link.arq_ack_pending = payload == 0;
		const zlrs_radio_req_t *req = zlrs_test_slot(&link, ZLRS_SLOT_DATA_A, 1000);
		require(req && req->is_tx && req->size == len, "prepare completion");
	}
	zlrs_link_plan_t plan;
	zlrs_link_plan(&link, &plan);
	const zlrs_data_header_t header = {.lq_percent = ZLRS_LQ_UNAVAILABLE};
	const uint64_t start = clock_ns();
	for (unsigned long i = 0; i < iterations; i++) {
		switch (phase) {
		case HEADER:
			checksum =
				zlrs_frame_encode_data(encoded, sizeof(encoded), ZLRS_PKT_DATA_L2F,
						       1U, &header, payload, tag_len);
			break;
		case TAG:
			zlrs_tag(link.session_key_out, ZLRS_KEY_LEN, encoded, len - ZLRS_TAG_LEN,
				 tag);
			checksum = tag[0];
			break;
		case PLAN: {
			zlrs_link_plan_t plan;
			zlrs_link_plan(&link, &plan);
			checksum = plan.op == ZLRS_OP_TX ? plan.radio.size : 0;
			break;
		}
		case FINALIZE: {
			/* Replay a selected TX finalization without completion or an ACK. */
			link.req_pending = false;
			link.arq_used = 0;
			link.arq_ack_pending = payload == 0;
			const zlrs_radio_req_t *req = zlrs_link_finalize(&link, &plan);
			checksum = req != NULL && req->is_tx ? req->size : 0;
			break;
		}
		case COMPLETE:
			/* Replay the same completed transaction to isolate bookkeeping, without encoding. */
			link.req_pending = true;
			zlrs_link_complete(&link, ZLRS_RADIO_OK, 0, 0, 0, 1000);
			break;
		}
	}
	const double ns = (double)(clock_ns() - start) / iterations;
	if (phase == COMPLETE) {
		require(link.stats.tx_frames == iterations, "completion count");
		require(link.stats.tx_data_bytes == iterations * payload,
			"completion payload count");
		require(payload ? link.stats.tx_data_frames == iterations
				: link.stats.tx_ack_only_frames == iterations,
			"completion classification");
	} else if (phase == PLAN) {
		require(checksum == len, "planned size");
		require(link.stats.tx_frame_builds == 0 && link.tx_seq == 1, "plan is read-only");
	} else if (phase == FINALIZE) {
		require(checksum == len, "built packet size");
		const uint8_t *packet = link.req.buf;
		zlrs_frame_t decoded;
		require(zlrs_frame_decode(&decoded, packet, len, tag_len), "decode built packet");
		frame.seq = decoded.seq;
		frame.body.data.header.lq_percent = ZLRS_LQ_UNAVAILABLE;
		require(zlrs_test_encode(&frame, encoded, sizeof(encoded), len, tag_len) == len,
			"encode reference packet");
		if (tag_len != 0) {
			zlrs_tag(link.session_key_out, ZLRS_KEY_LEN, encoded, len - tag_len,
				 encoded + len - tag_len);
		}
		require(memcmp(packet, encoded, len) == 0, "built packet matches reference");
	}
	return ns;
}

static int compare(const void *a, const void *b)
{
	const double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

ZTEST(link_cost, test_measurements)
{
	const unsigned long iterations = CONFIG_ZLRS_BENCH_ITERATIONS;
	const uint16_t payloads[] = {0, 32, 128, 499};
	printf("{\"compiler\":\"%s\",\"iterations\":%lu,\"samples_per_case\":%d,\"results\":[\n",
	       __VERSION__, iterations, SAMPLES);
	bool first = true;
	for (zlrs_profile_t profile = ZLRS_PROFILE_P0; profile <= ZLRS_PROFILE_P1; profile++) {
		for (unsigned int i = 0; i < sizeof(payloads) / sizeof(payloads[0]); i++) {
			for (enum phase phase = HEADER; phase <= COMPLETE; phase++) {
				if ((phase == PLAN && payloads[i] == 0) ||
				    (phase == TAG && profile == ZLRS_PROFILE_P0)) {
					continue;
				}
				/* Warm instruction/data caches. */
				(void)measure(phase, payloads[i], 100, profile);
				double ns[SAMPLES];
				for (unsigned int sample = 0; sample < SAMPLES; sample++) {
					ns[sample] =
						measure(phase, payloads[i], iterations, profile);
				}
				qsort(ns, SAMPLES, sizeof(ns[0]), compare);
				printf("%s{\"profile\":%u,\"phase\":\"%s\",\"payload_bytes\":%u,"
				       "\"median_ns\":%.3f,"
				       "\"min_ns\":%.3f,\"max_ns\":%.3f}",
				       first ? "" : ",\n", profile, names[phase], payloads[i],
				       ns[SAMPLES / 2], ns[0], ns[SAMPLES - 1]);
				first = false;
			}
		}
	}
	puts("\n]}");
}

ZTEST_SUITE(link_cost, NULL, NULL, NULL, NULL, NULL);
