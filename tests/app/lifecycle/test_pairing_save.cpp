/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <initializer_list>

#include "lifecycle.h"

namespace
{
const zlrs_persist_pairing_t pairing = {.peer_id = {1, 2, 3, 4}, .pair_salt = {5, 6, 7, 8}};

ZTEST(PairingSave, test_PersistentFailureBacksOffAndCapsRetries)
{
	zlrs_pairing_save_t state{};
	uint32_t now = 0U;
	for (uint32_t delay : {1000U, 2000U, 4000U, 8000U, 16000U, 30000U, 30000U}) {
		zassert_true(zlrs_pairing_save_due(&state, &pairing, nullptr, now));
		zexpect_equal(zlrs_pairing_save_complete(&state, false, now), delay);
		zexpect_false(zlrs_pairing_save_due(&state, &pairing, nullptr, now + 1U));
		zexpect_false(zlrs_pairing_save_due(&state, &pairing, nullptr, now + delay - 1U));
		now += delay;
	}
}

ZTEST(PairingSave, test_BusyFrameCadenceDoesNotCreateWriteStorm)
{
	zlrs_pairing_save_t state{};
	unsigned int writes = 0U;
	for (uint32_t now = 0U; now < 30000U; now += 2U) {
		if (zlrs_pairing_save_due(&state, &pairing, nullptr, now)) {
			writes++;
			(void)zlrs_pairing_save_complete(&state, false, now);
		}
	}
	zexpect_equal(writes, 5U);
}

ZTEST(PairingSave, test_EraseRetiresTheDeferredCandidate)
{
	zlrs_pairing_save_t state{};
	zassert_true(zlrs_pairing_save_due(&state, &pairing, nullptr, 0U));
	(void)zlrs_pairing_save_complete(&state, false, 0U);
	zexpect_false(zlrs_pairing_save_due(&state, nullptr, &pairing, 100U));
	zexpect_false(zlrs_pairing_save_due(&state, nullptr, &pairing, 60000U));
	zexpect_equal(zlrs_pairing_save_complete(&state, false, 60000U), 0U);
	zexpect_true(zlrs_pairing_save_due(&state, &pairing, nullptr, 60001U));
}

ZTEST(PairingSave, test_NewPeerOrSaltSupersedesAnOldRetry)
{
	for (bool change_peer : {false, true}) {
		TC_PRINT("change_peer=%u\n", (unsigned int)(change_peer));
		zlrs_pairing_save_t state{};
		zassert_true(zlrs_pairing_save_due(&state, &pairing, nullptr, 0U));
		(void)zlrs_pairing_save_complete(&state, false, 0U);
		auto next = pairing;
		if (change_peer) {
			next.peer_id[0]++;
		} else {
			next.pair_salt[0]++;
		}
		zassert_true(zlrs_pairing_save_due(&state, &next, &pairing, 1U));
		zexpect_equal(zlrs_pairing_save_complete(&state, false, 1U), 1000U);
		zexpect_false(zlrs_pairing_save_due(&state, &next, &pairing, 1000U));
		zexpect_true(zlrs_pairing_save_due(&state, &next, &pairing, 1001U));
	}
}

ZTEST(PairingSave, test_CompletedOrExternallySavedRecordIsNotRewritten)
{
	zlrs_pairing_save_t state{};
	zassert_true(zlrs_pairing_save_due(&state, &pairing, nullptr, 0U));
	zexpect_equal(zlrs_pairing_save_complete(&state, true, 10U), 0U);
	for (uint32_t now : {11U, 1000U, 60000U}) {
		zexpect_false(zlrs_pairing_save_due(&state, &pairing, &pairing, now));
	}
	zassert_true(zlrs_pairing_save_due(&state, &pairing, nullptr, 60001U));
	(void)zlrs_pairing_save_complete(&state, false, 60001U);
	zexpect_false(zlrs_pairing_save_due(&state, &pairing, &pairing, 60002U));
	zexpect_false(zlrs_pairing_save_due(&state, &pairing, &pairing, 100000U));
}

ZTEST(PairingSave, test_RetryDeadlineWrapsWithUptime)
{
	zlrs_pairing_save_t state{};
	const uint32_t start = UINT32_MAX - 500U;
	zassert_true(zlrs_pairing_save_due(&state, &pairing, nullptr, start));
	zexpect_equal(zlrs_pairing_save_complete(&state, false, start), 1000U);
	zexpect_false(zlrs_pairing_save_due(&state, &pairing, nullptr, UINT32_MAX));
	zexpect_false(zlrs_pairing_save_due(&state, &pairing, nullptr, 498U));
	zexpect_true(zlrs_pairing_save_due(&state, &pairing, nullptr, 499U));
}

ZTEST(PairingSave, test_SlowFailedWriteStillGetsAFullQuietInterval)
{
	zlrs_pairing_save_t state{};
	zassert_true(zlrs_pairing_save_due(&state, &pairing, nullptr, 1000U));
	zexpect_equal(zlrs_pairing_save_complete(&state, false, 8000U), 1000U);
	zexpect_false(zlrs_pairing_save_due(&state, &pairing, nullptr, 8999U));
	zexpect_true(zlrs_pairing_save_due(&state, &pairing, nullptr, 9000U));
}
} // namespace

ZTEST_SUITE(PairingSave, NULL, NULL, NULL, NULL, NULL);
