/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>

#include "platform/slot_timer.h"

ZTEST(slot_timer, test_launch_deadline_guard)
{
	zassert_true(zlrs_slot_timer_launch_ready(1000U, 1025U, 25U));
	zassert_true(zlrs_slot_timer_launch_ready(999U, 1025U, 25U));
	zassert_false(zlrs_slot_timer_launch_ready(1001U, 1025U, 25U));
	zassert_false(zlrs_slot_timer_launch_ready(1025U, 1025U, 25U));
	zassert_true(zlrs_slot_timer_launch_ready(UINT32_MAX - 15U, 9U, 25U));
	zassert_false(zlrs_slot_timer_launch_ready(UINT32_MAX - 14U, 9U, 25U));
	zassert_false(zlrs_slot_timer_launch_ready(0U, 1000U, 0U));
}

ZTEST(slot_timer, test_scan_delay)
{
	zassert_equal(zlrs_slot_timer_scan_delay_ms(14580U, 255U, false), 300U);
	zassert_equal(zlrs_slot_timer_scan_delay_ms(14580U, 255U, true), 7451U);
	zassert_equal(zlrs_slot_timer_scan_delay_ms(11092U, 300U, true), 6667U);
	zassert_equal(zlrs_slot_timer_scan_delay_ms(100000U, 5U, true), 1100U);
	zassert_equal(zlrs_slot_timer_scan_delay_ms(1000U, 5U, true), 300U);
	zassert_equal(zlrs_slot_timer_scan_delay_ms(100000U, 0U, true), 300U);
}

ZTEST(slot_timer, test_timer_advances_and_applies_phase_shift)
{
	uint32_t start_us;
	uint32_t frame;

	zassert_ok(zlrs_slot_timer_init());
	const uint32_t before_us = zlrs_slot_timer_now_us();

	zlrs_slot_timer_start(4000U, 0U, NULL);
	zlrs_slot_timer_shift(5000);
	zlrs_slot_timer_wait_frame(&start_us, &frame);
	zassert_equal(frame, 1U);
	zassert_true((int32_t)(start_us - before_us) > 0);
	zassert_equal(zlrs_slot_timer_next_frame_us() - start_us, 4250U);
	zlrs_slot_timer_stop();
}

ZTEST_SUITE(slot_timer, NULL, NULL, NULL, NULL, NULL);

ZTEST(slot_timer, test_band_transition_preserves_frame_and_physical_guard)
{
	uint32_t first_us, next_us, first, next;
	zassert_ok(zlrs_slot_timer_init());
	zlrs_slot_timer_start(4000U, 300U, NULL);
	zlrs_slot_timer_wait_frame(&first_us, &first);
	zlrs_slot_timer_hold_next_frame(12000U);
	zlrs_slot_timer_wait_until(first_us + 3000U, NULL);
	zlrs_slot_timer_reconfigure(20000U, 6000U, 0U);
	zlrs_slot_timer_wait_frame(&next_us, &next);
	zassert_equal(next, first + 1U);
	zassert_equal(next_us - first_us, 16000U);
	zassert_equal(zlrs_slot_timer_next_frame_us() - next_us, 20000U);
	zlrs_slot_timer_stop();
}
