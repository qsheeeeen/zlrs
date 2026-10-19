/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Malformed input: feed two stateful parsers (air frame and management frame) random bytes and
 * near-legal variants, and assert they only reject, never overrun or run away. Seeds are fixed.
 *
 * On its own it checks the logic invariants; the host tests carry ASan + UBSan by default
 * (`ctest --test-dir build/host`), so overruns and UB surface immediately. For more rounds, set
 * `-DZLRS_HOST_FUZZ_ITERS=2000000` when configuring the CMake project.
 */

#include "test.h"

#include <string.h>

#include "zlrs/mgmt/mgmt.h"
#include "zlrs/link/link.h"
#include "zlrs/proto/crypto.h"
#include "zlrs/proto/frame.h"

#ifndef FUZZ_ITERS
#define FUZZ_ITERS 20000U /* CMake supplies the configurable value. */
#endif
#define FUZZ_BUF_LEN 128U

typedef struct {
	uint32_t state;
} rng_t;

static uint32_t rng_next(rng_t *rng)
{
	uint32_t x = rng->state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng->state = (x != 0U) ? x : 0x12345678U;

	return rng->state;
}

static void rng_fill(rng_t *rng, uint8_t *out, uint16_t len)
{
	for (uint16_t i = 0; i < len; i++) {
		out[i] = (uint8_t)rng_next(rng);
	}
}

/* ---- air frames: decoded bytes belong to the frame, with length from input ---- */

static void fuzz_frame_decode(rng_t *rng)
{
	uint8_t buf[FUZZ_BUF_LEN];
	uint8_t pack[FUZZ_BUF_LEN];
	uint32_t violations = 0U;
	uint32_t decoded = 0U;

	for (uint32_t i = 0; i < FUZZ_ITERS; i++) {
		const uint16_t len = (uint16_t)(rng_next(rng) % (FUZZ_BUF_LEN + 1U));
		const uint8_t tag_len = ((rng_next(rng) & 1U) != 0U) ? ZLRS_TAG_LEN : 0U;
		zlrs_frame_t frame;

		rng_fill(rng, buf, len);
		if (len >= ZLRS_FRAME_HDR_LEN && (rng_next(rng) % 4U) == 0U) {
			/* Use a legal type so the parser goes further */
			buf[0] = (uint8_t)(rng_next(rng) % 11U);
		}

		if (!zlrs_frame_decode(&frame, buf, len, tag_len)) {
			continue;
		}
		decoded++;
		const zlrs_data_frame_t *const data = &frame.body.data;

		if ((frame.type == ZLRS_PKT_DATA_L2F || frame.type == ZLRS_PKT_DATA_F2L) &&
		    memcmp(data->bytes, buf + ZLRS_FRAME_HDR_LEN + ZLRS_DATA_PREFIX_LEN,
			   len - ZLRS_FRAME_HDR_LEN - ZLRS_DATA_PREFIX_LEN - tag_len) != 0) {
			violations++;
		}
		if (memcmp(frame.tag, buf + len - tag_len, tag_len) != 0) {
			violations++;
		}

		/* Decode and encode again: byte-identical, tag included */
		if (zlrs_frame_encode(&frame, pack, sizeof(pack), len, tag_len) != len) {
			violations++;
		} else if (memcmp(pack, buf, len) != 0) {
			violations++;
		}
	}

	CHECK(violations == 0U);
	CHECK(decoded > 0U); /* do not let the case spin without doing work */
}

/* ---- link entry: random bytes plus near-legal frames (tag included) ---- */

static bool fx_on_data(void *ctx, const uint8_t *data, uint16_t len)
{
	(void)ctx;
	(void)data;
	(void)len;
	return true;
}

static void fx_on_state(void *ctx, zlrs_link_state_t state)
{
	(void)ctx;
	(void)state;
}

static bool fx_random(void *ctx, uint32_t *out)
{
	(void)ctx;

	*out = 0x9E3779B9U;
	return true;
}

static void fx_link_init(zlrs_link_t *link, uint8_t arq)
{
	static const uint8_t root_key[ZLRS_KEY_LEN] = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
		0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
		0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
	};
	static const uint8_t dev_id[ZLRS_DEV_ID_LEN] = {0x11, 0x22, 0x33, 0x44};
	static const uint8_t peer_id[ZLRS_DEV_ID_LEN] = {0xaa, 0xbb, 0xcc, 0xdd};
	static const uint8_t salt[ZLRS_PAIR_SALT_LEN] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
	const zlrs_link_cfg_t cfg = {
		.role = ZLRS_ROLE_LEADER,
		.domain = ZLRS_DOMAIN_ISM2400,
		.profile = ZLRS_PROFILE_P1,
		.mod = {.rate_bps = 2600000U},
		.tx_power_dbm = 10,
		.rx_timeout_ms = 2U,
		.sync_period_ms = 2000U,
		.peer_timeout_ms = 0U,
		.pair_period_ms = 100U,
		.pair_window_ms = 30000U,
		.data_mtu = 128U,
		.arq = arq,
		.hopping = 1U,

	};
	const zlrs_link_events_t ev = {
		.on_data = fx_on_data,
		.on_state = fx_on_state,
		.ctx = NULL,
	};

	zlrs_link_init(link, &cfg, fx_random, NULL, &ev);
	zlrs_link_set_dev_id(link, dev_id);
	zlrs_link_set_root_key(link, root_key, 0U);
	zlrs_link_set_peer(link, peer_id, salt, 0U);
}

/* Build an air frame with a legal shape and tag; the caller then corrupts it at random */
static uint16_t fx_make_frame(rng_t *rng, uint8_t *out, uint16_t cap, const uint8_t *key)
{
	/* Construct bytes independently of the typed encoder. */
	const uint8_t type = (uint8_t)(rng_next(rng) % 11U);
	const uint16_t fixed[] = {0, 0, 9, 8, 16, 16, 8, 9};
	const uint16_t payload_len = type >= ZLRS_PKT_SYNC && type <= ZLRS_PKT_SESS_CFM
					     ? fixed[type]
					     : 3U + rng_next(rng) % 32U;
	const uint16_t mac_len = ZLRS_FRAME_HDR_LEN + payload_len;
	const uint16_t total = mac_len + ZLRS_TAG_LEN;
	if (total > cap) {
		return 0U;
	}
	rng_fill(rng, out, mac_len);
	out[0] = type;
	zlrs_tag(key, ZLRS_KEY_LEN, out, mac_len, out + mac_len);
	return total;
}

static void fuzz_link_rx(rng_t *rng)
{
	uint8_t buf[FUZZ_BUF_LEN];
	zlrs_link_t best_effort;
	zlrs_link_t arq;
	uint32_t now_ms = 0U;
	uint32_t violations = 0U;

	fx_link_init(&best_effort, 0U);
	fx_link_init(&arq, 1U);

	for (uint32_t i = 0; i < FUZZ_ITERS; i++) {
		zlrs_link_t *link = ((rng_next(rng) & 1U) != 0U) ? &best_effort : &arq;
		uint16_t len = fx_make_frame(rng, buf, sizeof(buf), link->link_auth_in);

		if (len == 0U || (rng_next(rng) % 3U) == 0U) {
			/* pure random bytes */
			len = (uint16_t)(rng_next(rng) % 70U);
			rng_fill(rng, buf, len);
		} else {
			/* Flip some bits of a legal frame, or truncate it at a random offset */
			uint8_t flips = (uint8_t)(1U + (rng_next(rng) % 4U));

			for (uint8_t f = 0U; f < flips; f++) {
				buf[rng_next(rng) % len] ^= (uint8_t)(1U << (rng_next(rng) % 8U));
			}
			if ((rng_next(rng) % 4U) == 0U) {
				len = (uint16_t)(rng_next(rng) % (len + 1U));
			}
		}

		now_ms += 7U;
		zlrs_link_rx(link, buf, len, -60, 0, now_ms);

		/* The state machine must end in a legal state */
		if (link->state > ZLRS_STATE_CONNECTED) {
			violations++;
		}
	}

	CHECK(violations == 0U);
	/* Something was rejected, so the parser really ran */
	CHECK(best_effort.stats.rx_bad_frame + best_effort.stats.rx_bad_tag +
		      arq.stats.rx_bad_frame + arq.stats.rx_bad_tag >
	      0U);
}

/* ---- management frames: random streams fed byte by byte ---- */

static void fuzz_byte_parsers(rng_t *rng)
{
	zlrs_mgmt_parser_t cfg;
	uint32_t cfg_frames = 0U;
	uint32_t violations = 0U;

	zlrs_mgmt_parser_init(&cfg);

	for (uint32_t i = 0; i < FUZZ_ITERS; i++) {
		if ((rng_next(rng) % 64U) == 0U) {
			zlrs_mgmt_parser_init(&cfg); /* resynchronise now and then */
		}
		if ((rng_next(rng) % 64U) == 0U) {
		}

		switch (zlrs_mgmt_feed(&cfg, (uint8_t)rng_next(rng))) {
		case 1: {
			cfg_frames++;
			if (cfg.payload_len > ZLRS_MGMT_PAYLOAD_MAX_LEN ||
			    ZLRS_MGMT_PAYLOAD(&cfg) + cfg.payload_len > cfg.buf + sizeof(cfg.buf)) {
				violations++;
			}
			break;
		}
		case 0:
		case -1:
			break;
		default:
			violations++;
			break;
		}
	}

	CHECK(violations == 0U);
	CHECK(cfg_frames < FUZZ_ITERS); /* a random stream must not produce frame after frame */
}

void test_fuzz(void)
{
	rng_t rng = {.state = 0x2545F491U};

	printf("fuzz: frame decode\n");
	fuzz_frame_decode(&rng);

	printf("fuzz: link rx\n");
	fuzz_link_rx(&rng);

	printf("fuzz: management parser\n");
	fuzz_byte_parsers(&rng);
}
