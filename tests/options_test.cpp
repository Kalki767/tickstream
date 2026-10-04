#include "tickstream/options.hpp"

#include <stdexcept>

#include <gtest/gtest.h>

using tickstream::parse_options;

TEST(Options, DefaultsWithNoArguments) {
    const auto options = parse_options({});
    EXPECT_EQ(options.symbols.size(), 4u);
    EXPECT_FALSE(options.record_path.has_value());
    EXPECT_EQ(options.stats_interval, std::chrono::seconds{10});
    EXPECT_EQ(options.writer, tickstream::WriterKind::multirow);
    EXPECT_EQ(options.batch_size, 1000u);
    EXPECT_EQ(options.flush_ms, std::chrono::milliseconds{200});
}

TEST(Options, PositionalArgumentsReplaceDefaultSymbols) {
    const auto options = parse_options({"btcusdt", "dogeusdt"});
    EXPECT_EQ(options.symbols, (std::vector<std::string>{"btcusdt", "dogeusdt"}));
}

TEST(Options, FlagValueSeparateOrInline) {
    EXPECT_EQ(parse_options({"--record", "a.jsonl"}).record_path, "a.jsonl");
    EXPECT_EQ(parse_options({"--record=b.jsonl"}).record_path, "b.jsonl");
    EXPECT_EQ(parse_options({"--stats-interval=5"}).stats_interval, std::chrono::seconds{5});
}

TEST(Options, RejectsBadInput) {
    EXPECT_THROW(parse_options({"--bogus"}), std::invalid_argument);
    EXPECT_THROW(parse_options({"--record"}), std::invalid_argument);           // missing value
    EXPECT_THROW(parse_options({"--stats-interval", "10s"}), std::invalid_argument);
    EXPECT_THROW(parse_options({"--stats-interval", "0"}), std::invalid_argument);
    EXPECT_THROW(parse_options({"--stats-interval", "-3"}), std::invalid_argument);
}

TEST(Options, PipelineFlags) {
    const auto options = parse_options({"--replay", "cap.jsonl", "--replay-copies=3",
                                        "--writer", "null", "--dsn", "dbname=x",
                                        "--synchronous-commit", "off", "--queue-capacity",
                                        "128", "--latency-out", "lat.txt"});
    EXPECT_EQ(options.replay_path, "cap.jsonl");
    EXPECT_EQ(options.replay_copies, 3);
    EXPECT_EQ(options.writer, tickstream::WriterKind::null);
    EXPECT_EQ(options.dsn, "dbname=x");
    EXPECT_FALSE(options.synchronous_commit);
    EXPECT_EQ(options.queue_capacity, 128u);
    EXPECT_EQ(options.latency_out, "lat.txt");
}

TEST(Options, RejectsBadPipelineValues) {
    EXPECT_THROW(parse_options({"--writer", "fast"}), std::invalid_argument);
    EXPECT_THROW(parse_options({"--batch-size", "0"}), std::invalid_argument);
    EXPECT_THROW(parse_options({"--flush-ms", "abc"}), std::invalid_argument);
    EXPECT_THROW(parse_options({"--synchronous-commit", "maybe"}), std::invalid_argument);
    EXPECT_THROW(parse_options({"--replay-copies", "101"}), std::invalid_argument);
    EXPECT_THROW(parse_options({"--queue-capacity", "0"}), std::invalid_argument);
}

TEST(Options, BatchingFlags) {
    const auto options =
        parse_options({"--writer=copy", "--batch-size", "500", "--flush-ms", "100"});
    EXPECT_EQ(options.writer, tickstream::WriterKind::copy);
    EXPECT_EQ(options.batch_size, 500u);
    EXPECT_EQ(options.flush_ms, std::chrono::milliseconds{100});
    EXPECT_EQ(parse_options({"--writer", "txn"}).writer, tickstream::WriterKind::txn);
    EXPECT_EQ(parse_options({"--writer", "multirow"}).writer, tickstream::WriterKind::multirow);
}
