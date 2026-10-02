/* Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test.h"
#include "zlrs/link/slot_clock.h"

void test_slot_clock(void)
{
	zlrs_slot_clock_t clock;

	zlrs_slot_clock_init(&clock, 100U, 6666U);
	CHECK(zlrs_slot_clock_next_origin(&clock) == 100U);
	CHECK(!zlrs_slot_clock_due(&clock, 99U));
	CHECK(zlrs_slot_clock_advance(&clock, 99U) == 0U);
	CHECK(zlrs_slot_clock_advance(&clock, 100U) == 1U);
	CHECK(clock.frame == 1U && clock.slot == 1U && clock.next_us == 1766U);
	CHECK(zlrs_slot_clock_next_origin(&clock) == 6766U);
	CHECK(zlrs_slot_clock_offset(6666U, 2U) == 3333U);
	CHECK(zlrs_slot_clock_offset(6666U, 3U) == 4999U);
	CHECK(zlrs_slot_clock_offset(6666U, 4U) == 6666U);
	/* Delayed IRQ catches up, preserving the original absolute origin. */
	CHECK(zlrs_slot_clock_advance(&clock, 7200U) == 4U);
	CHECK(clock.frame == 2U && clock.published_origin_us == 6766U);
	CHECK(clock.next_us == 8432U);
	/* A phase correction applies once, at the next frame boundary, and is bounded. */
	zlrs_slot_clock_shift(&clock, 10000);
	CHECK(clock.shift_us == 416);
	CHECK(zlrs_slot_clock_next_origin(&clock) == 13848U);
	CHECK(zlrs_slot_clock_advance(&clock, 11765U) == 3U);
	CHECK(clock.next_us == 13848U && clock.shift_us == 0);
	CHECK(zlrs_slot_clock_next_origin(&clock) == 13848U);
	CHECK(zlrs_slot_clock_advance(&clock, 13848U) == 1U);
	CHECK(clock.published_origin_us == 13848U);
	zlrs_slot_clock_shift(&clock, -10000);
	CHECK(clock.shift_us == -416);
	CHECK(zlrs_slot_clock_next_origin(&clock) == 20098U);
	CHECK(zlrs_slot_clock_advance(&clock, 18847U) == 3U);
	CHECK(clock.next_us == 20098U);
	CHECK(zlrs_slot_clock_next_origin(&clock) == 20098U);
	/* Counter wrap does not reset the schedule or corrupt lateness comparisons. */
	zlrs_slot_clock_init(&clock, UINT32_MAX - 999U, 4000U);
	CHECK(zlrs_slot_clock_advance(&clock, UINT32_MAX - 1000U) == 0U);
	CHECK(zlrs_slot_clock_advance(&clock, UINT32_MAX - 999U) == 1U);
	CHECK(clock.next_us == 0U);
	/* RF origin follows the early preparation wake, including across timer wrap. */
	CHECK(clock.published_origin_us + 2000U == 1000U);
	CHECK(zlrs_slot_clock_next_origin(&clock) == 3000U);
	CHECK(zlrs_slot_clock_advance(&clock, 0U) == 1U);
	CHECK(clock.next_us == 1000U);
	CHECK(zlrs_slot_clock_advance(&clock, 3000U) == 3U);
	CHECK(clock.frame == 2U && clock.published_origin_us == 3000U);
	zlrs_slot_clock_init(&clock, 0U, 0U);
	CHECK(clock.period_us == 4U);
	/* A legal idle SYNC gap must not start phase scanning or forget acquisition.
	 * At data24's 5 ms reserve, SYNC arrives every 3.718 s, far beyond 300 ms. */
	CHECK(zlrs_slot_clock_scan_delay_ms(14580U, 255U, false) == 300U);
	CHECK(zlrs_slot_clock_scan_delay_ms(14580U, 255U, true) == 7451U);
	CHECK(zlrs_slot_clock_scan_delay_ms(100000U, 5U, true) == 1100U);
	CHECK(zlrs_slot_clock_scan_delay_ms(1000U, 5U, true) == 300U);
	CHECK(zlrs_slot_clock_scan_delay_ms(100000U, 0U, true) == 300U);
	CHECK(zlrs_slot_clock_scan_delay_ms(0U, 255U, true) == 300U);
	CHECK(zlrs_slot_clock_scan_delay_ms(1000000U, 255U, true) == 511000U);
}
