#!/usr/bin/env bash
# Measures graceful-shutdown time while connected, and checks that a second
# SIGINT force-quits.
#
#   bench/shutdown_test.sh [binary] [runs]
#
# For each run: start the client, wait until it logs "connected" plus a few
# seconds of streaming, send SIGINT, and time how long the process takes to
# exit (`date +%s.%N` before the signal and after `wait` returns; one machine,
# one clock, so there is no cross-machine skew).
set -euo pipefail
# Job control: without it, bash starts background jobs with SIGINT ignored,
# which is not how the binary runs from a terminal.
set -m

BIN=${1:-./build/tickstream}
RUNS=${2:-3}
WARMUP_S=5

wait_for_connected() {
    local log=$1
    for _ in $(seq 1 300); do
        grep -q "\] connected" "$log" && return 0
        sleep 0.1
    done
    echo "never connected" >&2
    return 1
}

log=$(mktemp)
trap 'rm -f "$log"' EXIT

echo "# graceful shutdown: SIGINT -> process exit, while connected"
for i in $(seq 1 "$RUNS"); do
    "$BIN" >"$log" 2>&1 &
    pid=$!
    wait_for_connected "$log"
    sleep "$WARMUP_S"
    t0=$(date +%s.%N)
    kill -INT "$pid"
    wait "$pid" && status=0 || status=$?
    t1=$(date +%s.%N)
    printf 'run %d: %.3f s (exit status %d) | %s\n' "$i" "$(echo "$t1 - $t0" | bc)" "$status" \
        "$(grep -E 'closed|shut down' "$log" | sed 's/^\[[^]]*\] //' | tr '\n' ' ')"
done

echo
echo "# force quit: SIGINT, then a second SIGINT 200 ms later"
"$BIN" >"$log" 2>&1 &
pid=$!
wait_for_connected "$log"
sleep "$WARMUP_S"
t0=$(date +%s.%N)
kill -INT "$pid"
sleep 0.2
kill -INT "$pid"
wait "$pid" && status=0 || status=$?
t1=$(date +%s.%N)
printf 'force quit: %.3f s after first SIGINT, exit status %d (130 = killed by SIGINT)\n' \
    "$(echo "$t1 - $t0" | bc)" "$status"
echo "last log line: $(tail -1 "$log")"
