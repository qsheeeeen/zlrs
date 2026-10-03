/* SPDX-License-Identifier: Apache-2.0 */
#include "test.h"

#include "../../app/src/platform/radio.h"

void test_radio_fifo(void)
{
	const uint16_t lengths[] = {0U, 1U, 127U, 128U, 129U, 255U, 256U, 511U};

	printf("radio: FIFO prefix and final length bounds\n");
	for (size_t i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		for (uint16_t copied = 0U; copied <= 512U; copied++) {
			uint16_t remaining = 0xDEADU;
			const bool valid = copied <= lengths[i];

			CHECK(zlrs_radio_fifo_remaining(511U, copied, lengths[i], &remaining) ==
			      valid);
			CHECK(remaining == (valid ? (uint32_t)lengths[i] - copied : 0xDEADU));
		}
	}
	/* Check the addition before narrowing it: wrapped totals must not pass. */
	uint16_t remaining = 0xDEADU;

	CHECK(!zlrs_radio_fifo_remaining(511U, 500U, 512U, &remaining));
	CHECK(!zlrs_radio_fifo_remaining(511U, 500U, 66036U, &remaining));
	CHECK(!zlrs_radio_fifo_remaining(511U, 0U, UINT32_MAX, &remaining));
	CHECK(!zlrs_radio_fifo_remaining(511U, UINT16_MAX, 511U, &remaining));
	CHECK(remaining == 0xDEADU);
	CHECK(zlrs_radio_fifo_remaining(UINT16_MAX, 0U, UINT16_MAX, &remaining));
	CHECK(remaining == UINT16_MAX);
	CHECK(zlrs_radio_fifo_remaining(0U, 0U, 0U, &remaining));
	CHECK(remaining == 0U);
}
