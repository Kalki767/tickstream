// Tests for the source -> queue -> writer pipeline without a database:
// replay() as the source and fake TradeSinks as the database.

#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "tickstream/bounded_queue.hpp"
#include "tickstream/flush_policy.hpp"
#include "tickstream/replay.hpp"
#include "tickstream/trade_sink.hpp"
#include "tickstream/writer_loop.hpp"

using namespace tickstream;

namespace {

const std::vector<std::string> kCapture = {
    R"({"stream":"btcusdt@trade","data":{"e":"trade","s":"BTCUSDT","t":100,"p":"1.0","q":"2.0","T":1000,"m":true}})",
    R"({"stream":"ethusdt@trade","data":{"e":"trade","s":"ETHUSDT","t":7,"p":"3.0","q":"4.0","T":1001,"m":false}})",
    R"({"result":null,"id":1})",  // not a trade
};

// Records every trade it is given.
class RecordingSink final : public TradeSink {
public:
    std::uint64_t write(std::span<const Trade> trades) override {
        ++calls;
        rows.insert(rows.end(), trades.begin(), trades.end());
        return trades.size();
    }
    std::vector<Trade> rows;
    int calls = 0;
};

class FailingSink final : public TradeSink {
public:
    std::uint64_t write(std::span<const Trade>) override {
        throw std::runtime_error("database down");
    }
};

}  // namespace

TEST(Replay, PushesEveryTradeOncePerCopyWithIdOffsets) {
    BoundedQueue<Trade> queue(16);
    const ReplayResult result = replay(kCapture, 2, queue);
    EXPECT_EQ(result.messages, 6u);
    EXPECT_EQ(result.pushed, 4u);
    EXPECT_EQ(result.unparseable, 2u);
    EXPECT_GT(result.first_push_steady_ns, 0);

    queue.close();
    std::vector<std::int64_t> ids;
    while (auto t = queue.pop()) {
        EXPECT_GT(t->received_steady_ns, 0);
        EXPECT_GT(t->received_wall_ms, 0);
        ids.push_back(t->trade_id);
    }
    EXPECT_EQ(ids, (std::vector<std::int64_t>{100, 7, 100 + kReplayIdOffset, 7 + kReplayIdOffset}));
}

TEST(WriterLoop, DrainsEverythingQueuedBeforeClose) {
    BoundedQueue<Trade> queue(16);
    replay(kCapture, 3, queue);  // 6 trades
    queue.close();               // closed *before* the writer even starts

    RecordingSink sink;
    WriterStats stats;
    std::vector<std::int64_t> latencies;
    run_writer(queue, sink, FlushPolicy{}, stats, &latencies);

    EXPECT_EQ(sink.rows.size(), 6u);
    EXPECT_EQ(stats.rows_written.load(), 6u);
    EXPECT_EQ(stats.rows_inserted.load(), 6u);
    EXPECT_EQ(latencies.size(), 6u);
    for (auto ns : latencies) {
        EXPECT_GE(ns, 0);
    }
}

TEST(WriterLoop, ConcurrentProducerAndWriterLoseNothing) {
    BoundedQueue<Trade> queue(2);  // tiny: forces the producer to block
    RecordingSink sink;
    WriterStats stats;
    std::thread writer([&] { run_writer(queue, sink, FlushPolicy{}, stats, nullptr); });
    const ReplayResult result = replay(kCapture, 500, queue);
    queue.close();
    writer.join();
    EXPECT_EQ(result.pushed, 1000u);
    EXPECT_EQ(sink.rows.size(), 1000u);
}

TEST(WriterLoop, SinkFailurePropagates) {
    BoundedQueue<Trade> queue(4);
    replay(kCapture, 1, queue);
    queue.close();
    FailingSink sink;
    WriterStats stats;
    EXPECT_THROW(run_writer(queue, sink, FlushPolicy{}, stats, nullptr), std::runtime_error);
}

TEST(WriterLoop, BatchesBySizeAndFlushesRemainderOnClose) {
    BoundedQueue<Trade> queue(64);
    replay(kCapture, 5, queue);  // 10 trades, all queued before the writer runs
    queue.close();
    RecordingSink sink;
    WriterStats stats;
    using namespace std::chrono_literals;
    run_writer(queue, sink, FlushPolicy{4, 10s}, stats, nullptr);
    EXPECT_EQ(sink.rows.size(), 10u);
    EXPECT_EQ(sink.calls, 3);  // 4 + 4 + the remaining 2 at shutdown
}

TEST(WriterLoop, PartialBatchIsFlushedAfterMaxDelay) {
    using namespace std::chrono_literals;
    BoundedQueue<Trade> queue(64);
    RecordingSink sink;
    WriterStats stats;
    std::thread writer([&] { run_writer(queue, sink, FlushPolicy{1000, 50ms}, stats, nullptr); });
    replay(kCapture, 1, queue);  // 2 trades: far below max_rows
    // Without the time trigger these would sit in the batch until close().
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (stats.rows_written.load() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(stats.rows_written.load(), 2u);
    queue.close();
    writer.join();
    EXPECT_EQ(sink.calls, 1);
}
