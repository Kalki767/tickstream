#pragma once

#include <chrono>

namespace tickstream {

// Exponential backoff for reconnect attempts: 1s, 2s, 4s, 8s, 16s, 30s, 30s...
//
// Pure logic with no timers or I/O, so it can be unit-tested directly.
// The caller asks for next_delay() before each retry and calls reset() once
// a connection succeeds, so the next outage starts again from `initial`.
class Backoff {
public:
    explicit Backoff(std::chrono::seconds initial = std::chrono::seconds{1},
                     std::chrono::seconds max = std::chrono::seconds{30});

    // Returns the delay to wait before the next attempt, then doubles it
    // (capped at max) for the attempt after that.
    std::chrono::seconds next_delay();

    void reset();

private:
    std::chrono::seconds initial_;
    std::chrono::seconds max_;
    std::chrono::seconds current_;
};

}  // namespace tickstream
