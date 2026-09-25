#include "tickstream/backoff.hpp"

#include <gtest/gtest.h>

using std::chrono::seconds;
using tickstream::Backoff;

TEST(Backoff, DoublesFromInitialDelay) {
    Backoff backoff;
    EXPECT_EQ(backoff.next_delay(), seconds{1});
    EXPECT_EQ(backoff.next_delay(), seconds{2});
    EXPECT_EQ(backoff.next_delay(), seconds{4});
    EXPECT_EQ(backoff.next_delay(), seconds{8});
    EXPECT_EQ(backoff.next_delay(), seconds{16});
}

TEST(Backoff, CapsAtMaximum) {
    Backoff backoff;
    for (int i = 0; i < 5; ++i) {
        backoff.next_delay();  // 1, 2, 4, 8, 16
    }
    EXPECT_EQ(backoff.next_delay(), seconds{30});
    EXPECT_EQ(backoff.next_delay(), seconds{30});
    EXPECT_EQ(backoff.next_delay(), seconds{30});
}

TEST(Backoff, ResetStartsOver) {
    Backoff backoff;
    backoff.next_delay();
    backoff.next_delay();
    backoff.next_delay();
    backoff.reset();
    EXPECT_EQ(backoff.next_delay(), seconds{1});
    EXPECT_EQ(backoff.next_delay(), seconds{2});
}

TEST(Backoff, CustomInitialAndMax) {
    Backoff backoff(seconds{3}, seconds{10});
    EXPECT_EQ(backoff.next_delay(), seconds{3});
    EXPECT_EQ(backoff.next_delay(), seconds{6});
    EXPECT_EQ(backoff.next_delay(), seconds{10});
    EXPECT_EQ(backoff.next_delay(), seconds{10});
}
