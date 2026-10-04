# tickstream

A real-time market data ingestion service in C++20. It streams live trades from Binance over a
TLS WebSocket and writes them to PostgreSQL through a bounded producer–consumer queue and a
batching writer thread. Lost data during reconnects is detected and counted.

Every number in this README was measured in this repository; the raw output for each is in
[`bench/results/`](bench/results/), and the method and caveats are in
[BENCHMARKS.md](BENCHMARKS.md). The engineering log, with decisions and bug write-ups, is
[Log.md](Log.md).

## Architecture

```
                 network thread (Boost.Asio io_context)                    writer thread
 ┌───────────────────────────────────────────────────────────┐        ┌────────────────────────┐
 │ FeedClient::run()  C++20 coroutine                        │        │ run_writer()           │
 │   resolve → TCP → TLS (SNI + hostname check) → WebSocket  │        │   batch until N rows   │
 │   → read loop; any failure → backoff 1,2,4..30 s → retry  │        │   or M ms (FlushPolicy)│
 │                                                           │        │ PgWriter (libpqxx)     │
 │ on each message: stamp time → parse (SAX) → GapDetector   │ Bounded│   multirow INSERT …    │──► PostgreSQL
 │                  → queue.try_push (drop + count if full)  ├─Queue─►│   ON CONFLICT DO NOTHING│
 └───────────────────────────────────────────────────────────┘ 65,536 └────────────────────────┘
   ReplaySource (benchmarks): reads a capture, same parse,     slots
   queue.push (blocks when full, so nothing is dropped)
```

- **Two sources, one pipeline.** Live mode and replay mode share the parse, queue and writer
  code, so replay benchmarks measure the real system.
- **Backpressure.** Live mode never blocks the network thread: a full queue drops the newest trade
  and counts it. Blocking would stall reads, pings and Ctrl+C, and Binance would eventually
  disconnect us. Replay blocks instead, so a slow writer can't look fast by dropping rows.
- **Idempotent writes.** The key is `(symbol, trade_id)` and every insert is
  `ON CONFLICT DO NOTHING`, so replays and reconnect overlaps can't duplicate rows.
- **Shutdown order.** Stop the source → close the queue → the writer drains and commits the rest →
  join → exit.

## Results

| | Result | Source |
|---|---|---|
| Write capacity | per-row autocommit **200** → multi-row batches **37,220 trades/s** (186×); COPY ×5,000 45,050/s | [Phase 4](BENCHMARKS.md#phase-4-write-capacity-replay-benchmark) |
| Where the time went | `synchronous_commit=off` alone: 58.7×, so the per-commit WAL flush dominates; the SSD does 710 `fdatasync`/s | same |
| Live demand | mean 41.06, peak **808 trades/s** (4 symbols, 15-min capture); capacity is 46× the peak | `phase2-live-rate.txt` |
| Live latency, receive → commit | p99 **1,009 ms → 240 ms** (per-row vs batched); p50 56 → 203 ms (batching's cost) | [Phase 4 latency](BENCHMARKS.md#phase-4-live-latency-receive--commit) |
| JSON parsing | DOM → SAX: **4,507 → 2,408 ns** and **31 → 7 heap allocations** per message | [Phase 6](BENCHMARKS.md#phase-6-hot-path) |
| Move semantics | no measurable effect: every real string fits in SSO (control with 24-char strings shows where it would matter) | same |
| Pulled-cable test | silent drop detected in **19.98 s**, reconnected 6.19 s after restore, **953 missed trades reported** as gaps | `phase5-netdrop-timeline.txt` |
| Memory/thread safety | ASan, UBSan, TSan: 82/82 tests, replays and live runs with 0 reports; Valgrind 0 errors, all 40,561 allocations freed | `phase5-*.txt` |
| Index | one-symbol VWAP over 10M rows: **637 → 55 ms** with `(symbol, event_time)`; all-symbols query: no gain (explained) | [Week 4](BENCHMARKS.md#week-4-vwap-query-and-a-composite-index) |
| Image | build stage 1.18 GB → runtime **139 MB** | `week4-docker-image-sizes.txt` |

Measured on a 2-core/4-thread i5-7200U laptop with a SATA SSD, Postgres on the same machine,
partly on battery. Absolute rates are specific to that machine; see BENCHMARKS.md for conditions.

## Build and test

```bash
sudo apt install build-essential cmake git libboost-dev libssl-dev libpq-dev
cmake -S . -B build                 # RelWithDebInfo by default
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Fetched by CMake: nlohmann/json, spdlog, libpqxx 7.8.1, GoogleTest, Google Benchmark.
libpqxx is built from source on purpose: Ubuntu's package is compiled as C++17 and doesn't link
with C++20 code. `-DTICKSTREAM_BUILD_TESTS=OFF -DTICKSTREAM_BUILD_BENCH=OFF` builds only the
service.

Sanitizer builds (separate directories; never benchmark these):

```bash
cmake -S . -B build-asan -DTICKSTREAM_SANITIZE=address,undefined && cmake --build build-asan -j
cmake -S . -B build-tsan -DTICKSTREAM_SANITIZE=thread && cmake --build build-tsan -j
ctest --test-dir build-tsan   # TSan binaries run under `setarch -R` (GCC 13 TSan vs recent kernels)
```

## Run

```bash
psql -d tickstream -f sql/schema.sql
./build/tickstream                                  # live: btcusdt ethusdt solusdt bnbusdt
./build/tickstream --dsn "dbname=tickstream" btcusdt dogeusdt
./build/tickstream --help                           # all options
```

Defaults: `--writer multirow --batch-size 1000 --flush-ms 200`, chosen from the benchmark. The
connection string comes from `--dsn`, else `$TICKSTREAM_DSN`. A stats line every 10 s shows the
rate per symbol, gaps, missing trades, reconnects, queue depth, drops and rows written. Ctrl+C
shuts down gracefully; press it twice to force quit.

With Docker:

```bash
echo "POSTGRES_PASSWORD=$(openssl rand -hex 16)" > .env
docker compose up --build
```

## Reproduce the measurements

| What | Command |
|---|---|
| Record live data | `bench/capture.sh 900` then `bench/live_rate.py bench/data/live_capture.jsonl` |
| Correctness of a writer | `bench/verify.sh bench/data/live_capture.jsonl 2 --writer copy --batch-size 1000` |
| Write-capacity sweep | `bench/writer_bench.sh` |
| Live latency | `bench/live_latency.sh multirow 300 --writer multirow --batch-size 1000 --flush-ms 200` |
| Micro-benchmarks | `./build/tickstream_bench --benchmark_repetitions=3 --benchmark_report_aggregates_only=true` |
| Shutdown timing / race | `bench/shutdown_test.sh` · `bench/shutdown_race_test.sh` |
| Pulled cable | `docker build -t tickstream-netdrop bench/netdrop && bench/netdrop.sh ./build-asan/tickstream` then `bench/netdrop_timeline.py bench/results/phase5-netdrop` |
| VWAP index | build the dataset as in `week4-vwap-dataset.txt`, then `bench/vwap_index.sh` |
| Soak | `bench/soak.sh 12` |

The full capture (`bench/data/live_capture.jsonl`, 4.6 MB) isn't committed; `bench/capture.sh`
regenerates one. The first 1,000 lines are committed as `sample_capture.jsonl`, which the tests use.

## Design decisions

- **Coroutines over callbacks.** The connection is one linear loop with one `catch` per attempt
  (12 member functions → 3, 5 per-handler error checks → 1). Same event loop and performance
  model. Exceptions for failures that end an attempt; `as_tuple` where an error is an expected
  outcome (a cancelled timer, the close result).
- **Explicit shutdown, not cancellation slots.** A connected client must *send* a close frame, and
  Beast leaves a stream unusable after a cancelled read. After every handshake step the
  coroutine re-checks the stop flag, because `cancel()` can't take back a completion that is
  already queued (this was a real hang; see Log.md, Phase 1).
- **Multi-row INSERT over COPY by default.** COPY can't skip conflicts, so idempotent COPY goes
  through a staging table and writes every row twice. Live batches are small (time-triggered),
  and at those sizes multi-row INSERT was faster.
- **Ring-buffer queue.** A fixed allocation at startup (65,536 × 136 B = 8.9 MB, 81 s of a stalled
  writer at peak), with no allocation per push.
- **Latency on one clock.** Receive → commit uses `steady_clock` in one process. Exchange →
  receive mixes two machines' clocks, and on this network the feed itself sometimes lagged by
  tens of seconds upstream of the client (Log.md, Phase 2).

## Limitations

- **At-most-once delivery.** Trades missed during a reconnect are detected and counted, but not
  recovered. REST backfill of the reported id ranges is the obvious next step.
- **Crash window.** A crash loses the open batch (at most `--flush-ms` of trades) and whatever is
  in the in-memory queue. There is no disk buffer.
- **Single node, single connection, single writer.** No horizontal scaling, no Kafka-style
  durable log between the feed and the database.
- **One machine for everything.** Postgres, client and benchmarks share a laptop, so absolute
  numbers will differ on server hardware.
- **Index:** the `(symbol, event_time)` index only helps queries with a symbol predicate.

## Layout

| Path | What |
|---|---|
| `src/feed_client.cpp` | Coroutine TLS WebSocket client: reconnect, idle timeout, graceful close |
| `src/binance.cpp` | JSON → `Trade` (SAX parser, DOM reference), stream URL building |
| `include/tickstream/bounded_queue.hpp` | Ring-buffer queue: drop-and-count or blocking push |
| `src/writer_loop.cpp`, `flush_policy.hpp` | Writer thread: size/time-triggered batching |
| `src/pg_writer.cpp`, `src/sql.cpp` | naive / txn / multirow / copy writers |
| `src/gap_detector.cpp` | Per-symbol trade-id continuity |
| `src/replay.cpp`, `src/options.cpp` | Replay source, command line |
| `src/main.cpp` | Wiring: signals, stats line, shutdown order |
| `tests/` | 82 GoogleTest cases (parser tests run against both parsers) |
| `bench/` | Benchmark, verification and soak scripts; `bench/micro/` Google Benchmark |
| `sql/schema.sql` | Table definition |
