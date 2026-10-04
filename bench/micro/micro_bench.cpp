// Hot-path micro-benchmarks (Google Benchmark).
//
//   ./build/tickstream_bench --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
//
// This binary replaces the global operator new with one that counts calls,
// so each benchmark can report heap allocations per iteration ("allocs").
// Only this binary is affected; the service uses the normal allocator.
// (Aligned operator new is not replaced; nothing here uses over-aligned types.)

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "tickstream/binance.hpp"
#include "tickstream/bounded_queue.hpp"
#include "tickstream/trade.hpp"

namespace {
std::atomic<std::uint64_t> g_allocations{0};
}  // namespace

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
// The replacement new above allocates with malloc, so free is the matching
// deallocation. GCC can't see that pairing and warns; the warning is wrong here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#pragma GCC diagnostic pop
// operator new[] / delete[] default to calling the functions above.

namespace {

using tickstream::Trade;

// The committed 1,000-message sample of real Binance traffic, so parse cost
// is averaged over real symbols, prices and field lengths.
const std::vector<std::string>& sample_messages() {
    static const std::vector<std::string> lines = [] {
        std::ifstream in(TICKSTREAM_SOURCE_DIR "/bench/data/sample_capture.jsonl");
        if (!in) {
            throw std::runtime_error("missing bench/data/sample_capture.jsonl");
        }
        std::vector<std::string> v;
        for (std::string line; std::getline(in, line);) {
            v.push_back(line);
        }
        return v;
    }();
    return lines;
}

void report_allocations(benchmark::State& state, std::uint64_t before) {
    state.counters["allocs"] = benchmark::Counter(
        static_cast<double>(g_allocations.load(std::memory_order_relaxed) - before),
        benchmark::Counter::kAvgIterations);
}

template <std::optional<Trade> (*Parse)(std::string_view)>
void BM_Parse(benchmark::State& state) {
    const auto& messages = sample_messages();
    std::size_t i = 0;
    const std::uint64_t before = g_allocations.load();
    for (auto _ : state) {
        auto trade = Parse(messages[i]);
        benchmark::DoNotOptimize(trade);
        if (++i == messages.size()) {
            i = 0;
        }
    }
    report_allocations(state, before);
}
BENCHMARK(BM_Parse<tickstream::binance::parse_trade_dom>)->Name("BM_ParseTrade/dom");
BENCHMARK(BM_Parse<tickstream::binance::parse_trade_sax>)->Name("BM_ParseTrade/sax");

// A Trade as the parser produces it. long_strings = false: real field
// lengths (all <= 15 chars, so libstdc++'s small-string optimization keeps
// them inside the object). true: a control with 24-char strings that must
// live on the heap.
Trade make_trade(bool long_strings) {
    Trade t;
    t.symbol = long_strings ? "BTCUSDT_PERPETUAL_000000" : "BTCUSDT";
    t.price = long_strings ? "63012.450000000000000000" : "63012.45000000";
    t.quantity = long_strings ? "0.0015000000000000000000" : "0.00150000";
    t.trade_id = 3812345678;
    t.trade_time_ms = 1727190000120;
    return t;
}

// Push one Trade through the queue and pop it again. Both variants first make
// the same local copy `t`; they differ only in how `t` reaches try_push:
// copied (lvalue) or moved (std::move). So the difference between them is
// exactly one Trade copy versus one Trade move.
template <bool Move>
void BM_Push(benchmark::State& state) {
    const Trade prototype = make_trade(state.range(0) != 0);
    tickstream::BoundedQueue<Trade> queue(1024);
    const std::uint64_t before = g_allocations.load();
    for (auto _ : state) {
        Trade t = prototype;
        if constexpr (Move) {
            queue.try_push(std::move(t));
        } else {
            queue.try_push(t);
        }
        auto out = queue.pop();
        benchmark::DoNotOptimize(out);
    }
    report_allocations(state, before);
}
BENCHMARK(BM_Push<false>)->Name("BM_PushCopy")->ArgName("long_strings")->Arg(0)->Arg(1);
BENCHMARK(BM_Push<true>)->Name("BM_PushMove")->ArgName("long_strings")->Arg(0)->Arg(1);

}  // namespace

BENCHMARK_MAIN();
