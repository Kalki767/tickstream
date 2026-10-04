#!/usr/bin/env bash
# Network-drop ("pulled cable") test, run against a sanitizer build.
#
#   bench/netdrop.sh [binary] [total_seconds] [drop_at_s] [drop_for_s]
#   bench/netdrop.sh ./build-asan/tickstream 1800 600 60
#
# Runs the live client inside a container (image built from bench/netdrop/)
# and, DROP_AT seconds in, silently drops all TCP traffic to and from port
# 9443 with iptables *inside the container's own network namespace*: no FIN,
# no RST, exactly like a pulled cable, and the host firewall is untouched.
# After DROP_FOR seconds the rules are removed. Expected: the 20 s websocket
# idle timeout fires, backoff reconnect attempts fail while the drop lasts,
# the first attempt after it succeeds, and the gap detector reports what
# was missed.
#
# Writes bench/results/phase5-netdrop.{log,events.txt}. The container runs as
# uid 1000 and reaches the host Postgres through its unix socket (peer auth).
set -euo pipefail

BIN=${1:-./build-asan/tickstream}
TOTAL_S=${2:-1800}
DROP_AT_S=${3:-600}
DROP_FOR_S=${4:-60}
NAME=tickstream-netdrop
OUT=bench/results/phase5-netdrop
DSN="host=/var/run/postgresql dbname=tickstream_live user=$(id -un)"

docker rm -f "$NAME" >/dev/null 2>&1 || true
psql -d tickstream_live -qc "TRUNCATE trades"

docker run -d --name "$NAME" --cap-add NET_ADMIN --user "$(id -u):$(id -g)" \
    -v "$PWD:/src:ro" -v /var/run/postgresql:/var/run/postgresql \
    -v /etc/localtime:/etc/localtime:ro -w /src \
    tickstream-netdrop "$BIN" --dsn "$DSN" --stats-interval 5 >/dev/null

stamp() { date '+%Y-%m-%d %H:%M:%S.%3N'; }
rules() {  # rules -A|-D
    docker exec --user root "$NAME" iptables "$1" OUTPUT -p tcp --dport 9443 -j DROP
    docker exec --user root "$NAME" iptables "$1" INPUT -p tcp --sport 9443 -j DROP
}

{
    echo "# $(stamp) started $BIN in container (total ${TOTAL_S}s, drop at ${DROP_AT_S}s for ${DROP_FOR_S}s)"
    sleep "$DROP_AT_S"
    rules -A
    echo "$(stamp) DROP ON  (iptables: OUTPUT dport 9443 and INPUT sport 9443 -> DROP)"
    sleep "$DROP_FOR_S"
    rules -D
    echo "$(stamp) DROP OFF (rules removed)"
    sleep $((TOTAL_S - DROP_AT_S - DROP_FOR_S))
    docker kill -s INT "$NAME" >/dev/null
    docker wait "$NAME" | sed "s/^/$(stamp) container exit status: /"
} | tee "$OUT.events.txt"

docker logs "$NAME" >"$OUT.log" 2>&1
docker rm "$NAME" >/dev/null
echo "log: $OUT.log"
