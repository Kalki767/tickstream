#include "tickstream/binance.hpp"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

namespace tickstream::binance {

std::string combined_trade_stream_target(const std::vector<std::string>& symbols) {
    std::string target = "/stream?streams=";
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) {
            target += '/';
        }
        std::string symbol = symbols[i];
        std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        target += symbol + "@trade";
    }
    return target;
}

std::optional<Trade> parse_trade(std::string_view message) {
    using nlohmann::json;

    // All nlohmann errors (parse_error, type_error, out_of_range) derive from
    // json::exception. Malformed messages are rare, so using exceptions here
    // keeps the happy path readable at no real cost; we convert them to
    // nullopt at this boundary so nothing escapes into the network code.
    try {
        const json doc = json::parse(message);

        // Combined streams wrap the payload: {"stream": "...", "data": {...}}.
        const json& data = doc.contains("data") ? doc.at("data") : doc;

        if (data.at("e").get<std::string>() != "trade") {
            return std::nullopt;
        }

        Trade trade;
        trade.symbol = data.at("s").get<std::string>();
        trade.price = data.at("p").get<std::string>();
        trade.quantity = data.at("q").get<std::string>();
        trade.trade_id = data.at("t").get<std::int64_t>();
        trade.trade_time_ms = data.at("T").get<std::int64_t>();
        trade.is_buyer_maker = data.at("m").get<bool>();
        return trade;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

}  // namespace tickstream::binance
