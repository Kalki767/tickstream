#pragma once

#include <chrono>
#include <cstddef>

namespace tickstream {

// When the writer commits a batch: as soon as it holds max_rows, or once
// max_delay has passed since its first row arrived, whichever comes first.
//
// max_rows bounds the size of each transaction; max_delay bounds how long a
// trade can wait in a half-full batch, i.e. the extra latency batching adds on
// a quiet feed, and the most a crash can lose from the writer's buffer.
struct FlushPolicy {
    using Clock = std::chrono::steady_clock;

    std::size_t max_rows = 1;
    std::chrono::milliseconds max_delay{0};

    // The time at which a batch whose first row arrived at `first_row_at`
    // must be flushed even if it isn't full.
    [[nodiscard]] Clock::time_point deadline(Clock::time_point first_row_at) const {
        return first_row_at + max_delay;
    }

    // Pure: the caller passes `now`, so tests control time.
    [[nodiscard]] bool should_flush(std::size_t rows,
                                    Clock::time_point first_row_at,
                                    Clock::time_point now) const {
        if (rows == 0) {
            return false;
        }
        return rows >= max_rows || now >= deadline(first_row_at);
    }
};

}  // namespace tickstream
