#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tickstream/trade.hpp"

namespace tickstream::binance {

// Builds the request target for Binance's combined stream endpoint, which
// multiplexes several symbols over one WebSocket connection:
//   {"btcusdt","ethusdt"} -> "/stream?streams=btcusdt@trade/ethusdt@trade"
// Symbols are lower-cased because Binance stream names must be lower case.
std::string combined_trade_stream_target(const std::vector<std::string>& symbols);

// Parses one WebSocket text message into a Trade.
//
// Accepts both the raw trade payload ({"e":"trade",...}) and the combined
// stream envelope ({"stream":"...","data":{"e":"trade",...}}).
// Returns std::nullopt for anything that isn't a well-formed trade (bad JSON,
// missing or wrongly-typed fields, other event types). It never throws: one
// bad message must not take down the feed.
std::optional<Trade> parse_trade(std::string_view message);

// The two implementations behind parse_trade(), exposed for the differential
// test (they must agree on real captured messages) and the benchmark.
//   dom: builds an nlohmann::json tree, then reads fields from it.
//   sax: nlohmann's SAX interface; reads fields as the parser meets them,
//        without building a tree. Stricter on one point: trade ids must be
//        JSON integers (the DOM version's get<int64_t>() also accepts and
//        truncates floats, and converts booleans).
std::optional<Trade> parse_trade_dom(std::string_view message);
std::optional<Trade> parse_trade_sax(std::string_view message);

}  // namespace tickstream::binance
