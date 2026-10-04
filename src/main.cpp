#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

#include "tickstream/binance.hpp"
#include "tickstream/bounded_queue.hpp"
#include "tickstream/feed_client.hpp"
#include "tickstream/flush_policy.hpp"
#include "tickstream/gap_detector.hpp"
#include "tickstream/options.hpp"
#include "tickstream/pg_writer.hpp"
#include "tickstream/replay.hpp"
#include "tickstream/trade_sink.hpp"
#include "tickstream/writer_loop.hpp"

namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using namespace std::chrono;
using tickstream::BoundedQueue;
using tickstream::Trade;

namespace {

std::unique_ptr<tickstream::TradeSink> make_sink(const tickstream::Options& options) {
    using Mode = tickstream::PgWriter::Mode;
    Mode mode{};
    switch (options.writer) {
        case tickstream::WriterKind::null:
            return std::make_unique<tickstream::NullSink>();
        case tickstream::WriterKind::naive:
            mode = Mode::naive;
            break;
        case tickstream::WriterKind::txn:
            mode = Mode::txn;
            break;
        case tickstream::WriterKind::multirow:
            mode = Mode::multirow;
            break;
        case tickstream::WriterKind::copy:
            mode = Mode::copy;
            break;
    }
    std::string dsn = "dbname=tickstream";
    if (options.dsn) {
        dsn = *options.dsn;
    } else if (const char* env = std::getenv("TICKSTREAM_DSN")) {
        dsn = env;
    }
    return std::make_unique<tickstream::PgWriter>(tickstream::PgWriter::Config{
        dsn, mode, options.batch_size, options.synchronous_commit});
}

// Live-mode counters. Only the io_context thread touches these.
struct LiveStats {
    std::map<std::string, std::uint64_t> interval_trades;  // per symbol, since last stats line
    std::uint64_t total_trades = 0;
    std::uint64_t unparseable = 0;
};

asio::awaitable<void> print_stats(asio::steady_timer& timer,
                                  seconds interval,
                                  LiveStats& stats,
                                  const tickstream::GapDetector& gaps,
                                  const tickstream::FeedClient& client,
                                  const BoundedQueue<Trade>& queue,
                                  const tickstream::WriterStats& writer) {
    for (;;) {
        timer.expires_after(interval);
        const auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            co_return;  // cancelled at shutdown
        }
        std::string per_symbol;
        double rate_total = 0;
        for (auto& [symbol, count] : stats.interval_trades) {
            const double rate = static_cast<double>(count) / static_cast<double>(interval.count());
            rate_total += rate;
            per_symbol += fmt::format(" {}={:.1f}", symbol, rate);
            count = 0;
        }
        spdlog::info("stats: {:.1f} trades/s [{} ] total={} gaps={} missing={} reconnects={} "
                     "unparseable={} queue={} queue_max={} dropped={} written={} commits={}",
                     rate_total, per_symbol, stats.total_trades, gaps.gaps(),
                     gaps.missing_trades(), client.reconnects(), stats.unparseable, queue.size(),
                     queue.high_water_mark(), queue.dropped(), writer.rows_written.load(),
                     writer.commits.load());
    }
}

// Live source: the coroutine FeedClient on this thread's io_context. Returns
// after a signal (or a writer failure) has shut the client down.
int run_live(const tickstream::Options& options,
             BoundedQueue<Trade>& queue,
             const tickstream::WriterStats& writer_stats) {
    // Binance sends compact single-line JSON, so one message per line is a
    // lossless framing. Written synchronously on the network thread: fine at
    // live rates, and ofstream buffers.
    std::ofstream record;
    if (options.record_path) {
        record.open(*options.record_path, std::ios::app);
        if (!record) {
            spdlog::critical("cannot open --record file '{}'", *options.record_path);
            return 1;
        }
        spdlog::info("recording raw messages to {}", *options.record_path);
    }

    const tickstream::FeedConfig config{
        "stream.binance.com",
        "9443",
        tickstream::binance::combined_trade_stream_target(options.symbols),
    };

    asio::io_context ioc;

    // TLS setup: trust the OS certificate store and require the server to
    // present a certificate that chains to it.
    ssl::context ssl_ctx{ssl::context::tls_client};
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(ssl::verify_peer);

    LiveStats stats;
    tickstream::GapDetector gaps;
    int exit_code = 0;

    // The client is referenced from its own handler (to stop it if the
    // writer has died), so declare it first and construct it below.
    std::unique_ptr<tickstream::FeedClient> client;
    client = std::make_unique<tickstream::FeedClient>(
        ioc.get_executor(), ssl_ctx, config, [&](std::string_view message) {
            // Stamp first, before any of our own work, so latency measured
            // from here includes parsing and queueing.
            const auto received_steady = steady_clock::now();
            const auto received_wall = system_clock::now();

            if (record.is_open()) {
                record << message << '\n';
            }

            auto trade = tickstream::binance::parse_trade(message);
            if (!trade) {
                ++stats.unparseable;
                spdlog::warn("ignoring unparseable message: {}", message);
                return;
            }
            trade->received_steady_ns =
                duration_cast<nanoseconds>(received_steady.time_since_epoch()).count();
            trade->received_wall_ms =
                duration_cast<milliseconds>(received_wall.time_since_epoch()).count();

            if (const auto gap = gaps.observe(trade->symbol, trade->trade_id)) {
                spdlog::warn("gap in {}: ids {}..{} missing ({} trades)", gap->symbol,
                             gap->last_seen_id + 1, gap->next_id - 1, gap->missing());
            }
            ++stats.interval_trades[trade->symbol];
            ++stats.total_trades;

            // Backpressure policy for live data: never block. Blocking here
            // would stall the event loop: no reads, no pings, no Ctrl+C, and
            // eventually Binance disconnects us as a slow consumer, losing
            // more than one trade. So a full queue drops the newest trade and
            // counts it (shown in the stats line).
            if (!queue.try_push(std::move(*trade)) && queue.closed()) {
                // Closed while live means the writer thread failed.
                spdlog::critical("writer stopped; shutting down");
                exit_code = 1;
                client->stop();
            }
        });

    asio::steady_timer stats_timer(ioc);
    asio::co_spawn(ioc,
                   print_stats(stats_timer, options.stats_interval, stats, gaps, *client, queue,
                               writer_stats),
                   asio::detached);

    // Graceful shutdown. Asio delivers the signal as an ordinary completion
    // handler on the io_context thread, so stop() runs like any other handler:
    // no async-signal-safety concerns, no races with the network code.
    asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code& ec, int signal_number) {
        if (ec) {
            return;
        }
        spdlog::info("received signal {}, shutting down (press Ctrl+C again to force)",
                     signal_number);
        // Removing the signals from the set restores their default action, so
        // a second Ctrl+C kills the process immediately if shutdown hangs.
        // It also means the signal_set no longer keeps run() alive.
        signals.clear();
        client->stop();
    });

    asio::co_spawn(ioc, client->run(), [&](std::exception_ptr error) {
        if (error) {
            // run() handles network failures itself; anything that escapes is a bug.
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& e) {
                spdlog::critical("feed client terminated: {}", e.what());
            }
            exit_code = 1;
        }
        // The feed is finished either way: release what keeps run() alive.
        signals.cancel();
        stats_timer.cancel();
    });

    // Runs every async operation and handler on this thread. Returns when
    // there is no work left, i.e. after the client has finished closing.
    ioc.run();

    spdlog::info("feed stopped: total={} gaps={} missing={} reconnects={} unparseable={} "
                 "dropped={} queue_max={}",
                 stats.total_trades, gaps.gaps(), gaps.missing_trades(), client->reconnects(),
                 stats.unparseable, queue.dropped(), queue.high_water_mark());
    return exit_code;
}

}  // namespace

int main(int argc, char* argv[]) {
    tickstream::Options options;
    try {
        options = tickstream::parse_options(std::vector<std::string_view>(argv + 1, argv + argc));
    } catch (const std::invalid_argument& e) {
        fmt::print(stderr, "tickstream: {}\n\n{}", e.what(), tickstream::usage());
        return 2;
    }
    if (options.show_help) {
        fmt::print("{}", tickstream::usage());
        return 0;
    }

    std::unique_ptr<tickstream::TradeSink> sink;
    std::vector<std::string> capture;
    try {
        sink = make_sink(options);
        if (options.replay_path) {
            capture = tickstream::load_capture(*options.replay_path);
        }
    } catch (const std::exception& e) {
        spdlog::critical("startup failed: {}", e.what());
        return 1;
    }

    BoundedQueue<Trade> queue(options.queue_capacity);
    tickstream::WriterStats writer_stats;
    std::vector<std::int64_t> latencies_ns;
    std::exception_ptr writer_error;

    // The consumer: one thread that owns the database connection.
    std::thread writer([&] {
        try {
            tickstream::run_writer(queue, *sink,
                                   tickstream::FlushPolicy{options.batch_size, options.flush_ms},
                                   writer_stats, options.latency_out ? &latencies_ns : nullptr);
        } catch (...) {
            writer_error = std::current_exception();
            queue.close();  // tells the source to stop
        }
    });

    int exit_code = 0;
    tickstream::ReplayResult replay_result;
    if (options.replay_path) {
        replay_result = tickstream::replay(capture, options.replay_copies, queue);
    } else {
        exit_code = run_live(options, queue, writer_stats);
    }

    // Shutdown order matters: the source has stopped producing; now close the
    // queue so the writer drains and commits everything left, then join it.
    // Exiting before the join would lose the last batch.
    queue.close();
    writer.join();

    if (writer_error) {
        try {
            std::rethrow_exception(writer_error);
        } catch (const std::exception& e) {
            spdlog::critical("writer failed: {}", e.what());
        }
        exit_code = 1;
    }

    if (options.replay_path && !writer_error) {
        const std::int64_t elapsed_ns =
            writer_stats.last_commit_steady_ns.load() - replay_result.first_push_steady_ns;
        const double elapsed_s = static_cast<double>(elapsed_ns) / 1e9;
        // One machine-readable line; bench scripts grep for "replay:".
        spdlog::info("replay: messages={} trades={} unparseable={} copies={} written={} "
                     "inserted={} commits={} elapsed_s={:.3f} throughput={:.0f} trades/s",
                     replay_result.messages, replay_result.pushed, replay_result.unparseable,
                     options.replay_copies, writer_stats.rows_written.load(),
                     writer_stats.rows_inserted.load(), writer_stats.commits.load(), elapsed_s,
                     static_cast<double>(writer_stats.rows_written.load()) / elapsed_s);
    }

    if (options.latency_out) {
        std::ofstream out(*options.latency_out);
        for (const std::int64_t ns : latencies_ns) {
            out << ns << '\n';
        }
        spdlog::info("wrote {} latency samples to {}", latencies_ns.size(), *options.latency_out);
    }

    spdlog::info("shut down");
    return exit_code;
}
