/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zlrs/link/link.h"

#define SAMPLES 7
static volatile uint32_t checksum;

static void require(bool ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "benchmark precondition failed: %s\n", what);
		exit(1);
	}
}

static uint64_t clock_ns(void)
{
	struct timespec now;
	require(clock_gettime(CLOCK_MONOTONIC, &now) == 0, "monotonic clock");
	return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

enum phase {
	ENCODE,
	TAG,
	PREPARE,
	COMPLETE
};

static const char *const names[] = {"encode", "tag", "prepare", "complete"};

static double measure(enum phase phase, uint16_t payload, unsigned long iterations)
{
	const zlrs_link_cfg_t cfg = {.role = ZLRS_ROLE_LEADER,
				     .domain = ZLRS_DOMAIN_ISM2400,
				     .profile = ZLRS_PROFILE_P1,
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
	require(zlrs_link_write(&link, bytes, payload) == payload, "queue payload");
	const uint16_t len = zlrs_frame_len(ZLRS_DATA_PREFIX_LEN + payload, ZLRS_TAG_LEN);
	zlrs_frame_t frame = {.type = ZLRS_PKT_DATA_L2F, .seq = 1};
	memcpy(frame.body.data.bytes, bytes, payload);
	uint8_t encoded[ZLRS_FRAME_MAX_LEN], tag[ZLRS_TAG_LEN];
	require(zlrs_frame_encode(&frame, encoded, sizeof(encoded), len, ZLRS_TAG_LEN) == len,
		"encode packet");
	if (phase == COMPLETE) {
		link.arq_ack_pending = payload == 0;
		const zlrs_radio_req_t *req = zlrs_link_slot(&link, ZLRS_SLOT_DATA_A, 1000);
		require(req && req->is_tx && req->size == len, "prepare completion");
	}
	const uint64_t start = clock_ns();
	for (unsigned long i = 0; i < iterations; i++) {
		switch (phase) {
		case ENCODE:
			checksum = zlrs_frame_encode(&frame, encoded, sizeof(encoded), len,
						     ZLRS_TAG_LEN);
			break;
		case TAG:
			zlrs_tag(link.session_key_out, ZLRS_KEY_LEN, encoded, len - ZLRS_TAG_LEN,
				 tag);
			checksum = tag[0];
			break;
		case PREPARE: {
			uint16_t size;
			const uint8_t *packet =
				zlrs_link_prepare_data(&link, ZLRS_SLOT_DATA_A, 1000, &size);
			checksum = packet != NULL ? size : 0;
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
	} else if (phase == PREPARE) {
		require(checksum == len, "prepared packet size");
	}
	return ns;
}

static int compare(const void *a, const void *b)
{
	const double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
	unsigned long iterations = 20000;
	if (argc > 2) {
		fprintf(stderr, "usage: %s [iterations: 100..1000000]\n", argv[0]);
		return 2;
	}
	if (argc == 2) {
		char *end;
		iterations = strtoul(argv[1], &end, 10);
		require(*argv[1] && !*end && iterations >= 100 && iterations <= 1000000,
			"iterations in 100..1000000");
	}
	const uint16_t payloads[] = {0, 32, 128, 499};
	printf("{\"compiler\":\"%s\",\"iterations\":%lu,\"samples_per_case\":%d,\"results\":[\n",
	       __VERSION__, iterations, SAMPLES);
	bool first = true;
	for (unsigned int i = 0; i < sizeof(payloads) / sizeof(payloads[0]); i++) {
		for (enum phase phase = ENCODE; phase <= COMPLETE; phase++) {
			if (phase == PREPARE && payloads[i] == 0) {
				continue;
			}
			(void)measure(phase, payloads[i], 100); /* Warm instruction/data caches. */
			double ns[SAMPLES];
			for (unsigned int sample = 0; sample < SAMPLES; sample++) {
				ns[sample] = measure(phase, payloads[i], iterations);
			}
			qsort(ns, SAMPLES, sizeof(ns[0]), compare);
			printf("%s{\"phase\":\"%s\",\"payload_bytes\":%u,\"median_ns\":%.3f,"
			       "\"min_ns\":%.3f,\"max_ns\":%.3f}",
			       first ? "" : ",\n", names[phase], payloads[i], ns[SAMPLES / 2],
			       ns[0], ns[SAMPLES - 1]);
			first = false;
		}
	}
	puts("\n]}");
	return 0;
}
