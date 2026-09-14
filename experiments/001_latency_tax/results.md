# 001 results: what a millisecond of reaction latency costs a market maker

Status: **the millisecond claim failed, and it failed for a structural reason.**

The pre-registered hypothesis H1, that net maker edge falls as reaction latency
rises over 0.1 to 100 ms, is not supported. The null is not rejected anywhere in
that range, in any configuration tested, under any of the fill model variants.
A latency cost does exist on this venue, but it starts around one to two
seconds, which is three orders of magnitude away from where the question was
pointed.

Everything below comes from a run of `scripts/run_experiment.sh`. Nothing is
rounded up, extrapolated, or carried over from a different run.

---

## 1. Headline

Latency tax over the registered range, primary configuration, five holdout days,
Hyperliquid base tier fees, bootstrap over days with 10,000 draws:

    -0.001495 bps of notional per ms    95% CI [-0.004663, +0.000695]

The interval crosses zero. The point estimate is negative, which would mean edge
improving slightly as latency rises; that is noise, not a finding.

The same quantity on the untouched extension set, 25 days that were not looked
at until this file existed:

    -0.000598 bps of notional per ms    95% CI [-0.005000, +0.001453]

Also crossing zero.

Where the cost does appear, between 1 and 5 seconds of reaction latency on the
holdout:

    +0.000085 bps of notional per ms    95% CI [+0.000048, +0.000133]

which is 0.085 bps per second of latency. The interval excludes zero. In dollars,
moving from 1 s to 5 s of reaction latency costs the agent 22.15 USD over five
days on 0.9 million USD of traded notional, 95% CI [16.26, 28.05].

## 2. Why the millisecond range is flat: the venue has no millisecond

The trade tape has a hole in it. Over the 110,263 gaps between consecutive prints
on the calibration days:

Buckets are half open on the left, so "0 to 1 ms" means a gap of at most 1 ms.
From `scripts/tape_structure.py --from=2026-08-09 --to=2026-08-12`.

| gap between prints | count | share | cumulative |
|---|---|---|---|
| 0 to 1 ms | 32,707 | 29.66% | 29.66% |
| 1 to 2 ms | 0 | 0.00% | 29.66% |
| 2 to 5 ms | 0 | 0.00% | 29.66% |
| 5 to 10 ms | 0 | 0.00% | 29.66% |
| 10 to 20 ms | 0 | 0.00% | 29.66% |
| 20 to 30 ms | 0 | 0.00% | 29.66% |
| 30 to 50 ms | 41 | 0.04% | 29.70% |
| 50 to 70 ms | 1,800 | 1.63% | 31.33% |
| 70 to 100 ms | 826 | 0.75% | 32.08% |
| 100 to 150 ms | 2,208 | 2.00% | 34.08% |
| 150 to 200 ms | 1,050 | 0.95% | 35.04% |
| 200 to 300 ms | 3,171 | 2.88% | 37.91% |
| 300 to 500 ms | 5,040 | 4.57% | 42.48% |
| 500 to 1000 ms | 10,657 | 9.67% | 52.15% |
| above 1000 ms | 52,763 | 47.85% | 100.00% |

Nothing at all happens between 1 ms and 30 ms. The first percentile of non-zero
spacing is 66 ms. Over all 39 days rather than the four calibration days the hole
is not exactly empty, but nearly: 26 gaps out of 1,009,458 land between 1 and
30 ms, which is 0.0026%. Prints sharing a timestamp are the separate maker fills of one
aggressive order inside one block, and the gaps between them are block
intervals. An agent driven by this feed has no information to react to inside
the first 33 ms, so its behaviour at 0.1, 1, 10 and 33 ms is not merely similar,
it is identical.

That is checked rather than assumed. On the holdout, across all nine
(gamma, requote) configurations, the agent at 0.1, 1, 10 and 33 ms produces the
same number of fills and the same PnL to the last cent: the paired difference is
exactly 0.000000 USD and exactly 0 fills. On the extension set the identity holds
at gamma = 5 and very nearly holds at gamma = 0, where 10 ms differs from 0.1 ms
by 2 fills out of 76,644 and 0.08 USD out of 28,136, because those 25 days
contain a handful of prints spaced inside the hole.

This is the real content of the negative result. On a venue that batches into
blocks, reaction latency below the block interval is not expensive, it is
unobservable. Buying it cannot pay for itself because there is nothing between
two blocks to react to.

## 3. The full sweep

Primary configuration: Avellaneda-Stoikov quoting, gamma = 5, quoting at the
touch, 3 ticks of drift tolerated before giving up queue position,
beta = 0.00008052, kappa = 1.0, k = 8.6820, 0.5 ETH per quote, 25 ETH inventory
limit, maker 1.5 bps and taker 4.5 bps. Five holdout days, 2026-08-13 to
2026-08-18 excluding 08-15.

| latency | fills | notional USD | net PnL USD | edge bps | 95% CI | markout 1s | 5s | 30s | 300s |
|---|---|---|---|---|---|---|---|---|---|
| 0.1 ms | 1451 | 814,823 | -215.21 | -2.6412 | [-3.5727, -2.1998] | -0.4208 | -0.4937 | -0.6507 | -0.6867 |
| 1 ms | 1451 | 814,823 | -215.21 | -2.6412 | [-3.5727, -2.1998] | -0.4208 | -0.4937 | -0.6507 | -0.6867 |
| 10 ms | 1451 | 814,823 | -215.21 | -2.6412 | [-3.5727, -2.1998] | -0.4208 | -0.4937 | -0.6507 | -0.6867 |
| 33 ms | 1451 | 814,823 | -215.21 | -2.6412 | [-3.5727, -2.1998] | -0.4208 | -0.4937 | -0.6507 | -0.6867 |
| 66 ms | 1443 | 814,961 | -212.38 | -2.6060 | [-3.3654, -2.1930] | -0.4181 | -0.4970 | -0.6597 | -0.6204 |
| 100 ms | 1466 | 831,866 | -207.29 | -2.4918 | [-3.2483, -2.1373] | -0.4296 | -0.5141 | -0.6605 | -0.6073 |
| 200 ms | 1508 | 830,369 | -203.50 | -2.4507 | [-3.0614, -2.1591] | -0.4208 | -0.5132 | -0.6751 | -0.4876 |
| 500 ms | 1547 | 864,371 | -210.86 | -2.4394 | [-3.1677, -2.0457] | -0.4170 | -0.5366 | -0.7432 | -0.6816 |
| 1000 ms | 1577 | 916,348 | -224.35 | -2.4483 | [-3.0233, -2.0976] | -0.4478 | -0.5336 | -0.7693 | -0.7802 |
| 2000 ms | 1642 | 950,403 | -245.63 | -2.5844 | [-3.2750, -2.2324] | -0.4815 | -0.5952 | -0.8639 | -0.9349 |
| 5000 ms | 1990 | 1,202,470 | -335.12 | -2.7869 | [-3.5379, -2.3829] | -0.5429 | -0.6140 | -0.8161 | -0.7505 |

Paired difference in PnL against the 0.1 ms baseline, bootstrap over the five
days. The first latency whose interval stays entirely below zero is 2000 ms.

| latency | delta PnL USD | 95% CI |
|---|---|---|
| 1, 10, 33 ms | 0.000 | [0.000, 0.000] |
| 66 ms | +0.565 | [-1.710, +3.440] |
| 100 ms | +1.584 | [-1.589, +4.757] |
| 200 ms | +2.341 | [-0.958, +5.646] |
| 500 ms | +0.870 | [-2.698, +4.303] |
| 1000 ms | -1.829 | [-5.134, +1.365] |
| 2000 ms | -6.084 | [-11.697, -2.356] |
| 5000 ms | -23.983 | [-30.894, -17.302] |

The same comparison resampling 120 hourly blocks instead of 5 days agrees and is
tighter: 2000 ms is -31.57 USD [-52.99, -10.12] and 5000 ms is -112.06 USD
[-145.97, -79.98], where the hourly figures are scaled to the whole period.

Note the shape. Between 100 ms and 500 ms the agent trades more and loses very
slightly less per unit of notional; past 1 second it trades more *and* loses
more, in total and per unit. The activity of the agent is not independent of its
latency, which is why both the per-notional and the total-dollar views are
reported. A slower agent holds a stale quote for longer and therefore gets more
fills; when the fills are unprofitable, more of them is worse.

## 4. Inventory skew on versus off

Skew on is gamma = 5, skew off is gamma = 0, everything else equal, 0.1 ms,
five holdout days, paired by day.

| metric | skew on | skew off | difference | 95% CI |
|---|---|---|---|---|
| markout 1 s, bps | -0.4208 | -0.8042 | +0.3835 | [+0.1200, +0.4669] |
| markout 5 s, bps | -0.4937 | -0.8952 | +0.4015 | [+0.1112, +0.4716] |
| markout 30 s, bps | -0.6507 | -1.1019 | +0.4511 | [+0.0263, +0.5707] |
| markout 300 s, bps | -0.6867 | -1.1342 | +0.4475 | [-0.0955, +0.9289] |
| edge, bps | -2.6412 | -2.3670 | -0.2741 | [-2.6237, +1.7017] |
| peak absolute inventory, ETH, daily mean | 1.84 | 25.48 | -23.64 | [-24.14, -23.00] |
| fills | 1,451 | 11,044 | | |
| net PnL, USD | -215.21 | -1,827.44 | | |

Skew buys two things and pays for one. It cuts adverse selection by about
0.40 bps at every horizon out to 30 seconds, with an interval that excludes zero
at 1, 5 and 30 seconds and not at 300. It holds peak inventory at 1.84 ETH
against 25.48 for the unskewed agent, which spends its time pinned at the 25 ETH
limit; that is the largest and cleanest difference in the table. It pays by
cutting the fill count by a factor of 7.6, and the fills that remain do not cover
the fee, so net edge per unit of notional is 0.27 bps worse with an interval that
crosses zero.

## 5. Sensitivities

Every variant gives the same answer on the registered range. The headline
interval crosses zero in all of them.

| variant | latency tax 0.1 to 100 ms, bps/ms | 95% CI | first significant loss |
|---|---|---|---|
| primary (kappa 1.0, sweep Through, tier 0) | -0.001495 | [-0.004663, +0.000695] | 2000 ms |
| kappa 0.0 (no cancellation credit) | -0.000647 | [-0.002757, +0.000581] | 66 ms |
| kappa 0.5 | -0.000780 | [-0.002647, +0.000470] | 66 ms |
| sweep rule = Queue | -0.000007 | [-0.002231, +0.003689] | 2000 ms |
| fee tier 4 (maker 0.000%) | -0.001495 | [-0.004663, +0.000694] | 5000 ms |
| six day holdout including 08-15 | -0.000536 | [-0.003401, +0.001268] | 2000 ms |
| extension set, 25 untouched days | -0.000598 | [-0.005000, +0.001453] | 1000 ms |

At kappa 0 and 0.5 the first latency with a statistically significant paired PnL
difference is 66 ms rather than 2000 ms. That is worth stating plainly: under a
fill model that gives the agent no credit for cancellations ahead of it in the
queue, crossing from 33 ms to 66 ms, which is crossing one block boundary, is
detectable. It is still not evidence of a gradient inside the millisecond range,
because 0.1, 1, 10 and 33 ms remain identical under every kappa. It is evidence
that the block boundary is the thing that matters.

Across the nine (gamma, requote) configurations on the holdout, the headline
slope point estimates run from -0.001495 to +0.000870 bps per ms and every
interval includes zero. One configuration, gamma = 20 with requote = 0, takes
only about 50 fills in five days and its interval is not defined, because some
bootstrap resamples contain no fills at all.

## 6. The strategy loses money, and that is a separate finding

Every one of the 36 configurations in the calibration grid has negative net edge,
from -1.94 to -3.34 bps. It stays negative at tier 4 fees where the maker rate is
zero: the primary configuration makes -1.1403 bps there against -2.6412 bps at
tier 0. So this is not a fee problem.

The arithmetic is simple and it is about queue position, not speed.

- The spread on ETH-perp is one tick, in 99.46% of holdout snapshots. At the
  holdout median mid of 1887.0 USD, one tick of 0.1 USD is 0.5300 bps, so the
  half spread a maker at the touch is competing for is 0.2650 bps.
- The base tier maker fee is 1.5 bps, which is 5.7 times the entire half spread.
- The touch holds a few hundred ETH. An agent that joins it is behind all of
  that. In the primary configuration 41.0% to 50.1% of its fills, depending on
  latency, come from the tape trading *through* its price rather than stopping at
  it, and those are exactly the adversely selected ones.
- Its 5 second markout is -0.4937 bps. For comparison, the markout of the
  average maker on the same five days, computed directly from all 135,711 prints
  with no agent and no fill model in the way, is +0.0748 bps per print at
  5 seconds and +0.1140 weighted by size (`sim --tape-markout`). Sitting at the
  back of the queue is worth about 0.57 bps of markout.

A maker who is at the front of the queue, or who is quoting size large enough to
matter, is in a different business from the one simulated here. This result says
that joining the back of a deep one-tick queue is unprofitable on this
instrument, and that making that decision faster does not help.

## 7. What would change the answer

- **Finer book data.** The book is a snapshot every 5 seconds. Adverse selection
  inside the first 5 seconds after a fill is invisible, and that is exactly where
  a millisecond-scale effect would live if one existed on the quote side. The
  tape has millisecond stamps and the tape says nothing happens inside 33 ms, but
  quotes could still be moving. A 100 ms depth diff feed would settle this.
- **A venue that is not block-based.** The mechanism found here is a property of
  batching into blocks. On a CEX matching engine with continuous time, the hole
  in the gap distribution would not exist and the same experiment could give a
  different answer.
- **A front-of-queue maker.** The fill model puts the agent at the back of the
  queue by construction, because that is where a new order goes. An agent already
  holding queue position from earlier would have a different markout profile and
  might have more to lose from being slow.
- **Larger size.** The no-impact assumption holds at 0.5 ETH against a few
  hundred. It would not hold at 50 ETH, and at that size the agent's own
  behaviour would move the thing it is being measured against.

## 8. Reproducing this

    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
    ./scripts/run_experiment.sh

Takes about two minutes on four cores and writes everything named above into
`results/`. The fidelity check that the engine reproduces the exchange's own
snapshots runs first and the script stops if it fails.
