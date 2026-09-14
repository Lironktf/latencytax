#!/usr/bin/env python3
"""Confidence intervals for the latency sweep.

The agents at different latencies are deterministic functions of the same market
data, so the comparison between two latencies is paired. Resampling the two
levels independently would drown the effect in day to day market variation,
which is orders of magnitude larger. This resamples the paired difference.

Two resampling units:
  days    the registered one, one block per calendar day
  hours   marked to market at each hour boundary, more blocks and more power

Reads the csv files written by build/sim and prints a table plus the headline
slope. Everything printed here comes from those files; nothing is assumed.
"""
import argparse
import csv
import math
import sys
from collections import defaultdict

import numpy as np


def load_daily(path):
    rows = []
    with open(path) as fh:
        for r in csv.DictReader(fh):
            rows.append(r)
    return rows


def key_of(r):
    return (float(r["gamma"]), int(r["offset_ticks"]), int(r["requote_ticks"]),
            float(r["beta"]), float(r["kappa"]))


def bootstrap_paired(diffs, n=10000, seed=12345):
    """Percentile interval for the mean of a paired difference."""
    diffs = np.asarray(diffs, dtype=float)
    if len(diffs) == 0:
        return float("nan"), float("nan"), float("nan")
    rng = np.random.default_rng(seed)
    idx = rng.integers(0, len(diffs), size=(n, len(diffs)))
    means = diffs[idx].mean(axis=1)
    return float(diffs.mean()), float(np.percentile(means, 2.5)), float(np.percentile(means, 97.5))


def bootstrap_ratio(num, den, n=10000, seed=12345):
    """Percentile interval for sum(num)/sum(den), resampling blocks together.

    Edge in basis points is a ratio of two sums, so the blocks have to be
    resampled jointly rather than averaging per-block ratios.
    """
    num = np.asarray(num, dtype=float)
    den = np.asarray(den, dtype=float)
    if len(num) == 0 or den.sum() == 0:
        return float("nan"), float("nan"), float("nan")
    rng = np.random.default_rng(seed)
    idx = rng.integers(0, len(num), size=(n, len(num)))
    r = num[idx].sum(axis=1) / den[idx].sum(axis=1)
    point = num.sum() / den.sum()
    return float(point), float(np.percentile(r, 2.5)), float(np.percentile(r, 97.5))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--daily", required=True)
    ap.add_argument("--hourly")
    ap.add_argument("--gamma", type=float, default=5.0)
    ap.add_argument("--requote", type=int, default=3)
    ap.add_argument("--offset", type=int, default=0)
    ap.add_argument("--kappa", type=float, default=1.0)
    ap.add_argument("--baseline", type=float, default=0.1)
    ap.add_argument("--headline-to", type=float, default=100.0,
                    help="upper end of the registered headline slope range")
    ap.add_argument("--slope-from", type=float, default=None,
                    help="lower end of an additional slope range")
    ap.add_argument("--slope-to", type=float, default=5000.0,
                    help="upper end of that additional range")
    ap.add_argument("--boot", type=int, default=10000)
    ap.add_argument("--all-configs", action="store_true")
    args = ap.parse_args()

    rows = load_daily(args.daily)
    if not rows:
        sys.exit("no rows")

    def select(rs):
        out = []
        for r in rs:
            if abs(float(r["gamma"]) - args.gamma) > 1e-9:
                continue
            if int(r["offset_ticks"]) != args.offset:
                continue
            if int(r["requote_ticks"]) != args.requote:
                continue
            if abs(float(r["kappa"]) - args.kappa) > 1e-9:
                continue
            out.append(r)
        return out

    sel = select(rows)
    if not sel:
        sys.exit("no rows match the requested configuration")

    days = sorted({r["day"] for r in sel})
    lats = sorted({float(r["latency_ms"]) for r in sel})
    maker_bps = float(sel[0]["maker_bps"])

    # day -> latency -> row
    by = defaultdict(dict)
    for r in sel:
        by[r["day"]][float(r["latency_ms"])] = r

    print(f"configuration: gamma={args.gamma} offset={args.offset} requote={args.requote} "
          f"kappa={args.kappa}  maker fee {maker_bps} bps")
    print(f"days: {len(days)}  ({days[0]} .. {days[-1]})")
    print()
    print(f"{'latency':>9} {'fills':>8} {'notional':>12} {'net_pnl':>10} {'edge_bps':>9} "
          f"{'[95% CI]':>19} {'mo1s':>8} {'mo5s':>8} {'mo30s':>8} {'mo300s':>8}")

    table = {}
    for L in lats:
        pnl = np.array([float(by[d][L]["net_pnl"]) for d in days if L in by[d]])
        notl = np.array([float(by[d][L]["maker_notional"]) + float(by[d][L]["taker_notional"])
                         for d in days if L in by[d]])
        fills = sum(int(by[d][L]["maker_fills"]) for d in days if L in by[d])
        mo_n = np.array([float(by[d][L]["markout_n"]) for d in days if L in by[d]])
        mo1 = np.array([float(by[d][L]["markout_1s_bps"]) for d in days if L in by[d]])
        mo5 = np.array([float(by[d][L]["markout_5s_bps"]) for d in days if L in by[d]])
        mo30 = np.array([float(by[d][L]["markout_30s_bps"]) for d in days if L in by[d]])
        mo300 = np.array([float(by[d][L]["markout_300s_bps"]) for d in days if L in by[d]])
        e, lo, hi = bootstrap_ratio(pnl * 1e4, notl, args.boot)
        w = mo_n.sum()
        table[L] = dict(pnl=pnl, notl=notl, edge=e, fills=fills,
                        mo1=float((mo1 * mo_n).sum() / w) if w else float("nan"),
                        mo5=float((mo5 * mo_n).sum() / w) if w else float("nan"),
                        mo30=float((mo30 * mo_n).sum() / w) if w else float("nan"),
                        mo300=float((mo300 * mo_n).sum() / w) if w else float("nan"))
        print(f"{L:9.2f} {fills:8d} {notl.sum():12.0f} {pnl.sum():10.2f} {e:9.4f} "
              f"[{lo:8.4f},{hi:8.4f}] {table[L]['mo1']:8.4f} {table[L]['mo5']:8.4f} "
              f"{table[L]['mo30']:8.4f} {table[L]['mo300']:8.4f}")

    base = args.baseline
    if base not in table:
        sys.exit(f"baseline {base} ms not in the sweep")

    print()
    print(f"paired difference against the {base} ms baseline, bootstrap over "
          f"{len(days)} days, {args.boot} draws")
    print(f"{'latency':>9} {'d_pnl_usd':>12} {'[95% CI]':>24} {'d_edge_bps':>11} {'[95% CI]':>22}")
    for L in lats:
        if L == base:
            continue
        dp = table[L]["pnl"] - table[base]["pnl"]
        m, lo, hi = bootstrap_paired(dp, args.boot)
        # Paired edge difference: resample day blocks jointly for both levels.
        rng = np.random.default_rng(999)
        n = len(days)
        idx = rng.integers(0, n, size=(args.boot, n))
        dL, dB = table[L]["notl"][idx].sum(axis=1), table[base]["notl"][idx].sum(axis=1)
        keep = (dL != 0) & (dB != 0)
        eL = table[L]["pnl"][idx].sum(axis=1)[keep] / dL[keep] * 1e4
        eB = table[base]["pnl"][idx].sum(axis=1)[keep] / dB[keep] * 1e4
        de = eL - eB
        pt = (table[L]["pnl"].sum() / table[L]["notl"].sum() -
              table[base]["pnl"].sum() / table[base]["notl"].sum()) * 1e4
        print(f"{L:9.2f} {m:12.3f} [{lo:10.3f},{hi:10.3f}] {pt:11.4f} "
              f"[{np.percentile(de,2.5):9.4f},{np.percentile(de,97.5):9.4f}]")

    # Headline slope over the registered range.
    hi_L = args.headline_to
    if hi_L in table:
        n = len(days)
        rng = np.random.default_rng(4242)
        idx = rng.integers(0, n, size=(args.boot, n))
        dH, dB = table[hi_L]["notl"][idx].sum(axis=1), table[base]["notl"][idx].sum(axis=1)
        keep = (dH != 0) & (dB != 0)
        eH = table[hi_L]["pnl"][idx].sum(axis=1)[keep] / dH[keep] * 1e4
        eB = table[base]["pnl"][idx].sum(axis=1)[keep] / dB[keep] * 1e4
        slope = -(eH - eB) / (hi_L - base)
        pt = -(table[hi_L]["pnl"].sum() / table[hi_L]["notl"].sum() -
               table[base]["pnl"].sum() / table[base]["notl"].sum()) * 1e4 / (hi_L - base)
        lo, hig = np.percentile(slope, 2.5), np.percentile(slope, 97.5)
        print()
        print(f"HEADLINE  latency tax over {base} to {hi_L} ms")
        print(f"  {pt:+.6f} bps of notional per ms   95% CI [{lo:+.6f}, {hig:+.6f}]")
        print(f"  (positive means edge is lost as latency rises)")
        crosses = lo < 0 < hig
        print(f"  interval {'crosses' if crosses else 'does not cross'} zero")

    # Where does the curve first bite? Smallest latency whose paired PnL
    # difference has a 95% interval that stays below zero.
    knee = None
    for L in lats:
        if L == base:
            continue
        dp = table[L]["pnl"] - table[base]["pnl"]
        _, lo, hi = bootstrap_paired(dp, args.boot)
        if hi < 0:
            knee = L
            break
    print()
    if knee is None:
        print("no latency in the sweep has a paired PnL loss whose 95% interval excludes zero")
    else:
        print(f"first latency with a paired PnL loss significant at 95%: {knee:.0f} ms")

    # Optional second slope over a different range.
    if args.slope_from is not None and args.slope_from in table and args.slope_to in table:
        a_, b_ = args.slope_from, args.slope_to
        n = len(days)
        rng = np.random.default_rng(777)
        idx = rng.integers(0, n, size=(args.boot, n))
        dA, dB2 = table[a_]["notl"][idx].sum(axis=1), table[b_]["notl"][idx].sum(axis=1)
        keep = (dA != 0) & (dB2 != 0)
        eB = table[a_]["pnl"][idx].sum(axis=1)[keep] / dA[keep] * 1e4
        eH = table[b_]["pnl"][idx].sum(axis=1)[keep] / dB2[keep] * 1e4
        sl = -(eH - eB) / (b_ - a_)
        pt = -(table[b_]["pnl"].sum() / table[b_]["notl"].sum() -
               table[a_]["pnl"].sum() / table[a_]["notl"].sum()) * 1e4 / (b_ - a_)
        print(f"slope over {a_} to {b_} ms: {pt:+.6f} bps per ms  "
              f"95% CI [{np.percentile(sl,2.5):+.6f}, {np.percentile(sl,97.5):+.6f}]")
        dp = table[b_]["pnl"] - table[a_]["pnl"]
        m, lo, hi = bootstrap_paired(dp, args.boot)
        print(f"  paired PnL over the same range: {m:+.3f} USD  95% CI [{lo:+.3f}, {hi:+.3f}]")

    # Hourly block bootstrap.
    if args.hourly:
        print()
        hrs = defaultdict(lambda: defaultdict(lambda: [0.0, 0.0]))
        with open(args.hourly) as fh:
            for r in csv.DictReader(fh):
                if abs(float(r["gamma"]) - args.gamma) > 1e-9: continue
                if int(r["offset_ticks"]) != args.offset: continue
                if int(r["requote_ticks"]) != args.requote: continue
                if abs(float(r["kappa"]) - args.kappa) > 1e-9: continue
                L = float(r["latency_ms"])
                key = (r["day"], r["hour_start_ms"])
                hrs[L][key][0] += float(r["pnl"])
                hrs[L][key][1] += float(r["maker_notional"])
        if hrs:
            keys = sorted(set.intersection(*[set(v.keys()) for v in hrs.values()]))
            print(f"hourly block bootstrap, {len(keys)} blocks, {args.boot} draws")
            print(f"{'latency':>9} {'d_pnl_usd':>12} {'[95% CI]':>24}")
            bp = np.array([hrs[base][k][0] for k in keys])
            for L in sorted(hrs):
                if L == base: continue
                lp = np.array([hrs[L][k][0] for k in keys])
                m, lo, hi = bootstrap_paired(lp - bp, args.boot)
                print(f"{L:9.2f} {m*len(keys):12.3f} [{lo*len(keys):10.3f},{hi*len(keys):10.3f}]")


if __name__ == "__main__":
    main()
