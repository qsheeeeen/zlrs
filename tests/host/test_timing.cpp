/* SPDX-License-Identifier: Apache-2.0 */
#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <limits>
#include <numeric>

#include "timing.h"

namespace
{
TEST(Timing, EmptyHistogramAndInvalidPercentHaveNoResult)
{
	zlrs_timing_histogram_t histogram{};
	EXPECT_EQ(zlrs_timing_percentile_bound(&histogram, 50U), 0U);
	zlrs_timing_record(&histogram, 37U);
	EXPECT_EQ(zlrs_timing_percentile_bound(&histogram, 0U), 0U);
	EXPECT_EQ(zlrs_timing_percentile_bound(&histogram, 101U), 0U);
	EXPECT_EQ(zlrs_timing_percentile_bound(&histogram, 100U), 37U);
}

TEST(Timing, ZeroAndLargeSamplesKeepExactTotalsAndExtrema)
{
	zlrs_timing_histogram_t histogram{};
	for (uint32_t us : {0U, 1U, 8193U, UINT32_MAX}) {
		zlrs_timing_record(&histogram, us);
	}
	EXPECT_EQ(histogram.samples, 4U);
	EXPECT_EQ(histogram.min_us, 0U);
	EXPECT_EQ(histogram.max_us, UINT32_MAX);
	EXPECT_EQ(histogram.total_us, uint64_t{UINT32_MAX} + 8194U);
	EXPECT_EQ(std::accumulate(std::begin(histogram.buckets), std::end(histogram.buckets),
				  uint64_t{0}),
		  histogram.samples);
	EXPECT_EQ(zlrs_timing_percentile_bound(&histogram, 100U), UINT32_MAX);
}

TEST(Timing, EveryQuantileBoundsTheSortedSampleOracle)
{
	std::array<uint32_t, 1000> samples{};
	zlrs_timing_histogram_t histogram{};
	for (size_t i = 0; i < samples.size(); i++) {
		samples[i] = static_cast<uint32_t>((i * 65537U) % 12001U);
		zlrs_timing_record(&histogram, samples[i]);
	}
	std::sort(samples.begin(), samples.end());
	for (uint8_t percent = 1U; percent <= 100U; percent++) {
		SCOPED_TRACE(static_cast<unsigned int>(percent));
		const uint32_t bound = zlrs_timing_percentile_bound(&histogram, percent);
		const size_t rank = (samples.size() * percent + 99U) / 100U - 1U;
		EXPECT_GE(bound, samples[rank]);
		EXPECT_LE(bound, samples.back());
	}
	EXPECT_EQ(histogram.min_us, samples.front());
	EXPECT_EQ(histogram.max_us, samples.back());
	EXPECT_EQ(histogram.total_us, std::accumulate(samples.begin(), samples.end(), uint64_t{0}));
}

TEST(Timing, RareTailIsVisibleInsteadOfAveragedAway)
{
	zlrs_timing_histogram_t histogram{};
	for (unsigned int i = 0U; i < 98U; i++) {
		zlrs_timing_record(&histogram, 7U);
	}
	zlrs_timing_record(&histogram, 1000U);
	zlrs_timing_record(&histogram, 10000U);
	EXPECT_EQ(zlrs_timing_percentile_bound(&histogram, 50U), 8U);
	EXPECT_EQ(zlrs_timing_percentile_bound(&histogram, 95U), 8U);
	EXPECT_EQ(zlrs_timing_percentile_bound(&histogram, 99U), 1024U);
	EXPECT_EQ(histogram.max_us, 10000U);
}

TEST(Timing, SaturationKeepsCountsAndTotalsConsistent)
{
	zlrs_timing_histogram_t histogram{};
	histogram.samples = UINT32_MAX - 1U;
	histogram.buckets[0] = UINT32_MAX - 1U;
	zlrs_timing_record(&histogram, UINT32_MAX);
	ASSERT_EQ(histogram.samples, UINT32_MAX);
	zlrs_timing_record(&histogram, 123U);
	EXPECT_EQ(histogram.samples, UINT32_MAX);
	EXPECT_EQ(histogram.buckets[ZLRS_TIMING_BUCKETS - 1U], 1U);
	EXPECT_EQ(histogram.total_us, UINT32_MAX);
	EXPECT_EQ(zlrs_timing_percentile_bound(&histogram, 99U), 0U);
	EXPECT_EQ(zlrs_timing_percentile_bound(&histogram, 100U), UINT32_MAX);
}
} // namespace
