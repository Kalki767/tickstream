#include "tickstream/options.hpp"

#include <charconv>
#include <stdexcept>

namespace tickstream {

namespace {

// Parses a whole string as a positive integer, rejecting trailing junk
// ("10s") and overflow, which std::stoi would quietly accept or throw
// something unhelpful for.
long long parse_positive(std::string_view flag, std::string_view text) {
    long long value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value <= 0) {
        throw std::invalid_argument(std::string(flag) + " expects a positive integer, got '" +
                                    std::string(text) + "'");
    }
    return value;
}

}  // namespace

Options parse_options(const std::vector<std::string_view>& args) {
    Options options;
    std::vector<std::string> symbols;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];

        // Accept both "--flag value" and "--flag=value".
        std::string_view flag = arg;
        std::optional<std::string_view> inline_value;
        if (arg.starts_with("--")) {
            if (const auto eq = arg.find('='); eq != std::string_view::npos) {
                flag = arg.substr(0, eq);
                inline_value = arg.substr(eq + 1);
            }
        }
        const auto value = [&]() -> std::string_view {
            if (inline_value) {
                return *inline_value;
            }
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(std::string(flag) + " requires a value");
            }
            return args[++i];
        };

        if (flag == "--help" || flag == "-h") {
            options.show_help = true;
        } else if (flag == "--record") {
            options.record_path = std::string(value());
        } else if (flag == "--stats-interval") {
            options.stats_interval = std::chrono::seconds{parse_positive(flag, value())};
        } else if (flag == "--replay") {
            options.replay_path = std::string(value());
        } else if (flag == "--replay-copies") {
            const auto copies = parse_positive(flag, value());
            if (copies > 100) {  // copy k offsets ids by k * 10^10; keep far from INT64_MAX
                throw std::invalid_argument("--replay-copies must be <= 100");
            }
            options.replay_copies = static_cast<int>(copies);
        } else if (flag == "--writer") {
            const std::string_view v = value();
            if (v == "null") {
                options.writer = WriterKind::null;
            } else if (v == "naive") {
                options.writer = WriterKind::naive;
            } else if (v == "txn") {
                options.writer = WriterKind::txn;
            } else if (v == "multirow") {
                options.writer = WriterKind::multirow;
            } else if (v == "copy") {
                options.writer = WriterKind::copy;
            } else {
                throw std::invalid_argument(
                    "--writer must be null, naive, txn, multirow or copy, got '" +
                    std::string(v) + "'");
            }
        } else if (flag == "--batch-size") {
            options.batch_size = static_cast<std::size_t>(parse_positive(flag, value()));
        } else if (flag == "--flush-ms") {
            options.flush_ms = std::chrono::milliseconds{parse_positive(flag, value())};
        } else if (flag == "--dsn") {
            options.dsn = std::string(value());
        } else if (flag == "--synchronous-commit") {
            const std::string_view v = value();
            if (v != "on" && v != "off") {
                throw std::invalid_argument("--synchronous-commit must be on or off");
            }
            options.synchronous_commit = (v == "on");
        } else if (flag == "--queue-capacity") {
            options.queue_capacity = static_cast<std::size_t>(parse_positive(flag, value()));
        } else if (flag == "--latency-out") {
            options.latency_out = std::string(value());
        } else if (arg.starts_with("-")) {
            throw std::invalid_argument("unknown option '" + std::string(arg) + "'");
        } else {
            symbols.emplace_back(arg);
        }
    }

    if (!symbols.empty()) {
        options.symbols = std::move(symbols);
    }
    return options;
}

std::string usage() {
    return R"(usage: tickstream [options] [symbol...]

Streams live Binance trades for the given symbols (default: btcusdt ethusdt
solusdt bnbusdt).

Pipeline: source -> parse -> bounded queue -> writer thread -> PostgreSQL.

source:
  (default)                live Binance feed; if the queue is full, the newest
                           trade is dropped and counted (never blocks the network)
  --replay FILE            replay a capture as fast as possible instead; the
                           queue blocks when full (a benchmark must not drop)
  --replay-copies K        replay the file K times, offsetting trade ids by
                           k * 10^10 so copies are distinct rows (default 1)
  --record FILE            live only: append every raw message to FILE

writer:
  --writer MODE            null      discard everything
                           naive     one INSERT per trade, autocommit
                           txn       per-trade INSERTs, one transaction per batch
                           multirow  one multi-row INSERT per batch (default)
                           copy      COPY into staging, then INSERT ... SELECT
  --batch-size N           flush a batch when it holds N trades (default 1000)
  --flush-ms M             ...or when its first trade is M ms old (default 200)
  --dsn DSN                libpq connection string (default $TICKSTREAM_DSN,
                           else "dbname=tickstream")
  --synchronous-commit on|off
                           off is a diagnostic only: it gives up durability
  --queue-capacity N       queue size in trades (default 65536)
  --latency-out FILE       write one receive->commit latency (ns) per row

other:
  --stats-interval SECONDS print a stats line this often (default 10)
  -h, --help               show this help
)";
}

}  // namespace tickstream
