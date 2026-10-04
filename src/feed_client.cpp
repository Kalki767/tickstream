#include "tickstream/feed_client.hpp"

#include <chrono>
#include <utility>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/use_awaitable.hpp>
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

FeedClient::FeedClient(asio::any_io_executor executor,
                       ssl::context& ssl_ctx,
                       FeedConfig config,
                       MessageHandler on_message)
    : executor_(std::move(executor)),
      ssl_ctx_(ssl_ctx),
      config_(std::move(config)),
      host_header_(config_.host + ":" + config_.port),
      on_message_(std::move(on_message)),
      resolver_(executor_),
      reconnect_timer_(executor_) {}

// Error handling: every awaited operation uses use_awaitable, so a failure at
// any step throws boost::system::system_error. All failures are handled the
// same way (log, back off, reconnect), so one catch per attempt replaces an
// error check in every completion handler. `step` records how far we got so
// the log still says which stage failed.
asio::awaitable<void> FeedClient::run() {
    // stop() cancels whatever is in flight, but cancellation can't catch every
    // case: a completion that was already queued still arrives as a success,
    // and a range connect treats a cancelled endpoint as a failed one and moves
    // on to the next address. So after every handshake step, check again.
    const auto throw_if_stopping = [this] {
        if (stopping_) {
            throw boost::system::system_error(asio::error::operation_aborted);
        }
    };

    while (!stopping_) {
        std::string_view step = "resolve";
        try {
            // Fresh stream for each attempt; destroying the previous one closes its socket.
            ws_ = std::make_unique<WsStream>(executor_, ssl_ctx_);
            buffer_.clear();
            auto& tcp = beast::get_lowest_layer(*ws_);

            spdlog::info("connecting to {}{}", host_header_, config_.target);
            const auto endpoints =
                co_await resolver_.async_resolve(config_.host, config_.port, asio::use_awaitable);
            throw_if_stopping();

            // beast::tcp_stream supports per-operation timeouts; plain asio sockets don't.
            step = "tcp connect";
            tcp.expires_after(kConnectTimeout);
            co_await tcp.async_connect(endpoints, asio::use_awaitable);
            throw_if_stopping();

            step = "tls handshake";
            // SNI (Server Name Indication): tells the server which hostname we want
            // *before* it picks a certificate. Binance serves many hostnames from the
            // same IPs and rejects handshakes that don't say which one. Beast does not
            // set this for us.
            if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(),
                                          config_.host.c_str())) {
                throw boost::system::system_error(
                    static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category(),
                    "set SNI");
            }
            // Separate from SNI: check that the certificate the server sends is
            // actually valid for this hostname. Without this, any valid certificate
            // for any domain would be accepted (verify_peer alone only checks the chain).
            ws_->next_layer().set_verify_callback(ssl::host_name_verification(config_.host));
            tcp.expires_after(kConnectTimeout);
            co_await ws_->next_layer().async_handshake(ssl::stream_base::client,
                                                       asio::use_awaitable);
            throw_if_stopping();

            step = "websocket handshake";
            // From here on the websocket stream manages its own timeouts, so turn off
            // the tcp_stream one (otherwise it would fire in the middle of reading).
            tcp.expires_never();
            websocket::stream_base::timeout timeouts{};
            timeouts.handshake_timeout = kConnectTimeout;  // also bounds async_close
            timeouts.idle_timeout = kIdleTimeout;
            timeouts.keep_alive_pings = true;
            ws_->set_option(timeouts);
            ws_->set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
                req.set(beast::http::field::user_agent, "tickstream/0.2");
            }));
            ws_->read_message_max(kMaxMessageBytes);
            co_await ws_->async_handshake(host_header_, config_.target, asio::use_awaitable);
            throw_if_stopping();

            spdlog::info("connected");
            // Reset only after a *full* successful handshake. Resetting on TCP connect
            // would let a server that accepts TCP but fails TLS keep us retrying at 1s.
            backoff_.reset();

            step = "read";
            for (;;) {
                co_await ws_->async_read(buffer_, asio::use_awaitable);
                // flat_buffer keeps the message in one contiguous block, so we can
                // view it as a string_view without copying.
                on_message_(std::string_view(static_cast<const char*>(buffer_.data().data()),
                                             buffer_.size()));
                buffer_.consume(buffer_.size());
            }
        } catch (const boost::system::system_error& e) {
            if (stopping_) {
                break;  // expected: stop() closed or cancelled the stream
            }
            if (e.code() == websocket::error::closed) {
                spdlog::warn("server closed the connection (code {}, reason '{}')",
                             static_cast<int>(ws_->reason().code),
                             std::string(ws_->reason().reason));
            }
            spdlog::warn("{} failed: {}", step, e.code().message());
        }

        ++reconnects_;
        const auto delay = backoff_.next_delay();
        spdlog::info("reconnecting in {}s", delay.count());
        reconnect_timer_.expires_after(delay);
        // as_tuple: stop() cancelling this wait is an expected outcome, not a
        // failure, so take the error code instead of an exception. The loop
        // condition then sees stopping_.
        co_await reconnect_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
    }

    // If stop() interrupted a handshake, nothing uses the stream any more, but
    // the websocket's internal handshake timer is still armed and would keep
    // io_context::run() alive until it fires (measured: up to 10 s after
    // Ctrl+C). Destroying the stream cancels it. A graceful close still owns
    // the stream, so leave it alone in that case.
    if (!close_started_) {
        ws_.reset();
    }
}

// Cancellation strategy: explicit, not cancellation slots. When connected we
// want to *send* a close frame, which is an action rather than a cancel, and
// Beast leaves a websocket stream unusable after a cancelled read. So stop()
// starts the close; the pending async_read then completes with an error and
// run() exits through its catch because stopping_ is set.
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
        close_started_ = true;
        asio::co_spawn(executor_, close_gracefully(), asio::detached);
    } else {
        // Mid-handshake: nothing to close politely, so cancel the socket
        // operation. run() sees operation_aborted and, because stopping_ is
        // set, does not reconnect.
        beast::get_lowest_layer(*ws_).cancel();
    }
}

asio::awaitable<void> FeedClient::close_gracefully() {
    // Binance often doesn't answer the close frame at all. The close is
    // bounded by handshake_timeout, so shorten it here: waiting 10s to exit
    // buys nothing, since there's no data left for us to receive.
    websocket::stream_base::timeout timeouts{};
    ws_->get_option(timeouts);
    timeouts.handshake_timeout = kCloseTimeout;
    ws_->set_option(timeouts);

    // as_tuple: every outcome here is informational; we are exiting regardless.
    const auto [ec] = co_await ws_->async_close(websocket::close_code::normal,
                                                asio::as_tuple(asio::use_awaitable));
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
        // which phase of the close it was in. Either way we're exiting.
        spdlog::info("closed without server acknowledgement ({})", ec.message());
    }
}

}  // namespace tickstream
