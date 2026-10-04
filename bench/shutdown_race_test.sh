#!/usr/bin/env bash
# Sends SIGINT at a sweep of delays after start, so it lands in different
# connection stages (DNS resolve, TCP connect, TLS, WebSocket handshake,
# reading). A correct client exits promptly in every case; a run still alive
# DEADLINE_S seconds after the signal is reported as HANG and killed.
#
#   bench/shutdown_race_test.sh [binary] [delays...]
set -uo pipefail
set -m  # see shutdown_test.sh: keep default SIGINT disposition for the child

BIN=${1:-./build/tickstream}
shift || true
DELAYS=("$@")
[ ${#DELAYS[@]} -eq 0 ] && DELAYS=(0.05 0.1 0.15 0.2 0.3 0.5 0.8 1.2 1.6 2.0 2.5 3.0)
DEADLINE_S=10

log=$(mktemp)
trap 'rm -f "$log"' EXIT

hangs=0
for d in "${DELAYS[@]}"; do
    "$BIN" >"$log" 2>&1 &
    pid=$!
    sleep "$d"
    t0=$(date +%s.%N)
    kill -INT "$pid"
    result=HANG
    for _ in $(seq 1 $((DEADLINE_S * 20))); do
        if ! kill -0 "$pid" 2>/dev/null; then
            result=$(printf 'exited after %.3f s' "$(echo "$(date +%s.%N) - $t0" | bc)")
            break
        fi
        sleep 0.05
    done
    if [ "$result" = HANG ]; then
        hangs=$((hangs + 1))
        kill -KILL "$pid" 2>/dev/null
    fi
    wait "$pid" 2>/dev/null
    # Last connection stage logged before the signal arrived.
    stage=$(grep -v "BTCUSDT\|ETHUSDT\|SOLUSDT\|BNBUSDT" "$log" | grep -B1 "received signal" |
            head -1 | sed 's/^\[[^]]*\] \[[a-z]*\] //' | cut -c1-40)
    printf 'SIGINT at %4ss | %-24s | last stage before signal: %s\n' "$d" "$result" "$stage"
done
echo "hangs: $hangs / ${#DELAYS[@]}"
