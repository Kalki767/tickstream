#!/usr/bin/env bash
# Live receive -> commit latency for one writer configuration.
#
#   bench/live_latency.sh NAME SECONDS [tickstream flags...]
#   bench/live_latency.sh multirow-1000 300 --writer multirow --batch-size 1000 --flush-ms 200
#
# Writes bench/results/phase4-latency-NAME.{log,ns,txt}: the service log (its
# stats lines show live rate, queue depth and drops), one latency in ns per
# committed row, and the percentiles. Both ends of each latency are
# steady_clock readings in this process, so there is no clock skew.
set -euo pipefail
set -m  # keep default SIGINT disposition for the background child

NAME=$1
SECONDS_TO_RUN=$2
shift 2
DSN=${TICKSTREAM_DSN:-dbname=tickstream_live}
BIN=${BIN:-./build/tickstream}
OUT=bench/results/phase4-latency-$NAME

psql "$DSN" -qc "TRUNCATE trades"
timeout -s INT "$SECONDS_TO_RUN" "$BIN" --dsn "$DSN" --latency-out "$OUT.ns" "$@" >"$OUT.log" 2>&1 ||
    [ $? -eq 124 ]
{
    echo "\$ $BIN --dsn $DSN --latency-out $OUT.ns $* (for ${SECONDS_TO_RUN}s)"
    grep -E "feed stopped|wrote .* latency" "$OUT.log" | sed 's/^\[[^]]*\] \[info\] //'
    python3 "$(dirname "$0")/latency.py" "$OUT.ns"
} | tee "$OUT.txt"
