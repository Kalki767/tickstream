#Build Log

## 2026-08-13
I connected to binance trade page using a websocket to listen live trade exchanges and print them so I can visualize the volume and speed of data that I have.
I used to have vague understanding about websocket, asynchronous tasks and how the await works. But now I understood the concept behind those topics.

## 2026-09-24 — Week 1: C++ connect + parse
Did: C++17 client with Boost.Beast (async, single-threaded io_context) on Binance's combined stream (btcusdt, ethusdt, solusdt, bnbusdt). Parse into a `Trade` struct with nlohmann/json, log with spdlog. Reconnect with exponential backoff (1→2→4→8→16→30s cap, reset after a full handshake). Ctrl+C graceful shutdown; a second Ctrl+C force quits. 14 GoogleTest tests (parser, backoff). Builds clean with -Wall -Wextra -Wpedantic -Wshadow -Wconversion.
Broke / learned:
- SNI: Beast doesn't set it; added SSL_set_tlsext_host_name. Also added certificate hostname verification (a separate check from SNI).
- Shutdown took 10s. Measured with a separate Python client: Binance answered the close frame after 7.5s once and not at all the other two times (close code 1006). Fix: 2s close timeout.
- Binance drops TCP without a TLS close_notify ("stream truncated"). Harmless after the WebSocket close.
- Backoff checked by running with no network (`unshare -rn`): 1,2,4,8,16s as expected.
TODO: pull the network cable myself (idle timeout should detect it within 20s), 1-hour run.
