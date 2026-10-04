// Differential test: the SAX parser must produce exactly what the DOM parser
// produces for every real message in the committed capture sample.

#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "tickstream/binance.hpp"

namespace binance = tickstream::binance;

TEST(ParserEquivalence, SaxMatchesDomOnRealCapture) {
    std::ifstream in(TICKSTREAM_SOURCE_DIR "/bench/data/sample_capture.jsonl");
    ASSERT_TRUE(in) << "missing bench/data/sample_capture.jsonl";
    int lines = 0;
    for (std::string line; std::getline(in, line); ++lines) {
        const auto dom = binance::parse_trade_dom(line);
        const auto sax = binance::parse_trade_sax(line);
        ASSERT_TRUE(dom.has_value()) << line;
        ASSERT_TRUE(sax.has_value()) << line;
        EXPECT_EQ(sax->symbol, dom->symbol) << line;
        EXPECT_EQ(sax->price, dom->price) << line;
        EXPECT_EQ(sax->quantity, dom->quantity) << line;
        EXPECT_EQ(sax->trade_id, dom->trade_id) << line;
        EXPECT_EQ(sax->trade_time_ms, dom->trade_time_ms) << line;
        EXPECT_EQ(sax->is_buyer_maker, dom->is_buyer_maker) << line;
    }
    EXPECT_EQ(lines, 1000);
}
