# tickstream

Real-time market data ingestion service in C++17. Connects to Binance's live
trade feed over a TLS WebSocket, parses each trade, and (from week 2) persists
them to PostgreSQL.

**Status:** week 1: connect, parse, reconnect, graceful shutdown.

## Build

```bash
sudo apt install build-essential cmake git libboost-all-dev libssl-dev
cmake -S . -B build
cmake --build build -j
```

nlohmann/json, spdlog and GoogleTest are fetched by CMake at configure time.

## Run

```bash
./build/tickstream                      # btcusdt ethusdt solusdt bnbusdt
./build/tickstream btcusdt dogeusdt     # any symbols
```

Ctrl+C shuts down gracefully (sends a WebSocket close frame). Press it twice to force quit.

## Test

```bash
ctest --test-dir build --output-on-failure
```

## Layout

| Path | What |
|---|---|
| `include/tickstream/trade.hpp` | `Trade` struct |
| `src/binance.cpp` | JSON → `Trade`, stream URL building (pure, unit-tested) |
| `src/backoff.cpp` | Exponential backoff 1s→30s (pure, unit-tested) |
| `src/feed_client.cpp` | Boost.Beast TLS WebSocket client with reconnect |
| `src/main.cpp` | Wiring, signal handling |
