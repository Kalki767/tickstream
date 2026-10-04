#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tickstream {

// How the writer thread stores trades.
enum class WriterKind {
    null,      // discard (capture-only runs; pipeline ceiling without a database)
    naive,     // one prepared INSERT per trade, autocommit
    txn,       // per-trade INSERTs, one transaction per batch
    multirow,  // one multi-row INSERT per batch
    copy,      // COPY into staging + INSERT ... SELECT per batch
};

// Command-line configuration. Defaults: stream the default symbols live and
// write them with the multirow writer.
struct Options {
    std::vector<std::string> symbols{"btcusdt", "ethusdt", "solusdt", "bnbusdt"};
    std::optional<std::string> record_path;   // --record FILE: append raw messages
    std::chrono::seconds stats_interval{10};  // --stats-interval SECONDS

    std::optional<std::string> replay_path;  // --replay FILE: replay instead of live
    int replay_copies = 1;                   // --replay-copies K

    // Defaults chosen from the Phase 4 sweep (BENCHMARKS.md): live batches are
    // small (time-triggered), and multirow beats copy at small batch sizes.
    WriterKind writer = WriterKind::multirow;  // --writer null|naive|txn|multirow|copy
    std::size_t batch_size = 1000;             // --batch-size N: flush when a batch has N rows
    std::chrono::milliseconds flush_ms{200};  // --flush-ms M: ...or when its first row is M ms old
    std::optional<std::string> dsn;         // --dsn; main() falls back to $TICKSTREAM_DSN
    bool synchronous_commit = true;         // --synchronous-commit on|off (diagnostic)
    std::size_t queue_capacity = 65536;     // --queue-capacity N
    std::optional<std::string> latency_out;  // --latency-out FILE: receive->commit ns per row

    bool show_help = false;  // --help
};

// Parses the arguments after argv[0]. Throws std::invalid_argument with a
// human-readable message on unknown flags, missing values or bad numbers.
// Pure: no I/O, so it can be unit-tested directly.
Options parse_options(const std::vector<std::string_view>& args);

// The --help text.
std::string usage();

}  // namespace tickstream
