/* SPDX-License-Identifier: Apache-2.0 */
#include "test.h"

#include "../../app/src/platform/lr2021.h"
#include <string.h>

void test_radio(void)
{
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
		const uint16_t prefix = zlrs_lr2021_tx_fifo_prefix(true, size);

		CHECK(prefix == (size > 128U ? 128U : size));
		CHECK(zlrs_lr2021_tx_fifo_prefix(false, size) == size);
		CHECK(zlrs_lr2021_fifo_remaining(sizeof(packet), prefix, size, &remaining));
		CHECK((uint32_t)prefix + remaining == size);
		memcpy(fifo, packet, prefix);
		memcpy(fifo + prefix, packet + prefix, remaining);
		CHECK(memcmp(fifo, packet, size) == 0);
	}
}
