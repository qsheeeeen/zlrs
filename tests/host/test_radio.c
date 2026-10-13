/* SPDX-License-Identifier: Apache-2.0 */
#include "test.h"

#include "../../app/src/platform/lr2021.h"
#include "../../app/src/platform/slot_timer.h"
#include <string.h>

static void test_periodic_sync_acquisition(void)
{
	const struct {
		uint32_t cadence_us;
		uint16_t sync_frames;
		uint32_t window_us;
	} bands[] = {{11092U, 450U, 4000U}, {48134U, 103U, 20000U},
		     {22184U, 225U, 9000U}};

	for (size_t band = 0U; band < sizeof(bands) / sizeof(bands[0]); band++) {
		const uint32_t cadence = bands[band].cadence_us;
		const uint32_t receiver_period = cadence +
			zlrs_slot_timer_scan_step_us(cadence, bands[band].sync_frames);
		const uint32_t sync_us = cadence * bands[band].sync_frames;

		/* Try every initial clock offset: a quiet connected TX only sends periodic
		 * SYNC. The searching RX must hear one within three such opportunities. */
		for (uint32_t phase = 0U; phase < cadence; phase++) {
			bool heard = false;

			for (uint32_t attempt = 1U; attempt <= 3U; attempt++) {
				const uint32_t offset = (attempt * sync_us + phase) % receiver_period;

				heard |= offset < bands[band].window_us;
			}
			if (!heard) {
				printf("SYNC acquisition failed: cadence=%u phase=%u\n", cadence,
				       phase);
				CHECK(heard);
				break;
			}
		}
	}
}

void test_radio(void)
{
	/* ADC unit values are signed 13-bit Q8.5, including below-zero temperatures. */
	CHECK(zlrs_lr2021_temperature_centi_c(0U) == 0);
	CHECK(zlrs_lr2021_temperature_centi_c(1U) == 3);
	CHECK(zlrs_lr2021_temperature_centi_c(800U) == 2500);
	CHECK(zlrs_lr2021_temperature_centi_c(0x1FFFU) == -3);
	CHECK(zlrs_lr2021_temperature_centi_c(0x1FE0U) == -100);
	CHECK(zlrs_lr2021_temperature_centi_c(0x0FFFU) == 12796);
	CHECK(zlrs_lr2021_temperature_centi_c(0x1000U) == -12800);

	const uint16_t lengths[] = {0U, 1U, 127U, 128U, 129U, 255U, 256U, 511U};

	printf("radio: FIFO prefix and final length bounds\n");
	for (size_t i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		for (uint16_t copied = 0U; copied <= 512U; copied++) {
			uint16_t remaining = 0xDEADU;
			const bool valid = copied <= lengths[i];

			CHECK(zlrs_lr2021_fifo_remaining(511U, copied, lengths[i], &remaining) ==
			      valid);
			CHECK(remaining == (valid ? (uint32_t)lengths[i] - copied : 0xDEADU));
		}
	}
	/* Check the addition before narrowing it: wrapped totals must not pass. */
	uint16_t remaining = 0xDEADU;

	CHECK(!zlrs_lr2021_fifo_remaining(511U, 500U, 512U, &remaining));
	CHECK(!zlrs_lr2021_fifo_remaining(511U, 500U, 66036U, &remaining));
	CHECK(!zlrs_lr2021_fifo_remaining(511U, 0U, UINT32_MAX, &remaining));
	CHECK(!zlrs_lr2021_fifo_remaining(511U, UINT16_MAX, 511U, &remaining));
	CHECK(remaining == 0xDEADU);
	CHECK(zlrs_lr2021_fifo_remaining(UINT16_MAX, 0U, UINT16_MAX, &remaining));
	CHECK(remaining == UINT16_MAX);
	CHECK(zlrs_lr2021_fifo_remaining(0U, 0U, 0U, &remaining));
	CHECK(remaining == 0U);

	/* Fallback TX loads a prefix; appending the tail must reconstruct every byte. */
	uint8_t packet[511];
	uint8_t fifo[511];

	for (size_t i = 0U; i < sizeof(packet); i++) {
		packet[i] = (uint8_t)(i ^ (i >> 8));
	}
	for (uint16_t size = 0U; size <= sizeof(packet); size++) {
		const uint16_t prefix = zlrs_lr2021_tx_fifo_prefix(size);

		CHECK(prefix == (size > 128U ? 128U : size));
		CHECK(zlrs_lr2021_fifo_remaining(sizeof(packet), prefix, size, &remaining));
		CHECK((uint32_t)prefix + remaining == size);
		memcpy(fifo, packet, prefix);
		memcpy(fifo + prefix, packet + prefix, remaining);
		CHECK(memcmp(fifo, packet, size) == 0);
	}
	test_periodic_sync_acquisition();
}
