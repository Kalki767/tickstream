#include <csignal>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl/context.hpp>
#include <spdlog/spdlog.h>

#include "tickstream/binance.hpp"
#include "tickstream/feed_client.hpp"

namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;

int main(int argc, char* argv[]) {
    // Symbols from the command line, or a default set with decent volume.
    std::vector<std::string> symbols(argv + 1, argv + argc);
    if (symbols.empty()) {
        symbols = {"btcusdt", "ethusdt", "solusdt", "bnbusdt"};
    }

    const tickstream::FeedConfig config{
        "stream.binance.com",
        "9443",
        tickstream::binance::combined_trade_stream_target(symbols),
    };

    asio::io_context ioc;

    // TLS setup: trust the OS certificate store and require the server to
    // present a certificate that chains to it.
    ssl::context ssl_ctx{ssl::context::tls_client};
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(ssl::verify_peer);

    tickstream::FeedClient client(ioc, ssl_ctx, config, [](std::string_view message) {
        if (const auto trade = tickstream::binance::parse_trade(message)) {
            spdlog::info("{} price={} qty={} id={} time_ms={} buyer_maker={}",
                         trade->symbol, trade->price, trade->quantity, trade->trade_id,
                         trade->trade_time_ms, trade->is_buyer_maker);
        } else {
            spdlog::warn("ignoring unparseable message: {}", message);
        }
    });

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
        client.stop();
    });

    client.start();

    // Runs every async operation and handler on this thread. Returns when
    // there is no work left, i.e. after stop() has finished closing.
    ioc.run();

    spdlog::info("shut down");
    return 0;
}
