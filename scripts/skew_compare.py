#!/usr/bin/env python3
"""Inventory skew on versus off, at a fixed latency.

Both arms see the same market, so the comparison is paired by day. The two arms
do not fill on the same events, so this is not a paired test on individual
fills; it is a paired test on the day level aggregate, which is what the
pre-registration asked for.
"""
import argparse
import csv
from collections import defaultdict

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--daily", required=True)
    ap.add_argument("--latency", type=float, default=0.1)
    ap.add_argument("--requote", type=int, default=3)
    ap.add_argument("--on", type=float, default=5.0, help="gamma with skew on")
    ap.add_argument("--off", type=float, default=0.0, help="gamma with skew off")
    ap.add_argument("--boot", type=int, default=10000)
    args = ap.parse_args()

    arms = {args.on: defaultdict(dict), args.off: defaultdict(dict)}
    for r in csv.DictReader(open(args.daily)):
        g = float(r["gamma"])
        if g not in arms:
            continue
        if int(r["requote_ticks"]) != args.requote:
            continue
        if abs(float(r["latency_ms"]) - args.latency) > 1e-9:
            continue
        arms[g][r["day"]] = r

    days = sorted(set(arms[args.on]) & set(arms[args.off]))
    if not days:
        raise SystemExit("no overlapping days")

    def col(g, name):
        return np.array([float(arms[g][d][name]) for d in days])

    rng = np.random.default_rng(31337)
    idx = rng.integers(0, len(days), size=(args.boot, len(days)))

    print(f"inventory skew on (gamma={args.on}) versus off (gamma={args.off}), "
          f"latency {args.latency} ms, requote {args.requote}")
    print(f"{len(days)} days, {args.boot} bootstrap draws over days\n")
    print(f"{'metric':>22} {'skew on':>12} {'skew off':>12} {'difference':>12} {'[95% CI]':>24}")

    for label, name in [("markout 1s bps", "markout_1s_bps"),
                        ("markout 5s bps", "markout_5s_bps"),
                        ("markout 30s bps", "markout_30s_bps"),
                        ("markout 300s bps", "markout_300s_bps")]:
        w_on, w_off = col(args.on, "markout_n"), col(args.off, "markout_n")
        v_on, v_off = col(args.on, name), col(args.off, name)
        a = (v_on * w_on)[idx].sum(1) / w_on[idx].sum(1)
        b = (v_off * w_off)[idx].sum(1) / w_off[idx].sum(1)
        pa = (v_on * w_on).sum() / w_on.sum()
        pb = (v_off * w_off).sum() / w_off.sum()
        d = a - b
        print(f"{label:>22} {pa:12.4f} {pb:12.4f} {pa-pb:12.4f} "
              f"[{np.percentile(d,2.5):10.4f},{np.percentile(d,97.5):10.4f}]")

    for label in ["edge_bps"]:
        p_on = col(args.on, "net_pnl"); n_on = col(args.on, "maker_notional") + col(args.on, "taker_notional")
        p_off = col(args.off, "net_pnl"); n_off = col(args.off, "maker_notional") + col(args.off, "taker_notional")
        a = p_on[idx].sum(1) / n_on[idx].sum(1) * 1e4
        b = p_off[idx].sum(1) / n_off[idx].sum(1) * 1e4
        pa, pb = p_on.sum() / n_on.sum() * 1e4, p_off.sum() / n_off.sum() * 1e4
        d = a - b
        print(f"{label:>22} {pa:12.4f} {pb:12.4f} {pa-pb:12.4f} "
              f"[{np.percentile(d,2.5):10.4f},{np.percentile(d,97.5):10.4f}]")

    for label, name in [("max abs inventory ETH", "max_abs_inv_eth")]:
        a = col(args.on, name); b = col(args.off, name)
        d = (a - b)[idx].mean(1)
        print(f"{label:>22} {a.mean():12.4f} {b.mean():12.4f} {(a-b).mean():12.4f} "
              f"[{np.percentile(d,2.5):10.4f},{np.percentile(d,97.5):10.4f}]")

    print(f"\n{'fills':>22} {col(args.on,'maker_fills').sum():12.0f} "
          f"{col(args.off,'maker_fills').sum():12.0f}")
    print(f"{'net pnl usd':>22} {col(args.on,'net_pnl').sum():12.2f} "
          f"{col(args.off,'net_pnl').sum():12.2f}")


if __name__ == "__main__":
    main()
