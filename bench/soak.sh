#!/usr/bin/env bash
# Long-running live soak test.
#
#   bench/soak.sh [hours] [extra tickstream flags...]
#   bench/soak.sh 12 --writer copy --batch-size 500 --flush-ms 200
#
# Runs the live service and every 60 s samples:
#   RSS (VmRSS from /proc/<pid>/status), and from the latest stats line:
#   total trades, reconnects, gaps, missing trades, max queue depth, drops,
#   rows written.
# Writes $OUT.csv (one row per sample) and $OUT.log (the service's own log),
# then stops the service with SIGINT (graceful) at the end.
#
# Env: TICKSTREAM_DSN (default dbname=tickstream_live), BIN, OUT.
set -euo pipefail

HOURS=${1:-12}
shift || true
DSN=${TICKSTREAM_DSN:-dbname=tickstream_live}
BIN=${BIN:-./build/tickstream}
OUT=${OUT:-bench/results/phase7-soak}
INTERVAL_S=60

"$BIN" --dsn "$DSN" --stats-interval "$INTERVAL_S" "$@" >"$OUT.log" 2>&1 &
pid=$!
stop() {
    kill -INT "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}
trap stop EXIT

field() {  # field NAME LINE -> the integer after "NAME="
    grep -oP "(?<= $1=)[0-9]+" <<<"$2" || echo ""
}

start=$(date +%s)
end=$((start + HOURS * 3600))
echo "time,elapsed_s,rss_kb,total,reconnects,gaps,missing,queue_max,dropped,written" >"$OUT.csv"
while [ "$(date +%s)" -lt "$end" ] && kill -0 "$pid" 2>/dev/null; do
    sleep "$INTERVAL_S"
    kill -0 "$pid" 2>/dev/null || break
    rss=$(awk '/^VmRSS:/ {print $2}' "/proc/$pid/status")
    line=$(grep "\] stats:" "$OUT.log" | tail -1 || true)
    echo "$(date -Iseconds),$(($(date +%s) - start)),$rss,$(field total "$line"),$(field reconnects "$line"),$(field gaps "$line"),$(field missing "$line"),$(field queue_max "$line"),$(field dropped "$line"),$(field written "$line")" >>"$OUT.csv"
done
if ! kill -0 "$pid" 2>/dev/null; then
    echo "service exited before the soak ended; see $OUT.log" >&2
fi
