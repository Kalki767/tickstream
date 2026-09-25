#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <boost/asio/io_context.hpp>
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
// Threading: everything runs on the one thread that calls io_context::run().
// All handlers therefore run one at a time, so no member needs a lock, and
// handlers can safely capture `this` because the client is created before
// run() and destroyed after it returns.
class FeedClient {
public:
    using MessageHandler = std::function<void(std::string_view)>;

    FeedClient(boost::asio::io_context& ioc,
               boost::asio::ssl::context& ssl_ctx,
               FeedConfig config,
               MessageHandler on_message);

    FeedClient(const FeedClient&) = delete;
    FeedClient& operator=(const FeedClient&) = delete;

    // Starts the first connection attempt. Returns immediately.
    void start();

    // Requests a graceful shutdown: sends a WebSocket close frame if connected,
    // otherwise aborts whatever step is in progress. No reconnect happens
    // after this. io_context::run() returns once the close completes.
    void stop();

private:
    using WsStream = boost::beast::websocket::stream<
        boost::beast::ssl_stream<boost::beast::tcp_stream>>;

    // The connection pipeline. Each step starts an async operation whose
    // completion handler is the next step.
    void connect();
    void on_resolve(boost::beast::error_code ec,
                    boost::asio::ip::tcp::resolver::results_type results);
    void on_tcp_connect(boost::beast::error_code ec);
    void on_tls_handshake(boost::beast::error_code ec);
    void on_ws_handshake(boost::beast::error_code ec);
    void read_next();
    void on_read(boost::beast::error_code ec, std::size_t bytes);
    void on_close(boost::beast::error_code ec);

    // Logs the failure and schedules a reconnect (unless we are stopping).
    void fail(boost::beast::error_code ec, std::string_view step);
    void schedule_reconnect();

    boost::asio::io_context& ioc_;
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
};

}  // namespace tickstream
