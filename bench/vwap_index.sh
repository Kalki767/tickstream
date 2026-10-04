#!/usr/bin/env bash
# VWAP query before/after a composite (symbol, event_time) index.
#
#   bench/vwap_index.sh            (uses dbname=tickstream_vwap, see week4-vwap-dataset.txt)
#
# Query A: per-minute VWAP for ONE symbol over the last hour of data.
# Query B: the same for ALL symbols (no symbol predicate).
# "Last hour" is anchored at max(event_time) in the table, so results are
# reproducible. Each query: 1 warm-up run, then 3 timed runs with
# EXPLAIN (ANALYZE, BUFFERS); the median execution time is reported.
# Drops the index first if it exists, so the "before" plan is real.
set -euo pipefail

DSN=${TICKSTREAM_DSN:-dbname=tickstream_vwap}
INDEX=trades_symbol_event_time_idx
q() { psql "$DSN" -X -At -v ON_ERROR_STOP=1 -c "$1"; }

T_END=$(q "SELECT max(event_time) FROM trades")
QUERY_A="SELECT date_trunc('minute', event_time) AS minute,
       sum(price * quantity) / sum(quantity) AS vwap, count(*) AS trades
FROM trades
WHERE symbol = 'BTCUSDT'
  AND event_time >= TIMESTAMPTZ '$T_END' - interval '1 hour'
  AND event_time <  TIMESTAMPTZ '$T_END'
GROUP BY 1 ORDER BY 1"
QUERY_B="SELECT symbol, date_trunc('minute', event_time) AS minute,
       sum(price * quantity) / sum(quantity) AS vwap, count(*) AS trades
FROM trades
WHERE event_time >= TIMESTAMPTZ '$T_END' - interval '1 hour'
  AND event_time <  TIMESTAMPTZ '$T_END'
GROUP BY 1, 2 ORDER BY 1, 2"

measure() {  # measure LABEL QUERY
    local label=$1 query=$2 times=()
    q "$query" >/dev/null  # warm-up
    for run in 1 2 3; do
        plan=$(q "EXPLAIN (ANALYZE, BUFFERS) $query")
        t=$(grep -oP 'Execution Time: \K[0-9.]+' <<<"$plan")
        times+=("$t")
        [ "$run" -eq 1 ] && { echo "--- $label: plan (run 1)"; echo "$plan"; }
    done
    median=$(printf '%s\n' "${times[@]}" | sort -g | sed -n 2p)
    echo "--- $label: execution time runs: ${times[*]} ms -> median $median ms"
    echo "--- $label: result rows: $(q "SELECT count(*) FROM ($query) r")"
}

echo "# $(date -Iseconds)  rows=$(q 'SELECT count(*) FROM trades')  window: last hour before $T_END"
echo "# PostgreSQL $(q 'SHOW server_version'), shared_buffers=$(q 'SHOW shared_buffers')"
echo
q "DROP INDEX IF EXISTS $INDEX"
echo "===== BEFORE (only the primary key (symbol, trade_id)) ====="
measure "A one symbol, before" "$QUERY_A"
measure "B all symbols, before" "$QUERY_B"
echo
echo "===== CREATE INDEX $INDEX ON trades (symbol, event_time DESC) ====="
psql "$DSN" -X -v ON_ERROR_STOP=1 -c '\timing on' \
    -c "CREATE INDEX $INDEX ON trades (symbol, event_time DESC)" -c "ANALYZE trades" | grep -E "Time|CREATE"
echo "index size: $(q "SELECT pg_size_pretty(pg_relation_size('$INDEX'))")"
echo
echo "===== AFTER ====="
measure "A one symbol, after" "$QUERY_A"
measure "B all symbols, after" "$QUERY_B"
