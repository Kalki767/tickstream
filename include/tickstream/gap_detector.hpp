#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tickstream {

// A run of trade ids that never arrived for one symbol.
struct Gap {
    std::string symbol;
    std::int64_t last_seen_id = 0;  // last id received before the gap
    std::int64_t next_id = 0;       // first id received after the gap

    [[nodiscard]] std::int64_t missing() const noexcept { return next_id - last_seen_id - 1; }
};

// Detects lost trades from sequence numbers.
//
// Binance trade ids are consecutive per symbol, so if id N is followed by
// N + k (k > 1), exactly k - 1 trades were missed (typically during a
// reconnect). That turns "at-most-once, silently" into "at-most-once,
// measurably".
//
// Pure logic, no I/O; not thread-safe (use from one thread).
class GapDetector {
public:
    // Records `trade_id` for `symbol`. Returns the gap if one or more ids
    // were skipped since the previous trade of that symbol. The first trade
    // of a symbol never reports a gap: there is nothing to compare it with.
    // An id at or below the last one seen (duplicate or out of order) is
    // counted in out_of_order() and does not move the high-water mark.
    std::optional<Gap> observe(std::string_view symbol, std::int64_t trade_id);

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }
    [[nodiscard]] std::int64_t missing_trades() const noexcept { return missing_trades_; }
    [[nodiscard]] std::uint64_t out_of_order() const noexcept { return out_of_order_; }

private:
    // Transparent hash so lookups by string_view don't build a std::string.
    struct StringHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };

    std::unordered_map<std::string, std::int64_t, StringHash, std::equal_to<>> last_id_;
    std::uint64_t gaps_ = 0;
    std::int64_t missing_trades_ = 0;
    std::uint64_t out_of_order_ = 0;
};

}  // namespace tickstream
