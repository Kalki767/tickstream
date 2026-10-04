# tickstream: final report

**Last updated: 2026-10-04, ~09:45.** Phases 0–7 and Week 4 are done. **Only the 12-hour soak
remains**, and it starts automatically once the laptop is on AC power (§5). It has not run yet,
so this report contains **no soak numbers**. Every number measured so far is listed in §7.

> **Git note:** the commit hashes and tags in this file refer to my local history. They're being
> undone (see `COMMIT_PLAN.md`) so you can make your own commits.

| Phase | State | Commit / tag |
|---|---|---|
| 0: setup, week-1 fixes | ✅ | `v0.1-week1-callbacks` |
| 1: coroutine rewrite + shutdown-race fix | ✅ | `v0.2-coroutines` |
| 2: capture, gap detection | ✅ | `03ea069` |
| 3: queue, writer thread, naive writer | ✅ | `v0.3-naive-writer` |
| 4: batching benchmark + live latency | ✅ | `v0.4-batched` |
| 5: ASan/UBSan/TSan, Valgrind, network drop | ✅ | `0d54def` |
| 6: profile, micro-benchmarks, SAX parser | ✅ | `3d39a34` |
| Week 4: VWAP index, Docker | ✅ | `26dfc0f` |
| 7: README, 12 h soak | 🟡 README done (`bc7fa8a`); soak waiting for AC power | |

**Rule followed throughout:** every number below came from a command run in this project, and
its raw output is saved under `bench/results/`. Nothing is estimated. Results that contradicted
the plan are reported as measured, with their caveats.

---

## 1. Resume bullets

Pick the 4 strongest. Each is followed by its source files and the caveat to know before an
interview.

> **tickstream: Real-Time Market Data Ingestion Service**
> *C++20 · Boost.Beast/Asio · PostgreSQL (libpqxx) · GoogleTest · Google Benchmark · Docker*

**1. Throughput (headline)**
> • Raised PostgreSQL write capacity from 200 to 37,220 trades/sec (186×; 46× headroom over the
> 808 trades/sec live peak) by replacing per-row autocommit with size/time-triggered multi-row
> batches, after isolating per-commit WAL flushes as the dominant cost.

`phase4-writer-summary.txt`, `phase4-ratios.txt`, `phase2-live-rate.txt`.
- **Caveat:** naive's 3 runs were 471 / 200 / 198. 200 is the median; against the best run the gain is 79×.
- "Isolating the WAL flush" = `synchronous_commit=off` alone gave 58.7×, and the SSD does 710 `fdatasync`/s.

**2. Latency**
> • Cut p99 receive-to-commit latency on live Binance traffic from 1,009 ms to 240 ms by batching
> writes; the per-row writer fell behind during bursts, while batching bounded the worst case to
> its 200 ms flush window.

`phase4-latency-*.txt`.
- **Caveat:** the median got worse (56 → 203 ms), which is batching's cost. The two runs saw different live traffic.

**3. Coroutine rewrite + bug**
> • Rewrote a callback-based Boost.Beast TLS WebSocket client as a single C++20 coroutine
> (12 → 3 member functions, 5 error checks → 1), and fixed a latent shutdown race found with a
> stage-by-stage test: Ctrl+C during connection setup hung in 4/12 runs before, 0/36 after.

`phase1-*.txt`.
- **Caveat:** lines only went 342 → 322. Claim control flow, not size.

**4. Reliability and safety**
> • Verified with ASan, UBSan, TSan and Valgrind (0 reports; all 40,561 allocations freed) and a
> pulled-cable test: a silent network drop was detected in 20 s by keep-alive idle timeout, the
> client reconnected 6 s after restore, and per-symbol trade-ID gap detection reported all 953
> missed trades.

`phase5-*.txt`, `phase5-netdrop-timeline.txt`.

**5. Parser**
> • Cut JSON parse cost from 4,507 to 2,408 ns and from 31 to 7 heap allocations per message by
> moving from DOM to SAX parsing, measured with Google Benchmark and a counting allocator, and
> verified identical output on 1,000 real captured messages.

`phase6-micro.txt`.
- **Caveat:** end-to-end write throughput didn't change (Postgres-bound); the no-database pipeline ceiling went up 1.61×. Say that if asked.

**6. Index**
> • Cut a per-minute VWAP query from 637 ms to 55 ms on 10M rows with a composite
> (symbol, event_time) index, verified via EXPLAIN ANALYZE.

`week4-vwap-index.txt`.
- **Caveat:** that's the one-symbol query. The all-symbols query got no gain (PG16 has no skip
  scan), and you should be able to explain why. The 10M rows are the real capture copied and
  time-shifted, not 10M live trades.

**Optional extra (soak, only after it finishes):** "sustained 12 hours of live ingestion with
flat memory…": fill in only from `phase7-soak.csv`.

---

## 2. All measured numbers

| Area | Result | File (`bench/results/`) |
|---|---|---|
| Coroutine rewrite | 342 → 322 lines; 12 → 3 member functions; 5 `if (ec)` → 1 `catch` | `phase1-coroutine-size.txt` |
| Shutdown (connected) | median 0.823 s (2.011 / 0.679 / 0.823); force quit 0.211 s | `phase1-shutdown-time.txt` |
| Ctrl+C-during-connect race | week-1 code 4/12 hung → 0/36 after fixes, slowest 1.408 s | `phase1-shutdown-race-*.txt` |
| Backoff | 1, 2, 4, 8, 16, 30 s | `phase1-backoff-unshare.txt` |
| Live demand | 36,910 trades / 899 s; mean 41.06/s; peak 808/s | `phase2-live-rate.txt` |
| Real disconnect | 4 gaps, 2,253 missing; feed lag up to 37.9 s, upstream (Recv-Q 0 in 48/48) | `phase2-*.txt` |
| Writer sweep | naive 200 · sync-off 11,746 · txn-100 3,706 · multirow-1000 37,220 · copy-5000 45,050 · no-DB 120,135 | `phase4-writer-summary.txt` |
| SSD | 710 fdatasync/s | `phase4-pg-test-fsync.txt` |
| Live latency | naive p99 1,009.0 ms (queue 636) · multirow p99 240.5 ms | `phase4-latency-*.txt` |
| Sanitizers | 82/82 tests under each; replays verified; TSan live 15 min, 12,168 trades = rows; 0 reports; positive controls fire | `phase5-*.txt` |
| Valgrind | 0 errors; 40,561 allocs = frees | `phase5-valgrind-memcheck.txt` |
| Pulled cable | detected +19.978 s; reconnected +6.193 s after restore; 953 missing reported; rows = received (31,392) | `phase5-netdrop-timeline.txt` |
| Suspend (accidental) | after a ~10 h laptop sleep: timeout on resume, reconnect, 555,713 missing reported | `phase5-suspend-resume-incidental.txt` |
| Profile | DOM parse 76.4% of client instructions | `phase6-callgrind.txt` |
| Parser | DOM 4,507 ns / 31 allocs → SAX 2,408 ns / 7 allocs | `phase6-micro.txt` |
| Copy vs move | 142 vs 150 ns, 0 allocs each (SSO): no effect | same |
| Parser end-to-end | no-DB 119,357 → 191,876/s; multirow unchanged | `phase6-e2e-*-summary.txt` |
| Index | one symbol 637.0 → 55.4 ms; all symbols 780.8 vs 846.6 ms (no gain) | `week4-vwap-index.txt` |
| Docker | build stage 1.18 GB → runtime 139 MB; compose e2e OK | `week4-docker-*.txt` |
| Tests | 82 (was 14) | `ctest` |

---

## 3. Problems found and fixed (interview stories, detail in `Log.md`)

1. **Ctrl+C hang inherited from week 1.** A completion that's already queued can't be cancelled,
   and Asio's range connect retries the next IP on cancel. Separately, Beast's internal timer
   kept the process alive for 10 s.
2. **Feed lag and 2,253 lost trades.** Proved the delay was upstream of the client with `ss` socket sampling.
3. **libpqxx link failure.** The distro build is C++17 and incompatible with C++20 code; fixed by building libpqxx from source.
4. **COPY lost to multi-row INSERT** at most batch sizes, contrary to the plan's expectation.
5. **TSan aborted at startup** on this kernel; fixed by running TSan binaries with ASLR off (`setarch -R`).
6. **Two of my own measurement mistakes, caught and corrected:** a UBSan control program the
   compiler optimized away, and a TSan warning counter that counted its own header line.
7. **The laptop slept overnight mid-test.** The runs were redone with sleep blocked, and the
   accidental 10 h freeze became a recovery test.

---

## 4. What the plan expected vs what happened

| Plan said | Measured |
|---|---|
| "Batched COPY" for the headline | Multi-row INSERT is faster at live-sized batches; COPY only wins at 5,000 rows |
| Move semantics probably no win (SSO) | Confirmed: no measurable difference; a control shows where it would matter |
| A DOM → SAX/on-demand change could be a real win | Yes for CPU and allocations (1.87×, 31 → 7); no change to write throughput |
| `perf record` profile | `perf` is blocked here (`perf_event_paranoid=4`, no root); used callgrind instead |
| iptables network drop | Done inside a container's network namespace (no host sudo); also dropped INPUT, since OUTPUT-only isn't a clean "pulled cable" |
| Jitter in backoff (optional) | Skipped: it conflicts with the must-preserve 1, 2, 4, 8, 16 s check |

---

## 5. The soak (last step)

- **Status:** armed but **waiting for AC power**: still on battery at 09:36 (`AC=0`). The
  moment the charger is connected it starts automatically, runs 12 hours with sleep and
  lid-close suspend blocked (`systemd-inhibit`), and writes `bench/results/phase7-soak.csv`
  (RSS, trades, reconnects, gaps, queue depth, drops, rows: one row per minute) and
  `phase7-soak.log`.
- **Binary:** the final build, commit `3d39a34`, default writer multirow/1000/200 ms, into the
  `tickstream_live` database.
- **Afterwards:** check that RSS is flat, and use only numbers from the CSV for the optional
  soak bullet.

---

## 6. Things you should know

- **GitHub:** the first 14 of my commits (up to Phase 4) were pushed by mistake. `COMMIT_PLAN.md`
  has the commands to undo that and the order to commit the files yourself.
- **Where to read:** `README.md` (overview, results, how to reproduce), `BENCHMARKS.md`
  (method, every table, conditions), `Log.md` (decisions, bug write-ups),
  `bench/results/` (every raw output).
- **Conditions:** a laptop, partly on battery, with a browser running. Benchmarks taken on
  different days aren't comparable (multirow-1000 measured 37,220/s in the sweep and ~20,000/s
  the next morning); every comparison in this report uses runs taken back to back.
- **Environment:** no sudo, so libraries came from Ubuntu packages unpacked to
  `~/.local/share/tickstream-deps`. Your plan is now `PLAN.md`; the rules are in `CLAUDE.md`.
  New databases: `tickstream_bench`, `tickstream_live`, `tickstream_tsan`, `tickstream_vwap`
  (1.1 GB; drop it with `dropdb tickstream_vwap` when you no longer need it). Your old
  `tickstream` database, `schema.sql` and `stream_db.py` were not touched.

---

## 7. Appendix: every measured number, by result file

All files are in `bench/results/`. Numbers are copied from the files, not re-derived, except
where a line says "derived".

**Phase 0**
- `phase0-tests.txt`: 14/14 tests passed (week-1 code).
- `phase0-callback-baseline-size.txt`: `feed_client.cpp` 245 + `feed_client.hpp` 97 = 342 lines; 5 `if (ec)`; 6 `return fail(`; 12 member functions.
- `phase0-machine-specs.txt`: i5-7200U (2C/4T, max 3.1 GHz, 3 MiB L3), 15 GiB RAM, Micron 1100 SATA SSD, Ubuntu 24.04.4, kernel 7.0.0-31, g++ 13.3.0, CMake 3.28.3, PostgreSQL 16.15, Boost 1.83; laptop on battery, `powersave` governor.

**Phase 1**
- `phase1-coroutine-size.txt`: 231 + 91 = 322 lines; 220 → 194 non-blank, non-comment; 0 `if (ec)`; 1 `catch`; 7 `co_await`; 3 member functions.
- `phase1-backoff-unshare.txt`: waits of 1, 2, 4, 8, 16 s, then 30 s.
- `phase1-shutdown-time.txt`: 2.011 / 0.679 / 0.823 s (median 0.823); force quit exit 130 after 0.211 s.
- `phase1-shutdown-race-before-fix.txt`: week-1 code 4/12 hangs, plus exits of 9.221 s and 9.803 s; coroutine rewrite before the fix 5/12 hangs.
- `phase1-shutdown-race-after-fix.txt` (first fix only): 0/36 hangs, but 6/36 exits took 9.279–9.952 s.
- `phase1-shutdown-race-after-fix2.txt` (both fixes): 0/36 hangs, slowest exit 1.408 s.
- `phase1-shutdown-mid-handshake.txt`: header only. This is the run that hung and led to the bug hunt; it has no data.

**Phase 2**
- `phase2-live-rate.txt`: 36,910 trades over 899 s; mean 41.06/s; median 1-s bucket 9; p99 599; peak 808; 47 zero-trade seconds; BTCUSDT 11,389 (12.67/s), ETHUSDT 15,447 (17.18/s), BNBUSDT 5,712 (6.35/s), SOLUSDT 4,362 (4.85/s).
- `phase2-capture-log.txt`: 1 server disconnect ("End of file"), reconnect after 1 s; 4 gaps, 2,253 missing (BNB 158, BTC 473, SOL 279, ETH 1,343).
- `phase2-capture-reconnect-analysis.txt`: dark windows by exchange time: BNB 45.347 s, BTC 43.037 s, SOL 45.295 s, ETH 44.685 s; approximate lag before the disconnect rose to 37.90 s.
- `phase2-feed-lag-diagnosis.txt`: lag up to 16.40 s; Recv-Q 0 in 48/48 samples; smoothed RTT 340.3–365.2 ms; min RTT 295.6 ms.
- `phase2-rest-api-reachability.txt`: REST ping HTTP 200 (0.604 s; mirror 1.314 s).

**Phase 3**
- `phase3-verify-naive.txt`: 36,910 rows; sum(price) 1,014,122,891.34000000 and sum(quantity) 16,713.90137000 match the file exactly; that replay ran at 485 trades/s (one run, 76.105 s).
- `phase3-trade-size.txt`: `sizeof(Trade)` 136, `sizeof(std::string)` 32, SSO capacity 15; queue 65,536 slots = 8,912,896 bytes; longest string in the capture 14 chars.
- `phase3-live-naive-smoke.txt`: 763 rows in 30 s, 0 drops, queue max 93; ids contiguous per symbol.

**Phase 4**
- `phase4-verify-smoke.txt`: 5 configs all PASS (single correctness runs, not benchmarks: 4,994 / 12,997 / 11,267 / 18,605 / 32,925 trades/s).
- `phase4-writer-summary.txt` (221,460 trades per run; runs → median):
  - null-1000: 127,585 / 120,135 / 104,276 → 120,135
  - naive: 471 / 200 / 198 → 200
  - naive, sync commit off: 11,746 / 6,752 / 12,116 → 11,746
  - txn-100: 3,456 / 3,706 / 5,883 → 3,706
  - multirow-10: 1,706 / 1,711 / 1,693 → 1,706
  - multirow-100: 8,994 / 11,878 / 12,040 → 11,878
  - multirow-1000: 33,445 / 37,220 / 37,521 → 37,220
  - multirow-5000: 40,991 / 35,375 / 31,075 → 35,375
  - copy-10: 1,361 / 1,347 / 1,428 → 1,361
  - copy-100: 9,093 / 9,188 / 9,927 → 9,188
  - copy-1000: 32,055 / 31,918 / 34,979 → 32,055
  - copy-5000: 46,527 / 45,050 / 41,501 → 45,050
- `phase4-ratios.txt` (derived): vs naive median, multirow-1000 186.1×, copy-5000 225.3×; vs live peak 46.1× and 55.8×; vs naive best run 79.0× and 95.6×; copy-5000 = 37.5% of the no-DB ceiling.
- `phase4-pg-test-fsync.txt`: open_datasync 783.0/s (1,277 µs); fdatasync 710.0/s (1,409 µs); fsync 309.8/s (3,228 µs).
- `phase4-latency-naive.txt`: 12,611 samples; mean 152.1, p50 56.0, p95 662.8, p99 1,009.0, p99.9 1,114.9, min 1.25, max 2,157.8 ms; queue max 636.
- `phase4-latency-multirow-1000-200ms.txt`: 8,492 samples; mean 166.2, p50 202.8, p95 217.3, p99 240.5, p99.9 245.6, min 4.13, max 246.2 ms; queue max 115.

**Phase 5**
- `phase5-asan-ubsan-tests.txt`: 82/82. `phase5-tsan-tests.txt`: 82/82; 16 concurrency tests × 20 repeats pass; 0 TSan warnings.
- `phase5-sanitizer-canaries.txt`: data race, heap-use-after-free and signed overflow each detected.
- `phase5-asan-replay.txt` / `phase5-tsan-replay.txt`: 73,820 rows verified, exit 0, no reports.
- `phase5-valgrind-memcheck.txt`: 0 errors; 40,561 allocs = 40,561 frees; 13,362,293 bytes allocated in total; 0 bytes in use at exit.
- `phase5-tsan-live.txt`: 15 min, 12,168 trades = 12,168 rows, 0 drops, 0 gaps, queue max 136, 0 TSan reports.
- `phase5-netdrop-timeline.txt`: drop 60.243 s; detected +19.978 s; 3 failed reconnects at 10 s each (backoff 1, 2, 4 s), then 8 s; reconnected +6.193 s after restore; outage 66.436 s; 4 gaps, 953 missing (SOL 149, BTC 260, BNB 406, ETH 138); 31,392 trades = 31,392 rows; 4 reconnects; queue max 213; 0 sanitizer reports.
- `phase5-suspend-resume-incidental.txt`: after a ~10 h laptop suspend, timeout on resume, connected 1.364 s after the retry, 4 gaps, 555,713 missing (SOL 97,506, BTC 224,536, ETH 137,502, BNB 96,169); 0 TSan reports.
- `phase5-tsan-aslr.txt`: "FATAL: ThreadSanitizer: unexpected memory mapping" with ASLR on; works under `setarch -R`.

**Phase 6**
- `phase6-callgrind.txt`: 203,236,575 instructions in total; `parse_trade_dom` 76.4%; writer thread 14.7% (`write_multirow` 13.1%); `free` 8.65% and `malloc` 5.76% inclusive.
- `phase6-micro.txt` (median of 3; CV in brackets): DOM 4,507 ns, 31 allocs (2.4%); SAX 2,408 ns, 7 allocs (5.4%); push copy 142 ns, 0 allocs (13.6%); push move 150 ns, 0 allocs (1.6%); long-string copy 292 ns, 6 allocs (12.8%); long-string move 158 ns, 3 allocs (0.4%).
- `phase6-e2e-dom-summary.txt`: null 122,752 / 117,347 / 119,357 → 119,357; multirow-1000 17,460 / 19,965 / 21,242 → 19,965.
- `phase6-e2e-sax-summary.txt`: null 169,204 / 194,720 / 191,876 → 191,876; multirow-1000 20,630 / 20,556 / 19,892 → 20,556.

**Week 4**
- `week4-vwap-dataset.txt`: 10,002,610 rows; span 2 days 19:44:58; 1,107 MB; SQL copies + VACUUM ANALYZE took 3 min 24.5 s (wall clock).
- `week4-vwap-index.txt`:
  - One symbol before: 663.5 / 637.0 / 625.9 ms → 637.0 ms (parallel seq scan).
  - One symbol after: 55.4 / 61.3 / 50.2 → 55.4 ms (bitmap index scan).
  - All symbols before: 726.9 / 783.6 / 780.8 → 780.8 ms.
  - All symbols after: 940.5 / 846.6 / 788.7 → 846.6 ms.
  - `CREATE INDEX` 21.0 s; `ANALYZE` 0.35 s; index 116 MB; result rows 61 (one symbol) and 244 (all).
- `week4-docker-image-sizes.txt`: build stage 1,175,959,283 B; runtime 138,953,553 B; ubuntu:24.04 117,400,818 B.
- `week4-docker-compose-e2e.txt`: 588 rows after ~45 s (BNB 275, BTC 181, ETH 23, SOL 109); graceful stop, total 589, exit 0.

**Phase 7 (soak)**: not run yet. No numbers.

