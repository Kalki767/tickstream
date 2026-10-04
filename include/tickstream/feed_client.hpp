#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/websocket/stream.hpp>

#include "tickstream/backoff.hpp"

namespace tickstream {

struct FeedConfig {
    std::string host;    // "stream.binance.com"
    std::string port;    // "9443"
    std::string target;  // "/stream?streams=btcusdt@trade/..."
};

// A TLS WebSocket client that stays connected.
//
// It knows nothing about trades: it hands every text message to the
// MessageHandler and leaves parsing to the caller. On any failure (DNS, TCP,
// TLS, WebSocket handshake, read error, idle timeout, server close) it waits
// according to an exponential Backoff and reconnects from scratch.
//
// run() is a C++20 coroutine: one loop of resolve -> TCP -> TLS -> WebSocket
// -> read, with a single catch per connection attempt instead of one error
// check per completion handler.
//
// Threading: run() and stop() must execute on the same single-threaded
// executor. Every access to the members happens between co_await points on
// that thread, so no member needs a lock.
class FeedClient {
public:
    using MessageHandler = std::function<void(std::string_view)>;

    FeedClient(boost::asio::any_io_executor executor,
               boost::asio::ssl::context& ssl_ctx,
               FeedConfig config,
               MessageHandler on_message);

    FeedClient(const FeedClient&) = delete;
    FeedClient& operator=(const FeedClient&) = delete;

    // Connects and reads until stop() is called, reconnecting with backoff
    // after every failure. Start it with boost::asio::co_spawn. The client
    // must outlive the coroutine.
    boost::asio::awaitable<void> run();

    // Requests a graceful shutdown: sends a WebSocket close frame if connected,
    // otherwise aborts whatever step is in progress. No reconnect happens
    // after this. run() completes once the current operation unwinds.
    void stop();

    // Connection attempts that failed or dropped and were retried. Read it on
    // the executor thread.
    [[nodiscard]] std::uint64_t reconnects() const noexcept { return reconnects_; }

private:
    using WsStream = boost::beast::websocket::stream<
        boost::beast::ssl_stream<boost::beast::tcp_stream>>;

    // Sends the close frame and waits (bounded by kCloseTimeout) for the reply.
    boost::asio::awaitable<void> close_gracefully();

    boost::asio::any_io_executor executor_;
    boost::asio::ssl::context& ssl_ctx_;
    const FeedConfig config_;
    const std::string host_header_;  // "host:port", sent in the HTTP Upgrade request
    MessageHandler on_message_;

    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::steady_timer reconnect_timer_;
    Backoff backoff_;

    // A TLS stream can't be reused after a failure, so every connection
    // attempt builds a fresh one. unique_ptr gives us that "replace" operation
    // while still owning the stream (RAII: the old socket closes when replaced).
    std::unique_ptr<WsStream> ws_;
    boost::beast::flat_buffer buffer_;

    bool stopping_ = false;
    bool close_started_ = false;  // close_gracefully() owns ws_ until it completes
    std::uint64_t reconnects_ = 0;
};

}  // namespace tickstream
