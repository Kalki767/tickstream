#!/usr/bin/env python3
"""Percentiles of receive -> commit latency.

    bench/latency.py FILE [FILE...]

Each FILE holds one latency per line in nanoseconds, as written by
`tickstream --latency-out`. Both ends are steady_clock readings on this
machine, so there is no cross-machine clock skew. Percentiles use the
nearest-rank method on the raw samples (no interpolation, no histogram).
"""
import math
import sys


def nearest_rank(sorted_values, p):
    k = max(1, math.ceil(p / 100 * len(sorted_values)))
    return sorted_values[k - 1]


for path in sys.argv[1:]:
    ns = sorted(int(line) for line in open(path) if line.strip())
    ms = [v / 1e6 for v in ns]
    print(f"file: {path}")
    print(f"samples: {len(ms)}")
    print(f"mean: {sum(ms) / len(ms):.3f} ms")
    for p in (50, 95, 99, 99.9):
        print(f"p{p:g}: {nearest_rank(ms, p):.3f} ms")
    print(f"min: {ms[0]:.3f} ms")
    print(f"max: {ms[-1]:.3f} ms")
