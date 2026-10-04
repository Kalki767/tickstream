#include "tickstream/binance.hpp"

#include <gtest/gtest.h>

using tickstream::binance::combined_trade_stream_target;

namespace {

// Captured from wss://stream.binance.com:9443/ws/btcusdt@trade
constexpr const char* kRawTrade =
    R"({"e":"trade","E":1727190000123,"s":"BTCUSDT","t":3812345678,)"
    R"("p":"63012.45000000","q":"0.00150000","T":1727190000120,"m":true,"M":true})";

// Every ParseTrade test runs against each implementation: the public entry
// point and both parsers behind it.
using Parser = std::optional<tickstream::Trade> (*)(std::string_view);

struct NamedParser {
    const char* name;
    Parser parse;
};

class ParseTrade : public ::testing::TestWithParam<NamedParser> {
protected:
    static std::optional<tickstream::Trade> parse_trade(std::string_view message) {
        return GetParam().parse(message);
    }
};

}  // namespace

INSTANTIATE_TEST_SUITE_P(
    Parsers, ParseTrade,
    ::testing::Values(NamedParser{"public", &tickstream::binance::parse_trade},
                      NamedParser{"dom", &tickstream::binance::parse_trade_dom},
                      NamedParser{"sax", &tickstream::binance::parse_trade_sax}),
    [](const ::testing::TestParamInfo<NamedParser>& param_info) { return param_info.param.name; });

TEST_P(ParseTrade, ParsesRawTradePayload) {
    const auto trade = parse_trade(kRawTrade);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->symbol, "BTCUSDT");
    EXPECT_EQ(trade->price, "63012.45000000");
    EXPECT_EQ(trade->quantity, "0.00150000");
    EXPECT_EQ(trade->trade_id, 3812345678);
    EXPECT_EQ(trade->trade_time_ms, 1727190000120);
    EXPECT_TRUE(trade->is_buyer_maker);
}

TEST_P(ParseTrade, UnwrapsCombinedStreamEnvelope) {
    const std::string message =
        std::string(R"({"stream":"btcusdt@trade","data":)") + kRawTrade + "}";
    const auto trade = parse_trade(message);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->symbol, "BTCUSDT");
    EXPECT_EQ(trade->trade_id, 3812345678);
}

TEST_P(ParseTrade, KeepsPriceDigitsExactly) {
    // A double can't represent 0.1 exactly; the string must survive untouched.
    const auto trade = parse_trade(
        R"({"e":"trade","s":"X","t":1,"p":"0.10000001","q":"12345678.12345678","T":1,"m":false})");
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price, "0.10000001");
    EXPECT_EQ(trade->quantity, "12345678.12345678");
}

TEST_P(ParseTrade, RejectsMalformedJson) {
    EXPECT_FALSE(parse_trade(R"({"e":"trade","s":)").has_value());
    EXPECT_FALSE(parse_trade("not json").has_value());
    EXPECT_FALSE(parse_trade("").has_value());
}

TEST_P(ParseTrade, RejectsNonObjectJson) {
    EXPECT_FALSE(parse_trade("[1,2,3]").has_value());
    EXPECT_FALSE(parse_trade("42").has_value());
}

TEST_P(ParseTrade, RejectsMissingField) {
    // No "p" (price).
    EXPECT_FALSE(parse_trade(
        R"({"e":"trade","s":"BTCUSDT","t":1,"q":"1","T":1,"m":true})").has_value());
}

TEST_P(ParseTrade, RejectsWrongFieldType) {
    // Price as a JSON number instead of a string.
    EXPECT_FALSE(parse_trade(
        R"({"e":"trade","s":"BTCUSDT","t":1,"p":63012.45,"q":"1","T":1,"m":true})").has_value());
    // trade id as a string.
    EXPECT_FALSE(parse_trade(
        R"({"e":"trade","s":"BTCUSDT","t":"1","p":"1","q":"1","T":1,"m":true})").has_value());
}

TEST_P(ParseTrade, IgnoresOtherEventTypes) {
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

TEST_P(ParseTrade, RejectsDataThatIsNotAnObject) {
    EXPECT_FALSE(parse_trade(R"({"stream":"x","data":"oops"})").has_value());
    EXPECT_FALSE(parse_trade(R"({"stream":"x","data":null})").has_value());
}

TEST_P(ParseTrade, EnvelopeWinsOverTopLevelFields) {
    // A valid-looking top level is ignored when "data" is present but broken,
    // because the envelope says the payload is in "data".
    EXPECT_FALSE(parse_trade(
        R"({"e":"trade","s":"A","t":1,"p":"1","q":"1","T":1,"m":true,"data":{"e":"trade"}})")
                     .has_value());
}

TEST_P(ParseTrade, IgnoresUnknownAndNestedFields) {
    const auto trade = parse_trade(
        R"({"stream":"x","extra":{"a":[1,2,{"b":null}]},"data":{"e":"trade","E":5,)"
        R"("s":"ETHUSDT","t":9,"p":"2.5","q":"0.1","T":7,"m":false,"M":true,"x":[1,{"y":2}]}})");
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->symbol, "ETHUSDT");
    EXPECT_EQ(trade->trade_id, 9);
    EXPECT_EQ(trade->trade_time_ms, 7);
    EXPECT_FALSE(trade->is_buyer_maker);
}

TEST_P(ParseTrade, RejectsTrailingGarbage) {
    EXPECT_FALSE(parse_trade(std::string(kRawTrade) + "x").has_value());
}
