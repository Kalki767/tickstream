#include "tickstream/binance.hpp"

#include <gtest/gtest.h>

using tickstream::binance::combined_trade_stream_target;
using tickstream::binance::parse_trade;

namespace {

// Captured from wss://stream.binance.com:9443/ws/btcusdt@trade
constexpr const char* kRawTrade =
    R"({"e":"trade","E":1727190000123,"s":"BTCUSDT","t":3812345678,)"
    R"("p":"63012.45000000","q":"0.00150000","T":1727190000120,"m":true,"M":true})";

}  // namespace

TEST(ParseTrade, ParsesRawTradePayload) {
    const auto trade = parse_trade(kRawTrade);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->symbol, "BTCUSDT");
    EXPECT_EQ(trade->price, "63012.45000000");
    EXPECT_EQ(trade->quantity, "0.00150000");
    EXPECT_EQ(trade->trade_id, 3812345678);
    EXPECT_EQ(trade->trade_time_ms, 1727190000120);
    EXPECT_TRUE(trade->is_buyer_maker);
}

TEST(ParseTrade, UnwrapsCombinedStreamEnvelope) {
    const std::string message =
        std::string(R"({"stream":"btcusdt@trade","data":)") + kRawTrade + "}";
    const auto trade = parse_trade(message);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->symbol, "BTCUSDT");
    EXPECT_EQ(trade->trade_id, 3812345678);
}

TEST(ParseTrade, KeepsPriceDigitsExactly) {
    // A double can't represent 0.1 exactly; the string must survive untouched.
    const auto trade = parse_trade(
        R"({"e":"trade","s":"X","t":1,"p":"0.10000001","q":"12345678.12345678","T":1,"m":false})");
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price, "0.10000001");
    EXPECT_EQ(trade->quantity, "12345678.12345678");
}

TEST(ParseTrade, RejectsMalformedJson) {
    EXPECT_FALSE(parse_trade(R"({"e":"trade","s":)").has_value());
    EXPECT_FALSE(parse_trade("not json").has_value());
    EXPECT_FALSE(parse_trade("").has_value());
}

TEST(ParseTrade, RejectsNonObjectJson) {
    EXPECT_FALSE(parse_trade("[1,2,3]").has_value());
    EXPECT_FALSE(parse_trade("42").has_value());
}

TEST(ParseTrade, RejectsMissingField) {
    // No "p" (price).
    EXPECT_FALSE(parse_trade(
        R"({"e":"trade","s":"BTCUSDT","t":1,"q":"1","T":1,"m":true})").has_value());
}

TEST(ParseTrade, RejectsWrongFieldType) {
    // Price as a JSON number instead of a string.
    EXPECT_FALSE(parse_trade(
        R"({"e":"trade","s":"BTCUSDT","t":1,"p":63012.45,"q":"1","T":1,"m":true})").has_value());
    // trade id as a string.
    EXPECT_FALSE(parse_trade(
        R"({"e":"trade","s":"BTCUSDT","t":"1","p":"1","q":"1","T":1,"m":true})").has_value());
}

TEST(ParseTrade, IgnoresOtherEventTypes) {
    EXPECT_FALSE(parse_trade(R"({"e":"aggTrade","s":"BTCUSDT"})").has_value());
    // Reply to a SUBSCRIBE request.
    EXPECT_FALSE(parse_trade(R"({"result":null,"id":1})").has_value());
}

TEST(CombinedStreamTarget, JoinsAndLowercasesSymbols) {
    EXPECT_EQ(combined_trade_stream_target({"BTCUSDT", "ethusdt"}),
              "/stream?streams=btcusdt@trade/ethusdt@trade");
}

TEST(CombinedStreamTarget, SingleSymbol) {
    EXPECT_EQ(combined_trade_stream_target({"solusdt"}), "/stream?streams=solusdt@trade");
}
