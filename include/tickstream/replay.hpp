#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tickstream/bounded_queue.hpp"
#include "tickstream/trade.hpp"

namespace tickstream {

// Copy k of a replay adds k * kReplayIdOffset to every trade_id, so repeated
// copies of one capture are distinct rows instead of ON CONFLICT no-ops.
// Binance ids are currently < 10^10, so copies can't collide with each other.
inline constexpr std::int64_t kReplayIdOffset = 10'000'000'000;

struct ReplayResult {
    std::uint64_t messages = 0;     // messages processed (lines x copies)
    std::uint64_t pushed = 0;       // trades enqueued
    std::uint64_t unparseable = 0;  // messages that weren't trades
    std::int64_t first_push_steady_ns = 0;
};

// Reads a capture file (one raw message per line) into memory, so file I/O
// isn't part of the timed replay. Throws std::runtime_error if unreadable.
std::vector<std::string> load_capture(const std::string& path);

// Feeds `lines` through the same parse -> queue path as the live feed,
// `copies` times, as fast as the queue accepts them. Uses the *blocking*
// push: a benchmark must never drop rows, or a slow writer would look fast.
// Receive timestamps are stamped per message, exactly like the live handler.
ReplayResult replay(const std::vector<std::string>& lines, int copies,
                    BoundedQueue<Trade>& queue);

}  // namespace tickstream
