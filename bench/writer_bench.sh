#!/usr/bin/env bash
# Writer throughput benchmark: replay a capture through the real pipeline
# (parse -> queue -> writer thread -> PostgreSQL) for each writer
# configuration, RUNS times each, and report the median.
#
#   bench/writer_bench.sh                  # the full sweep below
#   bench/writer_bench.sh 'copy-500|--writer copy --batch-size 500'   # chosen configs
#
# Every run starts from an empty table, and every run's table is then checked
# against the capture (row count and exact NUMERIC sums), so each throughput
# number comes from a run that stored exactly the right data.
#
# Throughput = rows / (first enqueue -> last commit), printed by tickstream.
# Env: CAPTURE, COPIES (default 6, ~221k trades), RUNS (3), TICKSTREAM_DSN
# (dbname=tickstream_bench), BIN, PREFIX (raw-output file prefix).
set -euo pipefail

CAPTURE=${CAPTURE:-bench/data/live_capture.jsonl}
COPIES=${COPIES:-6}
RUNS=${RUNS:-3}
DSN=${TICKSTREAM_DSN:-dbname=tickstream_bench}
BIN=${BIN:-./build/tickstream}
PREFIX=${PREFIX:-phase4-writer}
RESULTS=bench/results

if [ $# -gt 0 ]; then
    CONFIGS=("$@")
else
    CONFIGS=(
        "null-1000|--writer null --batch-size 1000"
        "naive|--writer naive --batch-size 1"
        "naive-syncoff|--writer naive --batch-size 1 --synchronous-commit off"
        "txn-100|--writer txn --batch-size 100"
        "multirow-10|--writer multirow --batch-size 10"
        "multirow-100|--writer multirow --batch-size 100"
        "multirow-1000|--writer multirow --batch-size 1000"
        "multirow-5000|--writer multirow --batch-size 5000"
        "copy-10|--writer copy --batch-size 10"
        "copy-100|--writer copy --batch-size 100"
        "copy-1000|--writer copy --batch-size 1000"
        "copy-5000|--writer copy --batch-size 5000"
    )
fi

summary=$RESULTS/$PREFIX-summary.txt
{
    echo "# writer benchmark: $(date -Iseconds)"
    echo "# commit $(git rev-parse --short HEAD)$(git diff --quiet || echo ' (dirty)'), build type $(grep -oP 'CMAKE_BUILD_TYPE:STRING=\K.*' build/CMakeCache.txt)"
    echo "# capture $CAPTURE x $COPIES copies, $RUNS runs per config, median reported"
    echo "# power: AC online=$(cat /sys/class/power_supply/AC*/online 2>/dev/null || echo ?), governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo ?), loadavg=$(cut -d' ' -f1-3 /proc/loadavg)"
    printf '%-16s %10s %10s %10s %10s  %s\n' config run1 run2 run3 median verify
} | tee "$summary"

for entry in "${CONFIGS[@]}"; do
    name=${entry%%|*}
    flags=${entry#*|}
    rates=()
    verdicts=()
    for run in $(seq 1 "$RUNS"); do
        raw=$RESULTS/$PREFIX-$name-run$run.txt
        psql "$DSN" -qc "TRUNCATE trades"
        {
            echo "\$ $BIN --replay $CAPTURE --replay-copies $COPIES --dsn $DSN --flush-ms 200 $flags"
            # shellcheck disable=SC2086  # $flags is intentionally word-split
            "$BIN" --replay "$CAPTURE" --replay-copies "$COPIES" --dsn "$DSN" --flush-ms 200 $flags 2>&1
        } >"$raw"
        rate=$(grep -oP 'throughput=\K[0-9]+' "$raw")
        if [[ $flags == *"--writer null"* ]]; then
            verdict="n/a(null)"
        elif python3 bench/verify_db.py "$CAPTURE" "$COPIES" "$DSN" >>"$raw" 2>&1; then
            verdict=PASS
        else
            verdict=FAIL
        fi
        rates+=("$rate")
        verdicts+=("$verdict")
    done
    median=$(printf '%s\n' "${rates[@]}" | sort -n | sed -n "$(( (RUNS + 1) / 2 ))p")
    verdict_summary=$(printf '%s\n' "${verdicts[@]}" | sort -u | tr '\n' ' ')
    printf '%-16s %10s %10s %10s %10s  %s\n' "$name" "${rates[0]}" "${rates[1]:-}" \
        "${rates[2]:-}" "$median" "$verdict_summary" | tee -a "$summary"
done
