#!/usr/bin/env python3
"""Live trade rate ("demand") from a capture file.

    bench/live_rate.py bench/data/live_capture.jsonl

Buckets trades into 1-second windows by the exchange trade time "T" and
reports mean and peak trades/sec. Seconds with no trades count as 0, so the
mean is total trades / capture span, not an average over busy seconds only.

Also reports distinct (symbol, trade_id) pairs, which is the row count a
correct writer must produce for this file, and any id gaps inside the capture.
"""
import collections
import json
import statistics
import sys


def main(path: str) -> None:
    per_second = collections.Counter()
    per_symbol = collections.Counter()
    ids = collections.defaultdict(list)
    lines = 0
    for line in open(path, encoding="utf-8"):
        lines += 1
        msg = json.loads(line)
        data = msg.get("data", msg)
        if data.get("e") != "trade":
            continue
        per_second[data["T"] // 1000] += 1
        per_symbol[data["s"]] += 1
        ids[data["s"]].append(data["t"])

    first, last = min(per_second), max(per_second)
    span = last - first + 1
    counts = [per_second.get(s, 0) for s in range(first, last + 1)]
    total = sum(counts)
    counts_sorted = sorted(counts)

    def pct(p: float) -> int:
        return counts_sorted[min(len(counts_sorted) - 1, int(p / 100 * len(counts_sorted)))]

    distinct = sum(len(set(v)) for v in ids.values())
    gaps = missing = 0
    for v in ids.values():
        s = sorted(set(v))
        for a, b in zip(s, s[1:]):
            if b > a + 1:
                gaps += 1
                missing += b - a - 1

    print(f"file: {path}")
    print(f"lines: {lines}")
    print(f"trades: {total}")
    print(f"distinct (symbol, trade_id): {distinct}")
    print(f"span: {span} s (exchange trade time T, first to last second inclusive)")
    print(f"mean: {total / span:.2f} trades/s")
    print(f"median 1-s bucket: {statistics.median(counts):.0f} trades/s")
    print(f"p99 1-s bucket: {pct(99)} trades/s")
    print(f"peak 1-s bucket: {max(counts)} trades/s")
    print(f"seconds with zero trades: {counts.count(0)}")
    print(f"id gaps inside capture: {gaps} ({missing} missing trades)")
    print("per symbol:")
    for sym, n in sorted(per_symbol.items()):
        print(f"  {sym}: {n} trades, {n / span:.2f}/s")


if __name__ == "__main__":
    main(sys.argv[1])
