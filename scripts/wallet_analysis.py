#!/usr/bin/env python3
"""Experiment 003: does a wallet's toxicity persist, and does it separate flow?

Reads results/wallets.csv from build/wallets and runs exactly the walk forward
the pre-registration describes. Nothing here searches a parameter: K, the
thresholds, the horizon and the evaluation window are all fixed in
experiments/003_wallet_toxicity/hypothesis.md.
"""
import argparse
import csv
from collections import defaultdict

import numpy as np

EXCLUDE = {"2026-08-07", "2026-08-19", "2026-09-14"}


def spearman(x, y):
    if len(x) < 4:
        return np.nan
    rx = np.argsort(np.argsort(x)).astype(float)
    ry = np.argsort(np.argsort(y)).astype(float)
    if rx.std() == 0 or ry.std() == 0:
        return np.nan
    return float(np.corrcoef(rx, ry)[0, 1])


def boot_mean(vals, n, rng):
    v = np.asarray([x for x in vals if np.isfinite(x)], dtype=float)
    if len(v) < 2:
        return float("nan"), float("nan"), float("nan")
    idx = rng.integers(0, len(v), size=(n, len(v)))
    m = v[idx].mean(axis=1)
    return float(v.mean()), float(np.percentile(m, 2.5)), float(np.percentile(m, 97.5))


def boot_ratio(num, den, n, rng):
    """Pooled size weighted mean: sum(num)/sum(den), days resampled together."""
    num = np.asarray(num, float)
    den = np.asarray(den, float)
    keep = den != 0
    if keep.sum() < 2:
        return float("nan"), float("nan"), float("nan")
    num, den = num[keep], den[keep]
    idx = rng.integers(0, len(num), size=(n, len(num)))
    d = den[idx].sum(axis=1)
    r = num[idx].sum(axis=1)[d != 0] / d[d != 0]
    return float(num.sum() / den.sum()), float(np.percentile(r, 2.5)), float(
        np.percentile(r, 97.5))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="results/wallets.csv")
    ap.add_argument("--k", type=int, default=14, help="trailing window, days")
    ap.add_argument("--eval-from", default="2026-08-25")
    ap.add_argument("--eval-to", default="2026-09-13")
    ap.add_argument("--min-trailing", type=int, default=200)
    ap.add_argument("--min-day", type=int, default=20)
    ap.add_argument("--horizon", choices=["5", "30", "300"], default="30")
    ap.add_argument("--role", choices=["aggressor", "maker"], default="aggressor")
    ap.add_argument("--boot", type=int, default=10000)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    col = {"5": "w_mo5", "30": "w_mo30", "300": "w_mo300"}[args.horizon]

    # day -> wallet -> (weighted markout sum, weight, prints)
    data = defaultdict(dict)
    for r in csv.DictReader(open(args.csv)):
        if r["role"] != args.role or r["day"] in EXCLUDE:
            continue
        data[r["day"]][r["wallet"]] = (float(r[col]), float(r["scored_eth"]),
                                       int(r["scored"]))
    days = sorted(data)
    ev = [d for d in days if args.eval_from <= d <= args.eval_to]
    if not ev:
        raise SystemExit("no evaluation days")

    per_day_rho = []
    gap_num_hi, gap_den_hi, gap_num_lo, gap_den_lo = [], [], [], []
    all_num, all_den = [], []
    n_wallets = []

    for d in ev:
        i = days.index(d)
        hist = days[max(0, i - args.k):i]
        if len(hist) < args.k:
            continue
        trail = defaultdict(lambda: [0.0, 0.0, 0])
        for h in hist:
            for w, (s, e, n) in data[h].items():
                t = trail[w]
                t[0] += s
                t[1] += e
                t[2] += n
        rows = []
        for w, (s, e, n) in data[d].items():
            t = trail.get(w)
            if not t or t[2] < args.min_trailing or n < args.min_day or t[1] <= 0 or e <= 0:
                continue
            rows.append((t[0] / t[1], s / e, s, e))
        if len(rows) < 8:
            continue
        n_wallets.append(len(rows))
        score = np.array([r[0] for r in rows])
        nxt = np.array([r[1] for r in rows])
        num = np.array([r[2] for r in rows])
        den = np.array([r[3] for r in rows])
        per_day_rho.append(spearman(score, nxt))
        q1, q3 = np.percentile(score, 25), np.percentile(score, 75)
        hi, lo = score >= q3, score <= q1
        gap_num_hi.append(num[hi].sum()); gap_den_hi.append(den[hi].sum())
        gap_num_lo.append(num[lo].sum()); gap_den_lo.append(den[lo].sum())
        all_num.append(num.sum()); all_den.append(den.sum())

    rng = np.random.default_rng(20260916)
    rho, rlo, rhi = boot_mean(per_day_rho, args.boot, rng)
    hi_m, hi_lo, hi_hi = boot_ratio(gap_num_hi, gap_den_hi, args.boot, rng)
    lo_m, lo_lo, lo_hi = boot_ratio(gap_num_lo, gap_den_lo, args.boot, rng)
    base, _, _ = boot_ratio(all_num, all_den, args.boot, rng)

    # The gap, resampling the same days for both quartiles.
    gh, dh = np.array(gap_num_hi), np.array(gap_den_hi)
    gl, dl = np.array(gap_num_lo), np.array(gap_den_lo)
    idx = rng.integers(0, len(gh), size=(args.boot, len(gh)))
    g = gh[idx].sum(1) / dh[idx].sum(1) - gl[idx].sum(1) / dl[idx].sum(1)
    gap = hi_m - lo_m

    print(f"experiment 003, role {args.role}, horizon {args.horizon}s, "
          f"trailing {args.k} days")
    print(f"  evaluation days scored   {len(per_day_rho)} "
          f"({ev[0]} .. {ev[-1]}, {len(EXCLUDE)} excluded days skipped)")
    print(f"  wallets scored per day   median {int(np.median(n_wallets))}, "
          f"min {min(n_wallets)}, max {max(n_wallets)}")
    print(f"  eligibility              >= {args.min_trailing} trailing prints, "
          f">= {args.min_day} on the day")
    print()
    print(f"  H1  persistence, mean within-day Spearman")
    print(f"        {rho:+.4f}   95% CI [{rlo:+.4f}, {rhi:+.4f}]"
          f"   {'excludes zero' if (rlo > 0 or rhi < 0) else 'crosses zero'}")
    print()
    print(f"  H2  separation, next-day size weighted aggressor markout, bps")
    print(f"        top quartile by trailing score     {hi_m:+.4f}  "
          f"[{hi_lo:+.4f}, {hi_hi:+.4f}]")
    print(f"        bottom quartile                    {lo_m:+.4f}  "
          f"[{lo_lo:+.4f}, {lo_hi:+.4f}]")
    print(f"        all eligible flow                  {base:+.4f}")
    print(f"        GAP                                {gap:+.4f}  "
          f"[{np.percentile(g,2.5):+.4f}, {np.percentile(g,97.5):+.4f}]"
          f"   {'excludes zero' if np.percentile(g,2.5) > 0 or np.percentile(g,97.5) < 0 else 'crosses zero'}")


if __name__ == "__main__":
    main()
