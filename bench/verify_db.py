#!/usr/bin/env python3
"""Checks the trades table against a capture file.

    bench/verify_db.py CAPTURE COPIES DSN

Expected values come straight from the file: the number of distinct
(symbol, trade_id) pairs times COPIES, and the exact decimal sums of price and
quantity (Python Decimal, no floats) times COPIES. The table must match all
three exactly; the sums prove price/quantity survived string -> NUMERIC
without rounding. Exit status 0 on match, 1 otherwise.
"""
import json
import subprocess
import sys
from decimal import Decimal


def main(capture: str, copies: int, dsn: str) -> int:
    seen = {}
    for line in open(capture, encoding="utf-8"):
        data = json.loads(line)
        data = data.get("data", data)
        if data.get("e") == "trade":
            seen[(data["s"], data["t"])] = (Decimal(data["p"]), Decimal(data["q"]))
    expected = (
        len(seen) * copies,
        sum(p for p, _ in seen.values()) * copies,
        sum(q for _, q in seen.values()) * copies,
    )

    out = subprocess.run(
        ["psql", dsn, "-AtF", " ", "-c",
         "SELECT count(*), coalesce(sum(price), 0), coalesce(sum(quantity), 0) FROM trades"],
        check=True, capture_output=True, text=True).stdout.split()
    actual = (int(out[0]), Decimal(out[1]), Decimal(out[2]))

    for name, e, a in zip(("rows", "sum(price)", "sum(quantity)"), expected, actual):
        status = "OK" if e == a else "MISMATCH"
        print(f"verify {name}: expected={e} actual={a} {status}")
    ok = expected == actual
    print("verify: PASS" if ok else "verify: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], int(sys.argv[2]), sys.argv[3]))
