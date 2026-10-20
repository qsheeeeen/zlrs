/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>
#include <stdalign.h>

#include "platform/slot_timer.h"
#include "sha256.h"

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

static volatile bool launch_fired;
static volatile uint32_t launch_irq_us;

static void __ramfunc record_launch(uint32_t irq_us)
{
	launch_irq_us = irq_us;
	launch_fired = true;
}

ZTEST(slot_timer, test_launch_jitter_during_sha_load)
{
	static alignas(uint32_t) uint8_t input[1024];
	uint8_t digest[ZLRS_SHA256_LEN];
	uint32_t max_late_us = 0;
	unsigned int hashes = 0;
	zassert_ok(zlrs_slot_timer_init());
	zlrs_slot_timer_stop();
	for (unsigned int sample = 0; sample < 128; sample++) {
		launch_fired = false;
		const uint32_t target_us = zlrs_slot_timer_now_us() + 250U;
		zassert_true(zlrs_slot_timer_launch_at(target_us, record_launch));
		while (!launch_fired && (int32_t)(zlrs_slot_timer_now_us() - target_us) < 2000) {
			input[0] = (uint8_t)sample;
			zlrs_sha256(input, sizeof(input), NULL, 0, digest);
			hashes++;
		}
		zlrs_slot_timer_cancel_launch();
		zassert_true(launch_fired, "launch alarm did not fire, sample %u", sample);
		const int32_t late_us = (int32_t)(launch_irq_us - target_us);
		zassert_true(late_us >= 0, "launch alarm fired early");
		max_late_us = MAX(max_late_us, (uint32_t)late_us);
	}
	zlrs_slot_timer_stop();
	printk("LAUNCH_JITTER samples=128 sha_bytes=1024 hashes=%u max_late_us=%u budget_us=%u\n",
	       hashes, max_late_us, ZLRS_SLOT_TIMER_LAUNCH_MAX_LATE_US);
	zassert_true(hashes > 0);
	zassert_true(max_late_us <= ZLRS_SLOT_TIMER_LAUNCH_MAX_LATE_US,
		     "loaded alarm latency exceeds the radio acceptance window");
}
