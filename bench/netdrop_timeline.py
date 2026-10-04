#!/usr/bin/env python3
"""Timeline of a network-drop test (bench/netdrop.sh).

    bench/netdrop_timeline.py bench/results/phase5-netdrop

Reads PREFIX.events.txt (when the iptables rules went on/off) and PREFIX.log
(the client's own log, same machine clock via the mounted /etc/localtime)
and prints: time to detect the dead connection, each reconnect attempt,
time from restore to reconnected, total outage, and the gaps reported.
"""
import datetime
import re
import sys


def ts(text):
    return datetime.datetime.strptime(text, "%Y-%m-%d %H:%M:%S.%f")


prefix = sys.argv[1]
events = open(prefix + ".events.txt").read()
drop_on = ts(re.search(r"^(\S+ \S+) DROP ON", events, re.M).group(1))
drop_off = ts(re.search(r"^(\S+ \S+) DROP OFF", events, re.M).group(1))

log = [(ts(m.group(1)), m.group(2)) for m in
       (re.match(r"\[(\S+ \S+)\] \[\w+\] (.*)", line) for line in open(prefix + ".log")) if m]
after = [(t, msg) for t, msg in log if t >= drop_on]

detect = next((t, msg) for t, msg in after if "failed" in msg)
reconnected = next(t for t, msg in after if msg == "connected")
gaps = [(t, msg) for t, msg in after if msg.startswith("gap in") and t >= reconnected]

print(f"drop on:     {drop_on}")
print(f"drop off:    {drop_off}  (drop lasted {(drop_off - drop_on).total_seconds():.3f} s)")
print(f"detected:    {detect[0]}  +{(detect[0] - drop_on).total_seconds():.3f} s after drop  [{detect[1]}]")
print("reconnect attempts while down:")
for t, msg in after:
    if detect[0] < t <= reconnected and ("failed" in msg or "reconnecting in" in msg):
        print(f"  {t.time()}  {msg}")
print(f"reconnected: {reconnected}  +{(reconnected - drop_off).total_seconds():.3f} s after restore")
print(f"total outage (drop on -> connected): {(reconnected - drop_on).total_seconds():.3f} s")
missing = 0
for t, msg in gaps:
    print(f"  {t.time()}  {msg}")
    missing += int(re.search(r"\((\d+) trades\)", msg).group(1))
print(f"gaps reported after reconnect: {len(gaps)}, missing trades: {missing}")
for t, msg in log:
    if msg.startswith("feed stopped") or msg == "shut down":
        print(f"end: {t}  {msg}")
