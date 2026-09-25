#include "tickstream/feed_client.hpp"

#include <chrono>
#include <utility>

#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <spdlog/spdlog.h>

namespace tickstream {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = boost::asio::ssl;
namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;

namespace {

// Time allowed for each step of the handshake before we give up on it.
constexpr auto kConnectTimeout = std::chrono::seconds{10};

// How long stop() waits for the server to acknowledge our close frame.
constexpr auto kCloseTimeout = std::chrono::seconds{2};

// If nothing arrives for this long the connection is considered dead.
// Beast sends a WebSocket ping halfway through, so a healthy-but-quiet
// connection answers with a pong and never trips this. A pulled network cable
// sends no FIN/RST, so without this timeout the read would wait forever.
constexpr auto kIdleTimeout = std::chrono::seconds{20};

// A trade message is ~250 bytes. Refuse anything absurdly large instead of
// buffering it (Beast's default limit is 16 MiB).
constexpr std::size_t kMaxMessageBytes = 64 * 1024;

}  // namespace

FeedClient::FeedClient(asio::io_context& ioc,
                       ssl::context& ssl_ctx,
                       FeedConfig config,
                       MessageHandler on_message)
    : ioc_(ioc),
      ssl_ctx_(ssl_ctx),
      config_(std::move(config)),
      host_header_(config_.host + ":" + config_.port),
      on_message_(std::move(on_message)),
      resolver_(ioc),
      reconnect_timer_(ioc) {}

void FeedClient::start() {
    connect();
}

void FeedClient::stop() {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    spdlog::info("stopping feed client");

    reconnect_timer_.cancel();  // if we're waiting to reconnect, don't
    resolver_.cancel();         // if DNS lookup is in flight, abort it

    if (!ws_) {
        return;
    }
    if (ws_->is_open()) {
        // Polite shutdown: send a close frame and wait for the server's reply.
        // The outstanding async_read completes with websocket::error::closed.
        //
        // Binance often doesn't answer the close frame at all. The close is
        // bounded by handshake_timeout, so shorten it here: waiting 10s to
        // exit buys nothing, since there's no data left for us to receive.
        websocket::stream_base::timeout timeouts{};
        ws_->get_option(timeouts);
        timeouts.handshake_timeout = kCloseTimeout;
        ws_->set_option(timeouts);

        ws_->async_close(websocket::close_code::normal,
                         [this](beast::error_code ec) { on_close(ec); });
    } else {
        // Mid-handshake: nothing to close politely, so cancel the socket
        // operation. Its handler sees operation_aborted and, because
        // stopping_ is set, does not reconnect.
        beast::get_lowest_layer(*ws_).cancel();
    }
}

void FeedClient::connect() {
    // Fresh stream for each attempt; destroying the previous one closes its socket.
    ws_ = std::make_unique<WsStream>(ioc_, ssl_ctx_);
    buffer_.clear();

    spdlog::info("connecting to {}{}", host_header_, config_.target);
    resolver_.async_resolve(
        config_.host, config_.port,
        [this](beast::error_code ec, tcp::resolver::results_type results) {
            on_resolve(ec, std::move(results));
        });
}

void FeedClient::on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        return fail(ec, "resolve");
    }

    // beast::tcp_stream supports per-operation timeouts; plain asio sockets don't.
    beast::get_lowest_layer(*ws_).expires_after(kConnectTimeout);
    beast::get_lowest_layer(*ws_).async_connect(
        results, [this](beast::error_code ec2, const tcp::endpoint&) { on_tcp_connect(ec2); });
}

void FeedClient::on_tcp_connect(beast::error_code ec) {
    if (ec) {
        return fail(ec, "tcp connect");
    }

    // SNI (Server Name Indication): tells the server which hostname we want
    // *before* it picks a certificate. Binance serves many hostnames from the
    // same IPs and rejects handshakes that don't say which one. Beast does not
    // set this for us.
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), config_.host.c_str())) {
        ec.assign(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category());
        return fail(ec, "set SNI");
    }

    // Separate from SNI: check that the certificate the server sends is
    // actually valid for this hostname. Without this, any valid certificate
    // for any domain would be accepted (verify_peer alone only checks the chain).
    ws_->next_layer().set_verify_callback(ssl::host_name_verification(config_.host));

    beast::get_lowest_layer(*ws_).expires_after(kConnectTimeout);
    ws_->next_layer().async_handshake(
        ssl::stream_base::client, [this](beast::error_code ec2) { on_tls_handshake(ec2); });
}

void FeedClient::on_tls_handshake(beast::error_code ec) {
    if (ec) {
        return fail(ec, "tls handshake");
    }

    // From here on the websocket stream manages its own timeouts, so turn off
    // the tcp_stream one (otherwise it would fire in the middle of reading).
    beast::get_lowest_layer(*ws_).expires_never();

    websocket::stream_base::timeout timeouts{};
    timeouts.handshake_timeout = kConnectTimeout;  // also bounds async_close
    timeouts.idle_timeout = kIdleTimeout;
    timeouts.keep_alive_pings = true;
    ws_->set_option(timeouts);

    ws_->set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(beast::http::field::user_agent, "tickstream/0.1");
    }));

    ws_->read_message_max(kMaxMessageBytes);

    ws_->async_handshake(host_header_, config_.target,
                         [this](beast::error_code ec2) { on_ws_handshake(ec2); });
}

void FeedClient::on_ws_handshake(beast::error_code ec) {
    if (ec) {
        return fail(ec, "websocket handshake");
    }

    spdlog::info("connected");
    // Reset only after a *full* successful handshake. Resetting on TCP connect
    // would let a server that accepts TCP but fails TLS keep us retrying at 1s.
    backoff_.reset();
    read_next();
}

void FeedClient::read_next() {
    ws_->async_read(buffer_, [this](beast::error_code ec, std::size_t bytes) {
        on_read(ec, bytes);
    });
}

void FeedClient::on_read(beast::error_code ec, std::size_t /*bytes*/) {
    if (ec) {
        if (ec == websocket::error::closed) {
            spdlog::warn("server closed the connection (code {}, reason '{}')",
                         static_cast<int>(ws_->reason().code),
                         std::string(ws_->reason().reason));
        }
        return fail(ec, "read");
    }

    // flat_buffer keeps the message in one contiguous block, so we can view it
    // as a string_view without copying.
    const std::string_view message(static_cast<const char*>(buffer_.data().data()),
                                   buffer_.size());
    on_message_(message);
    buffer_.consume(buffer_.size());

    read_next();
}

void FeedClient::on_close(beast::error_code ec) {
    if (!ec) {
        spdlog::info("connection closed cleanly");
    } else if (ec == ssl::error::stream_truncated) {
        // The WebSocket close handshake completed, but the server then dropped
        // TCP without a TLS close_notify. Many servers (Binance included) do
        // this; no data is at risk because the WebSocket layer already agreed
        // to close, so this isn't an error for us.
        spdlog::info("connection closed (server skipped TLS close_notify)");
    } else {
        // Measured: Binance often never answers the close frame, and when it
        // does it can take 7+ seconds. When kCloseTimeout fires, Beast closes
        // the socket and reports "timeout" or "operation canceled" depending on
        // which phase of the close it was in. Either way we're exiting, and
        // there's nothing to retry, so this is informational, not a warning.
        spdlog::info("closed without server acknowledgement ({})", ec.message());
    }
}

void FeedClient::fail(beast::error_code ec, std::string_view step) {
    if (stopping_) {
        // Expected: stop() cancelled or closed whatever was running.
        return;
    }
    spdlog::warn("{} failed: {}", step, ec.message());
    schedule_reconnect();
}

void FeedClient::schedule_reconnect() {
    const auto delay = backoff_.next_delay();
    spdlog::info("reconnecting in {}s", delay.count());

    reconnect_timer_.expires_after(delay);
    reconnect_timer_.async_wait([this](beast::error_code ec) {
        if (ec || stopping_) {
            return;  // cancelled by stop()
        }
        connect();
    });
}

}  // namespace tickstream
