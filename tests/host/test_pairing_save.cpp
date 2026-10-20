/* SPDX-License-Identifier: Apache-2.0 */
#include <gtest/gtest.h>

#include "pairing_save.h"

namespace
{
const zlrs_persist_pairing_t pairing = {.peer_id = {1, 2, 3, 4}, .pair_salt = {5, 6, 7, 8}};

TEST(PairingSave, PersistentFailureBacksOffAndCapsRetries)
{
	zlrs_pairing_save_t state{};
	uint32_t now = 0U;
	for (uint32_t delay : {1000U, 2000U, 4000U, 8000U, 16000U, 30000U, 30000U}) {
		ASSERT_TRUE(zlrs_pairing_save_due(&state, &pairing, nullptr, now));
		EXPECT_EQ(zlrs_pairing_save_complete(&state, false, now), delay);
		EXPECT_FALSE(zlrs_pairing_save_due(&state, &pairing, nullptr, now + 1U));
		EXPECT_FALSE(zlrs_pairing_save_due(&state, &pairing, nullptr, now + delay - 1U));
		now += delay;
	}
}

TEST(PairingSave, BusyFrameCadenceDoesNotCreateWriteStorm)
{
	zlrs_pairing_save_t state{};
	unsigned int writes = 0U;
	for (uint32_t now = 0U; now < 30000U; now += 2U) {
		if (zlrs_pairing_save_due(&state, &pairing, nullptr, now)) {
			writes++;
			(void)zlrs_pairing_save_complete(&state, false, now);
		}
	}
	EXPECT_EQ(writes, 5U);
}

TEST(PairingSave, EraseRetiresTheDeferredCandidate)
{
	zlrs_pairing_save_t state{};
	ASSERT_TRUE(zlrs_pairing_save_due(&state, &pairing, nullptr, 0U));
	(void)zlrs_pairing_save_complete(&state, false, 0U);
	EXPECT_FALSE(zlrs_pairing_save_due(&state, nullptr, &pairing, 100U));
	EXPECT_FALSE(zlrs_pairing_save_due(&state, nullptr, &pairing, 60000U));
	EXPECT_EQ(zlrs_pairing_save_complete(&state, false, 60000U), 0U);
	EXPECT_TRUE(zlrs_pairing_save_due(&state, &pairing, nullptr, 60001U));
}

TEST(PairingSave, NewPeerOrSaltSupersedesAnOldRetry)
{
	for (bool change_peer : {false, true}) {
		SCOPED_TRACE(change_peer);
		zlrs_pairing_save_t state{};
		ASSERT_TRUE(zlrs_pairing_save_due(&state, &pairing, nullptr, 0U));
		(void)zlrs_pairing_save_complete(&state, false, 0U);
		auto next = pairing;
		if (change_peer) {
			next.peer_id[0]++;
		} else {
			next.pair_salt[0]++;
		}
		ASSERT_TRUE(zlrs_pairing_save_due(&state, &next, &pairing, 1U));
		EXPECT_EQ(zlrs_pairing_save_complete(&state, false, 1U), 1000U);
		EXPECT_FALSE(zlrs_pairing_save_due(&state, &next, &pairing, 1000U));
		EXPECT_TRUE(zlrs_pairing_save_due(&state, &next, &pairing, 1001U));
	}
}

TEST(PairingSave, CompletedOrExternallySavedRecordIsNotRewritten)
{
	zlrs_pairing_save_t state{};
	ASSERT_TRUE(zlrs_pairing_save_due(&state, &pairing, nullptr, 0U));
	EXPECT_EQ(zlrs_pairing_save_complete(&state, true, 10U), 0U);
	for (uint32_t now : {11U, 1000U, 60000U}) {
		EXPECT_FALSE(zlrs_pairing_save_due(&state, &pairing, &pairing, now));
	}
	ASSERT_TRUE(zlrs_pairing_save_due(&state, &pairing, nullptr, 60001U));
	(void)zlrs_pairing_save_complete(&state, false, 60001U);
	EXPECT_FALSE(zlrs_pairing_save_due(&state, &pairing, &pairing, 60002U));
	EXPECT_FALSE(zlrs_pairing_save_due(&state, &pairing, &pairing, 100000U));
}

TEST(PairingSave, RetryDeadlineWrapsWithUptime)
{
	zlrs_pairing_save_t state{};
	const uint32_t start = UINT32_MAX - 500U;
	ASSERT_TRUE(zlrs_pairing_save_due(&state, &pairing, nullptr, start));
	EXPECT_EQ(zlrs_pairing_save_complete(&state, false, start), 1000U);
	EXPECT_FALSE(zlrs_pairing_save_due(&state, &pairing, nullptr, UINT32_MAX));
	EXPECT_FALSE(zlrs_pairing_save_due(&state, &pairing, nullptr, 498U));
	EXPECT_TRUE(zlrs_pairing_save_due(&state, &pairing, nullptr, 499U));
}

TEST(PairingSave, SlowFailedWriteStillGetsAFullQuietInterval)
{
	zlrs_pairing_save_t state{};
	ASSERT_TRUE(zlrs_pairing_save_due(&state, &pairing, nullptr, 1000U));
	EXPECT_EQ(zlrs_pairing_save_complete(&state, false, 8000U), 1000U);
	EXPECT_FALSE(zlrs_pairing_save_due(&state, &pairing, nullptr, 8999U));
	EXPECT_TRUE(zlrs_pairing_save_due(&state, &pairing, nullptr, 9000U));
}
} // namespace
