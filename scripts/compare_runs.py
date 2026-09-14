#!/usr/bin/env python3
"""Paired comparison of two simulator runs at the same configuration.

Both runs see the same market on the same days, so the comparison is paired by
day. Used for the model gated agent against the ungated one.
"""
import argparse
import csv
from collections import defaultdict

import numpy as np


def load(path, gamma, requote, latency):
    out = {}
    for r in csv.DictReader(open(path)):
        if abs(float(r["gamma"]) - gamma) > 1e-9:
            continue
        if int(r["requote_ticks"]) != requote:
            continue
        if abs(float(r["latency_ms"]) - latency) > 1e-9:
            continue
        out[r["day"]] = r
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True, help="baseline daily csv")
    ap.add_argument("--b", required=True, help="treatment daily csv")
    ap.add_argument("--label-a", default="A")
    ap.add_argument("--label-b", default="B")
    ap.add_argument("--gamma", type=float, default=5.0)
    ap.add_argument("--requote", type=int, default=3)
    ap.add_argument("--latency", type=float, default=0.1)
    ap.add_argument("--boot", type=int, default=10000)
    args = ap.parse_args()

    A = load(args.a, args.gamma, args.requote, args.latency)
    B = load(args.b, args.gamma, args.requote, args.latency)
    days = sorted(set(A) & set(B))
    if not days:
        raise SystemExit("no overlapping days")

    rng = np.random.default_rng(20260914)
    idx = rng.integers(0, len(days), size=(args.boot, len(days)))

    def col(D, name):
        return np.array([float(D[d][name]) for d in days])

    print(f"{args.label_a} vs {args.label_b}: gamma={args.gamma} requote={args.requote} "
          f"latency={args.latency} ms, {len(days)} days, {args.boot} draws")
    print(f"{'metric':>22} {args.label_a:>12} {args.label_b:>12} {'difference':>12} "
          f"{'[95% CI]':>24}")

    for label, name in [("markout 1s bps", "markout_1s_bps"),
                        ("markout 5s bps", "markout_5s_bps"),
                        ("markout 30s bps", "markout_30s_bps"),
                        ("markout 300s bps", "markout_300s_bps")]:
        wa, wb = col(A, "markout_n"), col(B, "markout_n")
        va, vb = col(A, name), col(B, name)
        pa = (va * wa).sum() / wa.sum()
        pb = (vb * wb).sum() / wb.sum()
        d = ((vb * wb)[idx].sum(1) / wb[idx].sum(1)) - ((va * wa)[idx].sum(1) / wa[idx].sum(1))
        print(f"{label:>22} {pa:12.4f} {pb:12.4f} {pb-pa:12.4f} "
              f"[{np.percentile(d,2.5):10.4f},{np.percentile(d,97.5):10.4f}]")

    for label, num, den in [("swept share of fills", "swept_fills", "maker_fills")]:
        na, da = col(A, num), col(A, den)
        nb, db = col(B, num), col(B, den)
        pa, pb = na.sum() / da.sum(), nb.sum() / db.sum()
        d = (nb[idx].sum(1) / db[idx].sum(1)) - (na[idx].sum(1) / da[idx].sum(1))
        print(f"{label:>22} {pa:12.4f} {pb:12.4f} {pb-pa:12.4f} "
              f"[{np.percentile(d,2.5):10.4f},{np.percentile(d,97.5):10.4f}]")

    pa_, na_ = col(A, "net_pnl"), col(A, "maker_notional") + col(A, "taker_notional")
    pb_, nb_ = col(B, "net_pnl"), col(B, "maker_notional") + col(B, "taker_notional")
    ea, eb = pa_.sum() / na_.sum() * 1e4, pb_.sum() / nb_.sum() * 1e4
    d = (pb_[idx].sum(1) / nb_[idx].sum(1) - pa_[idx].sum(1) / na_[idx].sum(1)) * 1e4
    print(f"{'edge bps':>22} {ea:12.4f} {eb:12.4f} {eb-ea:12.4f} "
          f"[{np.percentile(d,2.5):10.4f},{np.percentile(d,97.5):10.4f}]")
    print(f"\n{'fills':>22} {col(A,'maker_fills').sum():12.0f} {col(B,'maker_fills').sum():12.0f}")
    print(f"{'net pnl usd':>22} {pa_.sum():12.2f} {pb_.sum():12.2f}")


if __name__ == "__main__":
    main()
