#pragma once

#include <cstdint>
#include <string>

namespace tickstream {

// One executed trade from the exchange.
//
// price and quantity are kept as the exact decimal strings Binance sends
// (e.g. "63012.45000000"). Converting to double would silently round them;
// the database stores them as NUMERIC, which parses the strings exactly.
struct Trade {
    std::string symbol;          // "BTCUSDT"
    std::string price;           // exact decimal string
    std::string quantity;        // exact decimal string
    std::int64_t trade_id = 0;   // unique per symbol, not globally
    std::int64_t trade_time_ms = 0;  // exchange trade time, ms since Unix epoch (UTC)
    bool is_buyer_maker = false; // true => the buyer's order was resting, i.e. a sell hit the bid
};

}  // namespace tickstream
