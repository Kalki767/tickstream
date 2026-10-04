#include "tickstream/flush_policy.hpp"

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using tickstream::FlushPolicy;
using Clock = FlushPolicy::Clock;

namespace {
// A fixed, arbitrary origin: the policy never reads the real clock.
const Clock::time_point t0 = Clock::time_point{} + 1h;
}  // namespace

TEST(FlushPolicy, EmptyBatchNeverFlushes) {
    const FlushPolicy policy{100, 200ms};
    EXPECT_FALSE(policy.should_flush(0, t0, t0 + 10s));
}

TEST(FlushPolicy, FlushesWhenFull) {
    const FlushPolicy policy{100, 200ms};
    EXPECT_FALSE(policy.should_flush(99, t0, t0));
    EXPECT_TRUE(policy.should_flush(100, t0, t0));
    EXPECT_TRUE(policy.should_flush(101, t0, t0));  // defensive: never wait past full
}

TEST(FlushPolicy, FlushesWhenFirstRowIsOldEnough) {
    const FlushPolicy policy{100, 200ms};
    EXPECT_FALSE(policy.should_flush(1, t0, t0 + 199ms));
    EXPECT_TRUE(policy.should_flush(1, t0, t0 + 200ms));
    EXPECT_TRUE(policy.should_flush(1, t0, t0 + 5s));
}

TEST(FlushPolicy, DeadlineIsMeasuredFromFirstRow) {
    const FlushPolicy policy{100, 200ms};
    EXPECT_EQ(policy.deadline(t0), t0 + 200ms);
}

TEST(FlushPolicy, BatchSizeOneFlushesEveryRow) {
    const FlushPolicy policy{1, 200ms};
    EXPECT_TRUE(policy.should_flush(1, t0, t0));
}

TEST(FlushPolicy, ZeroDelayFlushesImmediately) {
    const FlushPolicy policy{100, 0ms};
    EXPECT_TRUE(policy.should_flush(1, t0, t0));
}
