#include "tickstream/bounded_queue.hpp"

#include <atomic>
#include <chrono>
#include <numeric>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using tickstream::BoundedQueue;
using namespace std::chrono_literals;

TEST(BoundedQueue, RejectsZeroCapacity) {
    EXPECT_THROW(BoundedQueue<int>(0), std::invalid_argument);
}

TEST(BoundedQueue, FifoOrderAcrossWrapAround) {
    BoundedQueue<int> queue(3);
    // Push and pop past the end of the ring several times.
    for (int round = 0; round < 5; ++round) {
        EXPECT_TRUE(queue.try_push(round * 10 + 1));
        EXPECT_TRUE(queue.try_push(round * 10 + 2));
        EXPECT_EQ(queue.pop(), round * 10 + 1);
        EXPECT_EQ(queue.pop(), round * 10 + 2);
    }
    EXPECT_EQ(queue.size(), 0u);
}

TEST(BoundedQueue, TryPushDropsAndCountsWhenFull) {
    BoundedQueue<int> queue(2);
    EXPECT_TRUE(queue.try_push(1));
    EXPECT_TRUE(queue.try_push(2));
    EXPECT_FALSE(queue.try_push(3));
    EXPECT_FALSE(queue.try_push(4));
    EXPECT_EQ(queue.dropped(), 2u);
    EXPECT_EQ(queue.size(), 2u);
    // The oldest items survive; the newest were dropped.
    EXPECT_EQ(queue.pop(), 1);
    EXPECT_EQ(queue.pop(), 2);
}

TEST(BoundedQueue, TracksHighWaterMark) {
    BoundedQueue<int> queue(10);
    queue.try_push(1);
    queue.try_push(2);
    queue.try_push(3);
    queue.pop();
    queue.pop();
    queue.try_push(4);
    EXPECT_EQ(queue.high_water_mark(), 3u);
}

TEST(BoundedQueue, CloseDrainsRemainingItemsThenReportsClosed) {
    BoundedQueue<int> queue(4);
    queue.try_push(1);
    queue.try_push(2);
    EXPECT_FALSE(queue.closed());
    queue.close();
    EXPECT_TRUE(queue.closed());
    EXPECT_FALSE(queue.try_push(3));
    EXPECT_FALSE(queue.push(3));
    EXPECT_EQ(queue.dropped(), 0u);  // rejected because closed, not because full
    EXPECT_EQ(queue.pop(), 1);
    EXPECT_EQ(queue.pop(), 2);
    EXPECT_EQ(queue.pop(), std::nullopt);
}

TEST(BoundedQueue, PopUntilTimesOutOnEmptyQueue) {
    BoundedQueue<int> queue(1);
    int out = 0;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(queue.pop_until(out, start + 20ms), BoundedQueue<int>::PopStatus::timeout);
    EXPECT_GE(std::chrono::steady_clock::now() - start, 20ms);
}

TEST(BoundedQueue, PopUntilReturnsItemAndClosed) {
    BoundedQueue<int> queue(2);
    queue.try_push(7);
    int out = 0;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    EXPECT_EQ(queue.pop_until(out, deadline), BoundedQueue<int>::PopStatus::item);
    EXPECT_EQ(out, 7);
    queue.close();
    EXPECT_EQ(queue.pop_until(out, deadline), BoundedQueue<int>::PopStatus::closed);
}

TEST(BoundedQueue, CloseWakesBlockedConsumer) {
    BoundedQueue<int> queue(1);
    std::optional<int> result = 42;
    std::thread consumer([&] { result = queue.pop(); });
    std::this_thread::sleep_for(20ms);  // let it block
    queue.close();
    consumer.join();
    EXPECT_EQ(result, std::nullopt);
}

TEST(BoundedQueue, CloseWakesBlockedProducer) {
    BoundedQueue<int> queue(1);
    queue.push(1);
    bool pushed = true;
    std::thread producer([&] { pushed = queue.push(2); });  // blocks: full
    std::this_thread::sleep_for(20ms);
    queue.close();
    producer.join();
    EXPECT_FALSE(pushed);
}

// Several producers using the blocking push into a small queue, so they
// constantly hit "full" and wait. Every item must arrive exactly once.
// Run under ThreadSanitizer to check the locking.
TEST(BoundedQueue, MultiThreadedStressDeliversEveryItemOnce) {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 20000;
    BoundedQueue<int> queue(16);

    std::vector<int> seen(kProducers * kPerProducer, 0);
    std::thread consumer([&] {
        while (auto item = queue.pop()) {
            ++seen[static_cast<std::size_t>(*item)];
        }
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&queue, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                ASSERT_TRUE(queue.push(p * kPerProducer + i));
            }
        });
    }
    for (auto& t : producers) {
        t.join();
    }
    queue.close();
    consumer.join();

    for (std::size_t i = 0; i < seen.size(); ++i) {
        ASSERT_EQ(seen[i], 1) << "item " << i;
    }
    EXPECT_LE(queue.high_water_mark(), 16u);
}
