#include "tickstream/gap_detector.hpp"

#include <gtest/gtest.h>

using tickstream::GapDetector;

TEST(GapDetector, FirstTradeOfSymbolIsNeverAGap) {
    GapDetector detector;
    EXPECT_FALSE(detector.observe("BTCUSDT", 1000).has_value());
    EXPECT_EQ(detector.gaps(), 0u);
}

TEST(GapDetector, ConsecutiveIdsAreNotAGap) {
    GapDetector detector;
    for (std::int64_t id = 1; id <= 100; ++id) {
        EXPECT_FALSE(detector.observe("BTCUSDT", id).has_value());
    }
    EXPECT_EQ(detector.gaps(), 0u);
    EXPECT_EQ(detector.missing_trades(), 0);
}

TEST(GapDetector, ReportsSkippedIds) {
    GapDetector detector;
    detector.observe("BTCUSDT", 10);
    const auto gap = detector.observe("BTCUSDT", 15);  // 11..14 missing
    ASSERT_TRUE(gap.has_value());
    EXPECT_EQ(gap->symbol, "BTCUSDT");
    EXPECT_EQ(gap->last_seen_id, 10);
    EXPECT_EQ(gap->next_id, 15);
    EXPECT_EQ(gap->missing(), 4);
    EXPECT_EQ(detector.gaps(), 1u);
    EXPECT_EQ(detector.missing_trades(), 4);
}

TEST(GapDetector, TracksSymbolsIndependently) {
    GapDetector detector;
    detector.observe("BTCUSDT", 100);
    detector.observe("ETHUSDT", 5000);
    EXPECT_FALSE(detector.observe("BTCUSDT", 101).has_value());
    EXPECT_FALSE(detector.observe("ETHUSDT", 5001).has_value());
    EXPECT_TRUE(detector.observe("ETHUSDT", 5003).has_value());  // 5002 missing
    EXPECT_FALSE(detector.observe("BTCUSDT", 102).has_value());
    EXPECT_EQ(detector.gaps(), 1u);
    EXPECT_EQ(detector.missing_trades(), 1);
}

TEST(GapDetector, AccumulatesAcrossGaps) {
    GapDetector detector;
    detector.observe("X", 1);
    detector.observe("X", 3);   // 1 missing
    detector.observe("X", 10);  // 6 missing
    EXPECT_EQ(detector.gaps(), 2u);
    EXPECT_EQ(detector.missing_trades(), 7);
}

TEST(GapDetector, DuplicatesAndOldIdsAreCountedNotGaps) {
    GapDetector detector;
    detector.observe("X", 10);
    EXPECT_FALSE(detector.observe("X", 10).has_value());  // duplicate
    EXPECT_FALSE(detector.observe("X", 7).has_value());   // older
    EXPECT_EQ(detector.out_of_order(), 2u);
    // The high-water mark stays at 10, so 11 is not a gap.
    EXPECT_FALSE(detector.observe("X", 11).has_value());
    EXPECT_EQ(detector.gaps(), 0u);
}
