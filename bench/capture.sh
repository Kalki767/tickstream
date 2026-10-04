#!/usr/bin/env bash
# Records live Binance trade messages for replay benchmarks.
#
#   bench/capture.sh [seconds] [output]     (default: 900 s -> bench/data/live_capture.jsonl)
#
# Also writes the first 1,000 lines to bench/data/sample_capture.jsonl, the
# small deterministic input committed to git (for Valgrind and quick checks).
set -euo pipefail

DURATION_S=${1:-900}
OUT=${2:-bench/data/live_capture.jsonl}
BIN=${BIN:-./build/tickstream}

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
echo "recording ${DURATION_S}s of live trades to $OUT"
# SIGINT, not SIGTERM's default kill: the client shuts down gracefully and
# flushes the file.
timeout -s INT "$DURATION_S" "$BIN" --record "$OUT" || [ $? -eq 124 ]
head -n 1000 "$OUT" > "$(dirname "$OUT")/sample_capture.jsonl"
wc -l "$OUT"
