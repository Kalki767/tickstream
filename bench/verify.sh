#!/usr/bin/env bash
# Correctness check for a writer configuration: empty the table, replay a
# capture through the real pipeline, and require the table to match the file
# exactly (row count and exact NUMERIC sums). Run after every writer change.
#
#   bench/verify.sh CAPTURE [COPIES] [extra tickstream flags, e.g. --writer naive]
#
# Uses $TICKSTREAM_DSN (default dbname=tickstream_bench). TRUNCATEs its table.
set -euo pipefail

CAPTURE=$1
COPIES=${2:-1}
shift $(( $# >= 2 ? 2 : 1 ))
DSN=${TICKSTREAM_DSN:-dbname=tickstream_bench}
BIN=${BIN:-./build/tickstream}

psql "$DSN" -qc "TRUNCATE trades"
"$BIN" --replay "$CAPTURE" --replay-copies "$COPIES" --dsn "$DSN" "$@" 2>&1 | grep "replay:"
python3 "$(dirname "$0")/verify_db.py" "$CAPTURE" "$COPIES" "$DSN"
