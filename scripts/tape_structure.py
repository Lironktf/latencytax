#!/usr/bin/env python3
"""Structure of the Hyperliquid trade tape, straight from the raw files.

Produces the two facts the latency result rests on:

  1. the histogram of the gap between consecutive prints, which has a hole in it
     between 1 ms and 30 ms because the venue batches into blocks;
  2. a check that the feed's "side" field is the aggressor's side, by comparing
     each print against the prevailing snapshot best bid and offer.

Also reports the price range and the spread distribution, which are quoted in
the README.

Usage:
  scripts/tape_structure.py --data data/raw --days 2026-08-09,2026-08-10,...
  scripts/tape_structure.py --data data/raw --from 2026-08-09 --to 2026-08-12
"""
import argparse
import bisect
import glob
import gzip
import json
import os
import sys
from collections import Counter


def days_in(root, first, last):
    out = []
    for p in sorted(glob.glob(os.path.join(root, "date=*"))):
        d = os.path.basename(p)[5:]
        if first and d < first:
            continue
        if last and d > last:
            continue
        out.append(d)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="data/raw")
    ap.add_argument("--days")
    ap.add_argument("--from", dest="first")
    ap.add_argument("--to", dest="last")
    args = ap.parse_args()

    book_root = os.path.join(args.data, "l2book_ETH")
    trade_root = os.path.join(args.data, "trades_ETH")
    days = args.days.split(",") if args.days else days_in(book_root, args.first, args.last)
    if not days:
        sys.exit("no days")

    snap_t, snap_bid, snap_ask = [], [], []
    trades = []
    for d in days:
        for f in sorted(glob.glob(f"{book_root}/date={d}/*.jsonl.gz")):
            with gzip.open(f, "rt") as fh:
                for line in fh:
                    b = json.loads(line)["book"]
                    lv = b["levels"]
                    if not lv[0] or not lv[1]:
                        continue
                    snap_t.append(b["time"])
                    snap_bid.append(round(float(lv[0][0]["px"]) * 10))
                    snap_ask.append(round(float(lv[1][0]["px"]) * 10))
        for f in sorted(glob.glob(f"{trade_root}/date={d}/*.jsonl.gz")):
            with gzip.open(f, "rt") as fh:
                for line in fh:
                    t = json.loads(line)["trade"]
                    trades.append((t["time"], t["side"], round(float(t["px"]) * 10)))
    order = sorted(range(len(snap_t)), key=lambda i: snap_t[i])
    snap_t = [snap_t[i] for i in order]
    snap_bid = [snap_bid[i] for i in order]
    snap_ask = [snap_ask[i] for i in order]
    trades.sort()

    print(f"days {len(days)}  ({days[0]} .. {days[-1]})")
    print(f"snapshots {len(snap_t)}   prints {len(trades)}")
    print()

    mids = sorted((a + b) / 20.0 for a, b in zip(snap_ask, snap_bid))
    med = mids[len(mids) // 2]
    print(f"price range: {min(snap_bid)/10:.1f} to {max(snap_ask)/10:.1f} USD, "
          f"median mid {med:.1f}")
    print(f"one tick of 0.1 USD at the median mid is {0.1/med*1e4:.4f} bps, "
          f"so a half spread at the touch is {0.05/med*1e4:.4f} bps")
    spread = Counter(a - b for a, b in zip(snap_ask, snap_bid))
    tot = sum(spread.values())
    print("spread, in ticks of 0.1 USD:")
    for k, v in sorted(spread.items())[:5]:
        print(f"  {k:2d} tick  {v:8d}  {100.0*v/tot:6.2f}%")
    unchanged = sum(1 for i in range(len(snap_bid) - 1) if snap_bid[i + 1] == snap_bid[i])
    print(f"best bid unchanged between consecutive snapshots: "
          f"{100.0*unchanged/(len(snap_bid)-1):.2f}%")
    print()

    gaps = [trades[i + 1][0] - trades[i][0] for i in range(len(trades) - 1)]
    edges = [0, 1, 2, 5, 10, 20, 30, 50, 70, 100, 150, 200, 300, 500, 1000, 10**12]
    print(f"gap between consecutive prints, {len(gaps)} gaps")
    print(f"  {'bucket':>18} {'count':>9} {'share':>8} {'cum':>8}")
    cum = 0
    for i in range(len(edges) - 1):
        lo, hi = edges[i], edges[i + 1]
        n = sum(1 for g in gaps if lo < g <= hi) if lo > 0 else sum(1 for g in gaps if g <= hi)
        cum += n
        label = f"{lo} to {hi} ms" if hi < 10**12 else f"above {lo} ms"
        print(f"  {label:>18} {n:9d} {100.0*n/len(gaps):7.2f}% {100.0*cum/len(gaps):7.2f}%")
    nz = sorted(g for g in gaps if g > 0)
    if nz:
        print(f"  non-zero gaps: p1 {nz[len(nz)//100]} ms, p5 {nz[len(nz)//20]} ms, "
              f"p50 {nz[len(nz)//2]} ms")
    print()

    rel = Counter()
    for t, side, px in trades:
        i = bisect.bisect_right(snap_t, t) - 1
        if i < 0:
            continue
        if px >= snap_ask[i]:
            rel[(side, "at or above the ask")] += 1
        elif px <= snap_bid[i]:
            rel[(side, "at or below the bid")] += 1
        else:
            rel[(side, "inside the spread")] += 1
    print("print price against the prevailing snapshot best bid and offer")
    for s in ("A", "B"):
        tot = sum(v for (k, _), v in rel.items() if k == s)
        if not tot:
            continue
        print(f'  side "{s}": {tot} prints')
        for where in ("at or below the bid", "inside the spread", "at or above the ask"):
            v = rel[(s, where)]
            print(f"      {where:>22}: {v:8d}  {100.0*v/tot:6.2f}%")
    print()
    print('conclusion: "A" is a seller hitting the bid, "B" is a buyer lifting the offer.')


if __name__ == "__main__":
    main()
