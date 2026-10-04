# Build Log

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

## 2026-10-03 — Phase 0: setup and week-1 fixes
Did: `cmake_minimum_required` 3.24 → 3.25 (`FetchContent_Declare(... SYSTEM)` needs 3.25), fixed this file's heading. Clean build with 0 warnings, 14/14 tests (`bench/results/phase0-tests.txt`). Machine specs in `bench/results/phase0-machine-specs.txt`. Tagged `v0.1-week1-callbacks`.
Baseline of the callback client (`bench/results/phase0-callback-baseline-size.txt`): 342 lines (245 .cpp + 97 .hpp), 5 `if (ec)` failure checks, 6 `return fail(...)` call sites, 12 member functions.
Environment note: no sudo on this machine, so Boost 1.83, libpqxx 7.8.1, libpq-dev and Valgrind 3.22 are the official Ubuntu 24.04 .debs extracted to a user prefix (`apt-get download` + `dpkg -x`) and passed with `-DCMAKE_PREFIX_PATH`. Same versions `apt install` would give.

## 2026-10-03 — Phase 1: coroutine rewrite
Did: `FeedClient::run()` is an `asio::awaitable<void>` started with `co_spawn`. One loop: resolve → TCP → TLS → WebSocket → read, one `catch` per attempt, then the backoff wait.
Decisions:
- **Error style: exceptions (`use_awaitable`).** Every failure gets the same treatment (log, back off, reconnect), so one catch replaces a check in every handler. A `step` variable keeps the "which stage failed" log. `as_tuple` only where the error code is an expected outcome, not a failure: the backoff timer cancelled by `stop()`, and the close-frame result.
- **Cancellation: explicit, not cancellation slots.** Connected → `stop()` must *send* a close frame (an action, not a cancel), and Beast leaves a websocket stream unusable after a cancelled read. Mid-handshake → cancel the socket/resolver/timer.
- **Jitter: not added.** It would break the must-preserve "1, 2, 4, 8, 16 s" sequence; the plan marks it optional.

Must-preserve checks:

| Behaviour | Result | Evidence |
|---|---|---|
| SNI set before TLS handshake | ✅ connects to Binance | `SSL_set_tlsext_host_name` before `async_handshake`; live runs |
| Certificate hostname verification | ✅ code review + connects | `set_verify_callback(host_name_verification(host))` |
| 10 s timeout per handshake step | ✅ code review | `tcp.expires_after(10s)` before TCP and TLS; websocket `handshake_timeout = 10s` |
| 20 s idle timeout + keep-alive pings | ✅ code review; live test in Phase 5 | `idle_timeout = 20s`, `keep_alive_pings = true` |
| 64 KiB message limit | ✅ code review | `read_message_max(64 * 1024)` |
| Backoff resets only after full WS handshake | ✅ code review | `backoff_.reset()` is the line after `async_handshake` |
| Fresh stream per attempt | ✅ code review | `ws_ = make_unique<WsStream>` at the top of every loop iteration |
| Backoff 1, 2, 4, 8, 16 s | ✅ | `phase1-backoff-unshare.txt`: 1, 2, 4, 8, 16, then 30 s cap |
| Ctrl+C exits within ~2 s (connected) | ✅ 2.011 / 0.679 / 0.823 s, median 0.823 s | `phase1-shutdown-time.txt` (2.011 s = the 2 s close timeout when Binance didn't answer) |
| Second Ctrl+C force-quits | ✅ exit 130, 0.211 s after first SIGINT | `phase1-shutdown-time.txt` |

Size (`phase1-coroutine-size.txt`): 342 → 322 lines (220 → 194 non-blank, non-comment), 12 → 3 member functions, 5 `if (ec)` checks → 1 `catch`. The line saving is modest; the real change is that control flow reads top to bottom.

Bug found: **Ctrl+C during connect could hang forever, or take ~10 s.** Pre-existing in week 1.
- Symptom: testing Ctrl+C *before* "connected" (the plan's check only covers the connected case) hung. `bench/shutdown_race_test.sh` sends SIGINT at 12 delays from 0.05 s to 3 s. Week-1 callback build: 4/12 hangs, plus exits taking 9.2 s and 9.8 s. Coroutine rewrite: 5/12 hangs (`phase1-shutdown-race-before-fix.txt`).
- Diagnosis 1 (hang), from per-step debug logs: SIGINT arrived during TCP connect. `stop()` cancelled the socket, but the connect still succeeded and the client went on to read forever. Two reasons: (a) `cancel()` can't take back a completion that is already queued, so it arrives as a success; (b) Asio's *range* `async_connect` treats a cancelled endpoint as a failed one and tries the next address (Binance resolves to several). Nothing re-checked `stopping_` after a step succeeded.
- Fix 1: re-check `stopping_` after every handshake `co_await`. After it: 0/36 hangs, but 6/36 runs still took 9.3–9.9 s (`phase1-shutdown-race-after-fix.txt`).
- Diagnosis 2 (slow exit): in the logs, `run()` caught `Operation canceled` immediately during the WebSocket handshake, yet the process exited exactly 10.000 s after that handshake began. The coroutine was done; Beast's internal websocket handshake-timeout timer was still armed and kept `io_context::run()` alive. `ws_` wasn't destroyed until after `run()` returned.
- Fix 2: when `run()` exits and no graceful close owns the stream, destroy `ws_`, which cancels its timer. After it: 0/36 hangs, slowest exit 1.408 s (`phase1-shutdown-race-after-fix2.txt`).

## 2026-10-03 — Phase 2: instrumentation, capture, gap detection
Did: `received_steady_ns` / `received_wall_ms` on `Trade` (stamped in the handler before parsing), `--record FILE`, `GapDetector` (6 tests), CLI options module (tests), and a stats line every 10 s instead of a log line per trade.
Capture: `bench/capture.sh` → 15 min, **36,910 trades** (`bench/data/live_capture.jsonl`, gitignored; first 1,000 lines committed as `sample_capture.jsonl`).
Live demand (`phase2-live-rate.txt`, 1-s buckets by exchange trade time `T`): **mean 41.06 trades/s, median 9, p99 599, peak 808 trades/s**. The feed is very bursty. The mean is a slight underestimate because of the 2,253 trades lost in the reconnect below.

Found: **a real reconnect during the capture, caught by the gap detector.** At 20:51:18 the server closed TCP ("read failed: End of file"). The client reconnected after the 1 s backoff (connected 20:51:21.601) and logged 4 gaps, **2,253 missing trades** (BNB 158, BTC 473, SOL 279, ETH 1,343).
- 2,253 trades is far more than ~3 s of outage at ~86 trades/s would explain. Using exchange time T, the last trade received before the EOF was ~43–45 s older than the first one after it (`phase2-capture-reconnect-analysis.txt`).
- Cause: **in bursts, the feed delivered trades late, and the lag kept growing.** Comparing each stats line's running total with the T of that message, the approximate lag climbed 6.6 → 14.9 → 23.8 → 30.0 → 37.9 s in the minute before the EOF, then dropped back under 1 s after the reconnect. The backlog was never delivered: a new connection starts at "now".
- Is it my client? **No.** `phase2-feed-lag-diagnosis.txt`: a 4-minute run with `ss -tin` every 5 s. Lag reached 16.4 s while **Recv-Q was 0 in all 48 samples**, i.e. every byte the kernel received had already been read by the client. RTT to the server (AWS Tokyo) was 295–366 ms. The delay is upstream (network path or Binance's own pacing; I can't tell which from here).
- Consequences: (1) exchange→receive time on this network says nothing about this code, which is why the headline latency is receive→commit on one steady clock; (2) on this connection, at-most-once delivery loses real data during bursts, so the soak will show gaps that aren't bugs; (3) REST backfill of detected gaps is the obvious next feature (the REST API answers from here: `phase2-rest-api-reachability.txt`).

## 2026-10-03 — Phase 3: queue, writer thread, naive writer
Did: `BoundedQueue` (mutex + 2 condvars over a preallocated ring buffer), writer `std::thread` running `run_writer()` behind a `TradeSink` interface (tested with fake sinks, no DB), `PgWriter` naive mode, `--replay` / `--replay-copies`, `sql/schema.sql`, `bench/verify.sh`.
Backpressure decision:
- **Live: drop the newest trade and count it.** Blocking would stall the event loop, meaning no reads, no pings and no Ctrl+C, and Binance would eventually drop us as a slow consumer, losing more than one trade. Drops show in the stats line.
- **Replay/benchmark: block.** Otherwise a slow writer would "go faster" by dropping rows.
- **Capacity 65,536 trades.** `sizeof(Trade)` = 136 bytes, so 8.9 MB preallocated (`phase3-trade-size.txt`). At the measured 808 trades/s exchange peak, that covers 81 s of a completely stalled writer, longer than a Postgres restart. A ring buffer rather than `std::deque`: fixed memory known at startup, no allocation per push.
- `--writer null` (discard) doubles as capture-only mode and a no-database ceiling.
Shutdown order: stop source → close queue → writer drains and commits the rest → join → exit (unit test `DrainsEverythingQueuedBeforeClose`).
Verify (`phase3-verify-naive.txt`): replaying the full capture through the naive writer gives **36,910 rows, and sum(price) and sum(quantity) match the file's exact Decimal sums**, so prices and quantities survive string → NUMERIC unrounded. Live smoke test (`phase3-live-naive-smoke.txt`): 763 rows in 30 s, 0 drops; per symbol, row count = max id − min id + 1.
First sign of the bottleneck: that verify replay ran at 485 trades/s (one run, not the benchmark), **below the 808 trades/s live peak**.
Build issue: Ubuntu's libpqxx 7.8.1 is compiled as C++17; under C++20 its headers select `std::source_location` overloads the library doesn't contain, so linking fails as soon as `pqxx::params` or `stream_to` are used. Fixed in Phase 4 by building libpqxx 7.8.1 from source via FetchContent (libpqxx must be built with the same C++ standard as its user).

## 2026-10-03 — Phase 4: batching and the headline benchmark
Did: `txn`, `multirow` and `copy` writers; `FlushPolicy` (size or time, pure, tested with an injected `now`); a batching writer loop; `bench/writer_bench.sh` (12 configurations × 3 runs, every run's table verified). Full table in BENCHMARKS.md.
Results (medians, 221,460 trades per run): naive **200** trades/s → multirow-1000 **37,220** (186×) → copy-5000 **45,050** (225×). Naive's three runs were 471 / 200 / 198, so against naive's *best* run copy-5000 is 95.6×. Quote the median and know the range.
Where the time goes: `synchronous_commit=off` alone gave naive 58.7× (11,746/s), so the per-commit WAL flush dominates, not the SQL or the round trips. The SSD does 710 `fdatasync`/s (`pg_test_fsync`), which caps any commit-per-row writer. `txn` (same INSERTs, one commit per 100) gave 18.5×; removing round trips (multirow/copy) gave the rest.
COPY vs idempotency: COPY has no `ON CONFLICT`, so the copy writer COPYs into an `ON COMMIT DELETE ROWS` temp table and then runs `INSERT … SELECT … ON CONFLICT DO NOTHING`, writing every row twice. That cost shows: multirow beat copy at batch 10, 100 and 1,000; copy only won at 5,000. Dropping idempotency (COPY straight into `trades`) would be faster but would turn a replay or reconnect overlap into a primary-key error.
Default chosen: **multirow, 1,000 rows, 200 ms.** Live batches are small (time-triggered: about 8 rows at the mean rate, about 160 at the peak), and multirow is the faster writer at small batches. 46× headroom over the 808/s live peak.
Trade-offs of batching: a trade can wait up to `flush-ms` before its commit starts (latency, measured below), and a crash loses the unflushed batch plus whatever is in the queue.
Not done: re-running the sweep on AC power (the first part ran on battery). Skipped by decision; naive's spread is reported as it is.
Latency (live, 5 min each, receive→commit on the steady clock): naive p50 56.0 / p99 1,009.0 / max 2,157.8 ms with the queue reaching 636 in a burst; multirow-1000-200ms p50 202.8 / p99 240.5 / max 246.2 ms. Batching costs ~flush-ms at the median but cuts p99 4.2×, because naive falls behind in bursts. The runs saw different live traffic.

## 2026-10-03/04 — Phase 5: memory and thread safety
Builds: `-DTICKSTREAM_SANITIZE=address,undefined` (`build-asan`) and `=thread` (`build-tsan`), applied before the dependencies are fetched so libpqxx/spdlog/gtest are instrumented too. `-fno-sanitize-recover=all`, so a UBSan finding aborts.
Results, all clean:
- Unit tests: 82/82 under ASan+UBSan, 82/82 under TSan; the 16 queue/pipeline tests × 20 repeats under TSan; 0 TSan reports (`phase5-asan-ubsan-tests.txt`, `phase5-tsan-tests.txt`).
- Positive controls first (`phase5-sanitizer-canaries.txt`): a planted data race, use-after-free and signed overflow are each caught with the same flags. A clean result only means something if the tool can fire.
- Full-capture replay ×2 through the real writer: 73,820 rows verified, exit 0, no reports, under both (`phase5-asan-replay.txt`, `phase5-tsan-replay.txt`).
- Valgrind memcheck, normal build, 1,000-message replay: 0 errors, 40,561 allocs = 40,561 frees, 0 bytes in use at exit, no suppressions (`phase5-valgrind-memcheck.txt`).
- TSan live, 15 min: 12,168 trades = 12,168 rows, 0 drops, 0 reports (`phase5-tsan-live.txt`).
Problems hit (symptom → diagnosis → fix):
- **TSan build failed:** `FATAL: ThreadSanitizer: unexpected memory mapping` when CMake ran the test binary to discover tests. This is a known incompatibility between GCC 13's TSan runtime and the larger mmap ASLR randomisation of recent kernels (I couldn't read `vm.mmap_rnd_bits` without root, so this rests on the known issue plus the fix working). Fix: run TSan binaries with ASLR off for that process, `setarch -R`, no root needed, wired in through `CROSSCOMPILING_EMULATOR` so ctest and test discovery use it (`phase5-tsan-aslr.txt` shows the failure and the fix). Also `-Wno-tsan` for TSan builds only: Asio's `std_fenced_block` uses `atomic_thread_fence`, which TSan can't model. Asio runs on one thread here; the cross-thread queue uses a mutex, which TSan does model.
- **My UBSan control printed nothing:** I'd written `abs(x + 1) > 0`, which GCC rewrote at -O1 so the overflowing add was never executed. At -O0 UBSan caught it. Rewrote the control to use the overflowed value; caught at -O2.
- **My TSan live-run counter said "1 warning":** the grep matched its own header line in the output file. There were 0 real reports; I now count lines starting `WARNING: ThreadSanitizer:`. Both affected files are annotated.
- **The first live runs were invalidated by the laptop suspending overnight.** By accident this tested something: after a ~10 h freeze, the TSan build detected the dead connection by idle timeout on resume, reconnected after the 1 s backoff (1.36 s handshake) and reported 4 gaps totalling 555,713 missed trades (`phase5-suspend-resume-incidental.txt`). The runs were then repeated under `systemd-inhibit` (no sleep, no lid-switch suspend).
- **Network-drop test (closes the week-1 TODO), ASan+UBSan build, 30 min live** (`phase5-netdrop-timeline.txt`, `phase5-netdrop.log`). The client ran in a container; at minute 10 iptables dropped all TCP to and from port 9443 *inside the container's own network namespace*. That means no FIN and no RST, like a pulled cable, and the host firewall was untouched. Rules removed after 60.243 s.
  - **Detected after 19.978 s** by the websocket idle timeout (20 s; keep-alive pings get no pong).
  - Reconnect attempts after 1, 2 and 4 s backoff each failed at the 10 s TCP connect timeout; then an 8 s wait.
  - **Reconnected 6.193 s after the network came back**; total outage 66.436 s.
  - The gap detector reported **4 gaps, 953 missing trades**, one per symbol, with exact id ranges.
  - 31,392 trades received = 31,392 rows in the database; 0 drops; container exit 0; no AddressSanitizer, UBSan or LeakSanitizer reports.
  - Why INPUT too: the plan's command drops only OUTPUT. With only that, the server's data keeps arriving until its unacknowledged window fills, so it isn't a clean pulled-cable test.

## 2026-10-04 — Phase 6: hot path
- Profile (callgrind, since `perf` is blocked by `perf_event_paranoid=4` without root): the DOM JSON parse is 76.4% of the client's instructions; the writer thread is 14.7%.
- Google Benchmark with a counting `operator new` (bench binary only): DOM parse 4,507 ns / 31 allocations per message; nlohmann SAX parse 2,408 ns / 7. **SAX becomes the default** after passing every parser test and the 1,000-message differential test. The DOM parser stays as the reference implementation.
- Copy vs move into the queue: 142 vs 150 ns, 0 allocations each → no measurable effect, because every real string fits in SSO (≤ 14 chars vs 15). A 24-char control shows the difference appears only above SSO (copy +3 allocations). The plan predicted this; recorded rather than claimed.
- End to end: the null-writer ceiling went 119,357 → 191,876 trades/s (1.61×); the multirow-1000 writer 19,965 → 20,556 (within noise). Writes are Postgres-bound, so the parser win is a CPU/allocation win, not a throughput win.
- Today's ~20k/s for multirow-1000 vs 37,220 in yesterday's sweep: different conditions (battery; a 10M-row load had just run on the same server). Comparisons are only made within back-to-back runs.
