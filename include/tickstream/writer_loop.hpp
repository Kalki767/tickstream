#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

#include "tickstream/bounded_queue.hpp"
#include "tickstream/flush_policy.hpp"
#include "tickstream/trade.hpp"
#include "tickstream/trade_sink.hpp"

namespace tickstream {

// Counters the writer thread publishes for the stats line (read from other
// threads, hence atomic).
struct WriterStats {
    std::atomic<std::uint64_t> rows_written{0};   // handed to the sink and committed
    std::atomic<std::uint64_t> rows_inserted{0};  // actually new rows (not duplicates)
    std::atomic<std::uint64_t> commits{0};        // sink.write() calls
    // steady_clock ns of the most recent commit; replay reads it after join().
    std::atomic<std::int64_t> last_commit_steady_ns{0};
};

// The writer thread's body: pops trades into a batch, writes each batch
// through `sink` when `policy` says so (full, or its first row is
// policy.max_delay old), and returns once the queue is closed and fully
// drained, so nothing queued before close() is lost.
//
// If `latencies_ns` is non-null, appends one receive -> commit latency per
// row (steady clock, measured right after the commit returns). Only this
// thread touches the vector; read it after joining.
//
// Exceptions from the sink propagate to the caller.
void run_writer(BoundedQueue<Trade>& queue,
                TradeSink& sink,
                const FlushPolicy& policy,
                WriterStats& stats,
                std::vector<std::int64_t>* latencies_ns);

}  // namespace tickstream
