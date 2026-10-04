#include "tickstream/writer_loop.hpp"

namespace tickstream {

namespace {

std::int64_t to_ns(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

}  // namespace

void run_writer(BoundedQueue<Trade>& queue,
                TradeSink& sink,
                const FlushPolicy& policy,
                WriterStats& stats,
                std::vector<std::int64_t>* latencies_ns) {
    using Clock = FlushPolicy::Clock;

    std::vector<Trade> batch;
    batch.reserve(policy.max_rows);
    Clock::time_point first_row_at{};

    const auto flush = [&] {
        const std::uint64_t inserted = sink.write(batch);
        const std::int64_t committed_ns = to_ns(Clock::now());
        if (latencies_ns != nullptr) {
            for (const Trade& t : batch) {
                latencies_ns->push_back(committed_ns - t.received_steady_ns);
            }
        }
        stats.rows_written.fetch_add(batch.size(), std::memory_order_relaxed);
        stats.rows_inserted.fetch_add(inserted, std::memory_order_relaxed);
        stats.commits.fetch_add(1, std::memory_order_relaxed);
        stats.last_commit_steady_ns.store(committed_ns, std::memory_order_relaxed);
        batch.clear();
    };

    for (;;) {
        if (batch.empty()) {
            // Nothing pending, so no deadline: sleep until a row arrives.
            auto trade = queue.pop();
            if (!trade) {
                break;  // closed and drained
            }
            batch.push_back(std::move(*trade));
            first_row_at = Clock::now();
        } else {
            // A batch is open: wait for more rows, but no later than its deadline.
            Trade trade;
            const auto status = queue.pop_until(trade, policy.deadline(first_row_at));
            if (status == BoundedQueue<Trade>::PopStatus::closed) {
                break;
            }
            if (status == BoundedQueue<Trade>::PopStatus::item) {
                batch.push_back(std::move(trade));
            }
        }
        if (policy.should_flush(batch.size(), first_row_at, Clock::now())) {
            flush();
        }
    }
    // Shutdown: commit whatever is left, however small.
    if (!batch.empty()) {
        flush();
    }
}

}  // namespace tickstream
