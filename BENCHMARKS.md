# Benchmarks

Every number below has its raw output under [`bench/results/`](bench/results/) and was produced
in this repository by the scripts in [`bench/`](bench/). Throughput and micro-benchmark numbers
are the **median of 3 runs**, all from a RelWithDebInfo build (`-O2 -g -DNDEBUG`), never a
sanitizer build.

## Machine

From [`phase0-machine-specs.txt`](bench/results/phase0-machine-specs.txt):

| | |
|---|---|
| CPU | Intel Core i5-7200U @ 2.50 GHz (max 3.1 GHz), 2 cores / 4 threads, 3 MiB L3 |
| RAM | 15 GiB |
| Disk | Micron 1100 SATA SSD, 256 GB (Postgres data directory on it) |
| OS | Ubuntu 24.04.4 LTS, kernel 7.0.0-31-generic |
| Compiler | g++ 13.3.0 |
| CMake | 3.28.3 |
| PostgreSQL | 16.15, local, default durability settings (`fsync=on`, `synchronous_commit=on`, `wal_sync_method=fdatasync`, `shared_buffers=128MB`) |
| Boost | 1.83 (Beast/Asio) |
| libpqxx | 7.8.1, built from source as C++20 |
| OpenSSL | 3.x (system `libssl.so.3`) |

**Conditions to keep in mind when reading absolute numbers:** a laptop **on battery** with the
`powersave` CPU governor, with a desktop session running (a browser tab used about half a core
throughout; load average ≈ 3–4 when the sweep started, recorded in each summary file). Postgres
and the client share the same 4 threads and the same SSD. Ratios between configurations are more
portable than absolute rates.

## Phase 4: write capacity (replay benchmark)

**Method** ([`bench/writer_bench.sh`](bench/writer_bench.sh)):
- Input: the 15-minute live capture (36,910 trades), replayed 6 times with `trade_id` offset by
  k·10¹⁰ per copy, giving **221,460 trades** per run.
- Path: the same parse → queue → writer thread → PostgreSQL code as live mode, with the queue
  blocking instead of dropping.
- Throughput: rows ÷ (first enqueue → last commit).
- Each run starts from an empty table, and **each run's table is verified** against the
  capture (row count plus exact `NUMERIC` sums of price and quantity). Every database
  configuration passed on all 3 runs.
- Raw outputs: `bench/results/phase4-writer-<config>-run<N>.txt`. Summary:
  [`phase4-writer-summary.txt`](bench/results/phase4-writer-summary.txt). Ratios:
  [`phase4-ratios.txt`](bench/results/phase4-ratios.txt).

**Demand, for comparison** ([`phase2-live-rate.txt`](bench/results/phase2-live-rate.txt)):
mean 41.06 trades/s, peak 808 trades/s (1-second buckets, 4 symbols).

| Configuration | Run 1 | Run 2 | Run 3 | **Median trades/s** | × naive | × live peak (808/s) |
|---|---:|---:|---:|---:|---:|---:|
| `null` (no database: parse + queue ceiling) | 127,585 | 120,135 | 104,276 | **120,135** | — | 148.7 |
| `naive`: prepared INSERT per row, autocommit | 471 | 200 | 198 | **200** | 1.0 | **0.25** |
| `naive` + `synchronous_commit=off` (diagnostic) | 11,746 | 6,752 | 12,116 | **11,746** | 58.7 | 14.5 |
| `txn` batch 100: same INSERTs, 1 commit per 100 | 3,456 | 3,706 | 5,883 | **3,706** | 18.5 | 4.6 |
| `multirow` batch 10 | 1,706 | 1,711 | 1,693 | **1,706** | 8.5 | 2.1 |
| `multirow` batch 100 | 8,994 | 11,878 | 12,040 | **11,878** | 59.4 | 14.7 |
| `multirow` batch 1,000 | 33,445 | 37,220 | 37,521 | **37,220** | 186.1 | 46.1 |
| `multirow` batch 5,000 | 40,991 | 35,375 | 31,075 | **35,375** | 176.9 | 43.8 |
| `copy` batch 10 | 1,361 | 1,347 | 1,428 | **1,361** | 6.8 | 1.7 |
| `copy` batch 100 | 9,093 | 9,188 | 9,927 | **9,188** | 45.9 | 11.4 |
| `copy` batch 1,000 | 32,055 | 31,918 | 34,979 | **32,055** | 160.3 | 39.7 |
| `copy` batch 5,000 | 46,527 | 45,050 | 41,501 | **45,050** | 225.3 | 55.8 |

**Where the time goes.**
1. The naive writer is bounded by the per-commit WAL flush. Changing only
   `synchronous_commit` (COMMIT no longer waits for the WAL flush; the SQL, round trips and
   rows are identical) made it **58.7× faster** (200 → 11,746 trades/s).
2. Independent check: this SSD completes **710 `fdatasync`/s, 1.41 ms each**
   ([`phase4-pg-test-fsync.txt`](bench/results/phase4-pg-test-fsync.txt), run on AC power).
   Any write path that commits once per row is capped near that rate whatever the SQL looks like.
3. `txn` keeps the per-row round trips but shares one commit per 100 rows: 18.5×. Removing the
   round trips as well (`multirow`/`copy`) gives the rest.
4. Synchronous commit stays on in every real configuration. `off` was only used to locate the cost.

**Findings that contradict the plan's expectations (reported as measured):**
- **COPY is not the winner at most batch sizes.** At batch 10, 100 and 1,000, multi-row INSERT
  beat COPY (1,706 vs 1,361; 11,878 vs 9,188; 37,220 vs 32,055). COPY wins only at 5,000
  (45,050 vs 35,375). Likely reason, not separately measured: this COPY path writes each row
  twice (staging table, then `INSERT … SELECT … ON CONFLICT`) for idempotency, plus one extra
  statement per batch, and that fixed cost is only amortised in large batches.
- **The naive writer cannot keep up with live bursts.** Even its best run (471/s) is 58% of the
  808/s peak, and the median is 25%. During a burst the queue absorbs the difference.

**Production default: `--writer multirow --batch-size 1000 --flush-ms 200`.**
- In live mode, batches are cut by `flush-ms`, not by size. At 200 ms the measured rates give
  about 8 rows per batch on average (41/s) and about 160 at the peak (808/s). At those sizes
  multirow beats copy (batch 10 and 100 rows above).
- Capacity at batch 1,000: 37,220 trades/s, **46×** the live peak; even batch 100 has 14.7×.
- `flush-ms` = 200 caps the batching delay a trade can see, and with it the batch a crash can
  lose from the writer's buffer (rows still in the queue are also lost on a crash; see the
  limitations section of the README).
- The sweep also shows the ceiling: `null` is 2.7× faster than the best database configuration,
  so the database, not parsing or queueing, is the bottleneck.

**Run-to-run noise:** the laptop ran on battery at the start of the sweep, and the charger was
connected partway through. Naive's first run (471/s) is 2.4× its other two (200, 198). The
fsync test above ran on AC. I did not re-run the sweep to separate the effect of power state,
so naive is reported as the median with the full range shown.

## Phase 4: live latency (receive → commit)

**Method** ([`bench/live_latency.sh`](bench/live_latency.sh)):
- 5 minutes of live Binance traffic per configuration, with 0 drops, gaps and reconnects in both runs.
- One sample per committed row: commit-returned time minus message-received time. Both are
  `std::chrono::steady_clock` readings in the same process, so there is no clock skew.
  Exchange-to-us time is deliberately not used (two machines' clocks, and the feed-lag finding
  in Log.md).
- Percentiles by nearest rank over all raw samples ([`bench/latency.py`](bench/latency.py)).
  Raw per-row samples: `phase4-latency-*.ns`. Service logs, with a stats line every 10 s:
  `phase4-latency-*.log`.

| Writer | Samples | p50 | p95 | p99 | p99.9 | max | Max queue depth |
|---|---:|---:|---:|---:|---:|---:|---:|
| `naive` | 12,611 | 56.0 ms | 662.8 ms | **1,009.0 ms** | 1,114.9 ms | 2,157.8 ms | 636 |
| `multirow`, 1,000 rows, 200 ms | 8,492 | 202.8 ms | 217.3 ms | **240.5 ms** | 245.6 ms | 246.2 ms | 115 |

Sources: [`phase4-latency-naive.txt`](bench/results/phase4-latency-naive.txt),
[`phase4-latency-multirow-1000-200ms.txt`](bench/results/phase4-latency-multirow-1000-200ms.txt).

**Reading it:**
- **Batching raises the typical latency by about `flush-ms`** (p50 56.0 → 202.8 ms). Trades
  arrive in clumps, so most rows join a batch early and wait close to the full 200 ms.
- **It lowers the tail.** p99 drops 4.2× (1,009.0 → 240.5 ms) and the max 8.8× (2,157.8 →
  246.2 ms). The naive writer falls behind in bursts: in the 10 s window with 167.8 trades/s its
  queue reached 636 trades, and those rows waited behind one-commit-per-row writes. The batched
  writer's worst case is bounded by `flush-ms` plus one commit.
- `flush-ms` is the knob. A lower value buys lower p50 at the cost of more commits per second.
  Not tuned here.

**Caveat:** the runs were back to back (22:14–22:19 and 22:19–22:24), so they saw different
live traffic (12,611 vs 8,492 trades). A same-input comparison would replay a capture with its
original timing, which this tool doesn't do yet.

## Week 4: VWAP query and a composite index

**Dataset** ([`week4-vwap-dataset.txt`](bench/results/week4-vwap-dataset.txt)):
- The 36,910-trade capture, loaded through the real pipeline, then copied in SQL 270 more
  times. Copy *k* shifts `trade_id` by k·10¹⁰ and the timestamps by k × 900 s (the capture spans
  899 s).
- **10,002,610 rows covering 2 days 19:44:58**, 1,107 MB with the primary key. The data is
  synthetic in its *volume*; every row is a real trade's price, quantity and symbol mix. Without
  the time shift all copies would share one 15-minute window and an index test would be meaningless.

**Method** ([`bench/vwap_index.sh`](bench/vwap_index.sh), raw:
[`week4-vwap-index.txt`](bench/results/week4-vwap-index.txt)):
- Per-minute VWAP (`sum(price·quantity) / sum(quantity)`) over the last hour of data, anchored
  at `max(event_time)` so it is reproducible.
- 1 warm-up run, then 3 runs of `EXPLAIN (ANALYZE, BUFFERS)`; median execution time reported.
- PostgreSQL 16.15, `shared_buffers=128MB`.

| Query | Before: PK only | After: `(symbol, event_time DESC)` | Plan |
|---|---:|---:|---|
| A: one symbol (`WHERE symbol = 'BTCUSDT'`), 61 result rows | 637.0 ms (663.5 / 637.0 / 625.9) | **55.4 ms** (55.4 / 61.3 / 50.2) | parallel seq scan → bitmap index scan: **11.5×** |
| B: all symbols, 244 result rows | 780.8 ms (726.9 / 783.6 / 780.8) | 846.6 ms (940.5 / 846.6 / 788.7) | seq scan both times: **no gain** |

Index build: 21.0 s; index size 116 MB.

**Reading it:** the composite index helps exactly the query shape it was designed for (one
symbol, recent time range). For the all-symbols query, the leading `symbol` column makes it
useless on PostgreSQL 16, which has no skip scan, so the planner correctly keeps the sequential
scan. Not measured, so not claimed: an index or BRIN index on `event_time` alone would be the
next thing to try for query B. BRIN fits here because rows arrive in time order.

## Week 4: Docker

Raw: [`week4-docker-image-sizes.txt`](bench/results/week4-docker-image-sizes.txt),
[`week4-docker-compose-e2e.txt`](bench/results/week4-docker-compose-e2e.txt).

| Image | Size (bytes) | |
|---|---:|---|
| `ubuntu:24.04` (base of both stages) | 117,400,818 | |
| build stage (toolchain + Boost + sources) | 1,175,959,283 | |
| **runtime** (binary + libssl3 + libpq5 + CA certificates) | **138,953,553** | 8.5× smaller than the build stage; +21.6 MB over the base |

`docker compose up` end to end:
1. Postgres starts with `sql/schema.sql` applied and becomes healthy; tickstream then starts.
2. **588 rows** were in the containerized database after ~45 s.
3. `docker compose stop` sends SIGINT; the service shut down gracefully (`total=589`, exit 0).

The password comes from `.env`/the environment (`${POSTGRES_PASSWORD:?…}` fails fast if unset);
nothing is hard-coded.

## Phase 6: hot path

### Profile (callgrind; `perf` isn't usable here: `perf_event_paranoid=4`, no root)

[`phase6-callgrind.txt`](bench/results/phase6-callgrind.txt): instructions executed by the
process while replaying 5,000 trades through the default multirow writer with the DOM parser.

| Part | Share of instructions |
|---|---:|
| `parse_trade_dom` (nlohmann DOM parse + field reads) | **76.4%** |
| writer thread (`run_writer` → `PgWriter::write_multirow`, libpq) | 14.7% |
| `malloc` + `free` (inclusive, overlaps the rows above) | ~14% |

Callgrind counts instructions, not wall time: time spent waiting on Postgres doesn't appear. It
shows where the client's own CPU goes, not what limits write throughput.

### Micro-benchmarks (Google Benchmark, counting `operator new`)

[`phase6-micro.txt`](bench/results/phase6-micro.txt) (JSON: `phase6-micro.json`):
- 3 repetitions; medians shown.
- Parse cost is averaged over the 1,000 real captured messages.
- `allocs` = heap allocations per operation, counted by a replacement global `operator new`
  that exists in the benchmark binary only.
- Ran on battery (AC=0, powersave governor). Google Benchmark warned about CPU scaling noise;
  coefficients of variation are in the raw file.

| Benchmark | Median time | Allocations |
|---|---:|---:|
| Parse, DOM (`nlohmann::json::parse` + `at()`) | 4,507 ns | 31 |
| Parse, **SAX** (`nlohmann::json::sax_parse`, no tree) | **2,408 ns** | **7** |
| Queue push+pop, copy, real field lengths | 142 ns | 0 |
| Queue push+pop, move, real field lengths | 150 ns | 0 |
| Queue push+pop, copy, 24-char strings (control) | 292 ns | 6 |
| Queue push+pop, move, 24-char strings (control) | 158 ns | 3 |

**Parser: SAX is now the default.** It's **1.87× faster** and makes **31 → 7** heap allocations
per message (4.4× fewer). It passes all 12 parser tests (run against both implementations) and
the differential test: identical output to the DOM parser on all 1,000 real captured messages.
The remaining 7 allocations come from inside nlohmann's parser; I haven't broken them down.

**Move semantics: no measurable effect, as predicted.**
- Every string in a real `Trade` is ≤ 14 chars (`phase3-trade-size.txt`) and libstdc++'s
  small-string buffer holds 15. So copying a `Trade` allocates nothing, and copy (142 ns) vs
  move (150 ns) is inside the noise (copy's CV was 13.6%).
- The 24-char control shows the effect only appears once strings leave the small-string
  buffer: the extra copy then costs 3 allocations and ~134 ns. Not claimed as a win, because
  real data never hits it.

### End-to-end effect of the parser (replay, back-to-back, median of 3, all verified)

[`phase6-e2e-dom-summary.txt`](bench/results/phase6-e2e-dom-summary.txt),
[`phase6-e2e-sax-summary.txt`](bench/results/phase6-e2e-sax-summary.txt):

| Configuration | DOM | SAX | |
|---|---:|---:|---|
| `null` writer (parse + queue ceiling) | 119,357 | **191,876** | **1.61×** |
| `multirow`, 1,000 rows | 19,965 (17,460–21,242) | 20,556 (19,892–20,630) | +3%, within run-to-run spread: **no claimed change** |

The parser change lowers CPU cost per message and raises the pipeline's own ceiling, but
database writes stay bound by Postgres.

**Note on conditions:** today's multirow-1000 runs (≈20k/s) are well below yesterday's sweep
(37,220/s). Today ran on battery, right after a 10-million-row load into another database on the
same Postgres server. The Phase 4 table and this comparison are each internally consistent (back-to-back runs);
absolute rates across days are not comparable on this laptop.
