# Reaction latency below the block interval is informationally void

An empirical note on Hyperliquid ETH-perp, 2026-08-07 to 2026-09-14.

## Abstract

On a continuous-time matching engine, a market maker's reaction latency is a
continuous cost: every additional microsecond is more time during which a
resting quote reflects stale information. On a venue that orders and batches
trades into consensus blocks, that is not true. Market events become observable
only at block boundaries, so a maker that reacts in 0.1 ms and a maker that
reacts in 30 ms see the same sequence of events at the same times and make the
same decisions.

We measure this on Hyperliquid ETH-perpetual. Over the 110,263 gaps between
consecutive prints on four days, 29.66% are 0 to 1 ms and there are no
observations at all between 1 ms and 30 ms; the first percentile of non-zero
spacing is 66 ms. Over all 39 days the 1 to 30 ms range holds 26 of 1,009,458
gaps, 0.0026%. We build a limit order book and matching
engine, reconstruct the book from 5-second L2 snapshots plus the trade tape,
verify the reconstruction against 25,570,480 exchange-published level positions
with zero mismatches, and run an Avellaneda-Stoikov market maker inside the
replay at eleven reaction latencies.

On a pre-registered five-day holdout the latency tax over the 0.1 to 100 ms range
is -0.001495 basis points of notional per millisecond, 95% CI [-0.004663,
+0.000695], an interval that crosses zero. Agents at 0.1, 1, 10 and 33 ms produce
bit-identical fills and PnL in all nine configurations tested. A cost does appear
at second scale: +0.000085 bps per millisecond between 1 s and 5 s, 95% CI
[+0.000048, +0.000133]. A 25-day extension set, untouched until the holdout
result was recorded, agrees.

The practical reading is that on a block-batched venue, latency spending should
be budgeted against the block interval rather than against a continuous
microsecond scale, and that the marginal value of going from tens of milliseconds
to single-digit milliseconds of reaction time is, in this sample, zero.

## 1. Why the question is different onchain

The standard framing of latency in market making comes from continuous-time
venues. A quote is exposed from the moment the market moves until the moment the
maker's cancel reaches the matching engine, and shortening that window has value
proportional to the amount of price discovery that happens inside it. Because
price discovery on a liquid continuous venue is roughly uniform in time at short
horizons, the value of latency is roughly linear in latency, and firms buy
microseconds.

Hyperliquid, like other consensus-ordered perpetual venues, does not run a
continuous clock. Transactions are ordered and executed in blocks. Two
consequences follow, and they point in the same direction:

1. Every fill inside one block carries the same timestamp, so a burst of maker
   fills from one aggressive order is a single instant as far as any observer is
   concerned.
2. Nothing happens between blocks. An observer polling at 1 ms and an observer
   polling at 30 ms receive the same events at the same time.

If (2) holds, sub-block reaction latency cannot have a price. This note checks
whether it holds and what the resulting latency curve looks like.

## 2. Data

Two datasets, collected live and held read-only.

- **Book.** Top 20 levels of each side of the ETH-perpetual book, one snapshot
  every 5 seconds, 639,307 snapshots over 39 days, 94.86% of the theoretical
  maximum.
- **Trades.** Every print, deduplicated on trade id, with the venue's own
  millisecond timestamp and the aggressor side. 1,009,422 prints.

Both carry the venue timestamp and a collector receive timestamp. All ordering is
on the venue timestamp; the receive timestamp is used only to measure collection
lag, which had a median of 366 ms and a 99th percentile of 667 ms on 2026-08-09.

The aggressor side field is reported by the venue as "A" or "B". Against the
prevailing snapshot best bid and offer, over the calibration days 92.88% of "A"
prints land at or below the bid and 93.87% of "B" prints land at or above the
ask; over all 39 days the figures are 89.14% and 89.72%. The residual is the
snapshot going stale inside its five second window rather than ambiguity about
the convention. "A" is a seller hitting the bid and "B" is a buyer lifting the
offer.

The instrument is tightly tick-constrained. The tick is 0.1 USD against a price
of 1853.1 to 2663.0 over the period, so one tick is 0.4093 basis points at the
median mid of 2443.4, and the spread is exactly one tick in 98.39% of snapshots.
The best bid is unchanged across 60.22% of consecutive 5-second snapshots over
the whole period and 74.04% over the calibration days. On the five holdout days
the median mid is 1887.0, one tick is 0.5300 basis points, the spread is one tick
in 99.46% of snapshots, and a maker at the touch is competing for a half spread
of 0.2650 basis points.

**The principal limitation of this data is stated once and applies throughout:
the book is a snapshot every five seconds, not an order-by-order feed.**
Individual orders, their identifiers, their arrival times and their queue
positions are not observable, and an order added and cancelled inside one window
leaves no trace. The only millisecond-resolution data is the trade tape.
Everything this note concludes about sub-33-millisecond behaviour is therefore a
statement about trade events, not about quote events.

## 3. The gap distribution

The central measurement needs no model. Take every pair of consecutive prints on
the four calibration days and histogram the gap.

Buckets are half open on the left. 110,263 gaps.

| gap | count | share | cumulative |
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

The 29.66% at zero is intra-block: several maker fills from one taker order. The
gap from 1 ms to 30 ms is empty, with a mode at 50 to 70 ms and a first
percentile of non-zero spacing at 66 ms. That is the block interval.

Over all 39 days rather than the four calibration days the hole is not exactly
empty: 26 gaps out of 1,009,458 fall between 1 and 30 ms, 0.0026%. That residue
is visible downstream, as the two-fill difference on the extension set reported
in section 5.1.

Two observers separated by less than about 33 milliseconds of reaction time
therefore see an identical event stream. No model is required for that
conclusion; it is a property of the timestamps.

## 4. Method

### 4.1 Engine

A limit order book with price levels in a flat array indexed by tick, per-level
FIFO queues on 32-bit slot indices, an open-addressing order-id map with
backward-shift deletion, and an occupancy bitmap scanned with count-trailing-zeros
to move the touch. Limit, market, cancel and modify with price-time priority.
Everything is integer fixed point. Roughly 700 lines of C++20.

### 4.2 Reconstruction, and why it is a test

For each 5-second window, every print is replayed as a marketable
immediate-or-cancel order at the printed price, and the result is then reconciled
to the next snapshot with adds, cancels and modifies. Both steps operate on a
shadow model built from the raw files; the shadow model never reads engine state.
The engine is then required to agree with the exchange's next published snapshot
exactly. Any bookkeeping error in the level array, the queues, the bitmap or the
id map surfaces as a mismatch rather than being silently corrected.

Result over all 39 days: **0 mismatches out of 25,570,480 compared level
positions**, across 639,262 windows and 24,183,264 engine commands, with zero
trades produced during reconciliation.

The same run produces a resolution measurement worth recording. Applying only the
trade tape to a snapshot and comparing to the next one leaves 82.93% of level
positions wrong and an 87.09% size error in the top five levels. Trades explain
very little of what this book does in five seconds; most of the change is quote
revision. Separately, 14.2% of traded volume printed at a price where the
five-second-old snapshot showed nothing resting, which is a direct reading of how
far the book moves between snapshots.

### 4.3 Agent

Avellaneda-Stoikov quoting with an inventory skew and an order-flow term:

    reservation  = mid - inventory * gamma * sigma^2 * tau + beta * ofi
    half_spread  = 0.5 * (gamma * sigma^2 * tau + (2/gamma) * ln(1 + gamma/k))

Quotes snap to the tick grid and clamp to the touch, since the spread is one tick
and improving it is not possible without crossing. A hard inventory limit stops
the agent quoting the side that would breach it.

The arrival decay k is fitted rather than assumed. In the Avellaneda-Stoikov
model the rate of fills at distance delta from the mid is A·exp(-k·delta). Every
print in the tape is a fill for whoever was resting at that price, so the
empirical distribution of a print's distance from the prevailing mid identifies
k, and for an exponential the maximum likelihood estimate is the reciprocal of
the mean distance. Over 110,264 calibration prints the mean distance is 0.11518
USD and the median is 0.05000 USD, with 76.49% of prints within half a tick of
the mid, giving k = 8.6820 per USD.

The order-flow coefficient beta is fitted by regressing the next-interval mid
change on exponentially decayed signed volume over the calibration days:
beta = 0.00008052 USD per ETH, correlation +0.01816, r-squared 0.00033 over
69,117 snapshot pairs. The signal is weak, which is itself relevant: even where
information does arrive, reacting to it faster is worth little.

### 4.4 Fill model

The agent's order is not in the book, so fills are inferred. Volume printing at
its price consumes the quantity ahead of it first. A print through its price
fills it, because price-time priority requires everything at that price to trade
first. The quantity ahead also falls as orders ahead are cancelled: a level that
shrinks by more than the volume that traded at it lost the difference to
cancellations, and the model attributes them uniformly along the queue, scaled by
a parameter kappa.

kappa is an assumption rather than a fit. There is no ground truth for an order
that was never placed, so calibrating it would be calibrating against nothing. It
is set to 1.0 for the primary results and reported at 0.0 and 0.5 as a
sensitivity. The treatment of a print through the quote price is likewise
reported under two rules, one that fills and one that does not.

The agent quotes 0.5 ETH against a touch holding a few hundred, and is assumed
not to move the market.

### 4.5 Latency

Reaction latency L is defined as the delay between a market event becoming
observable at the venue and the agent's response taking effect at the venue. An
event with venue timestamp t can change the agent's quotes only from t + L
onward; in between, its resting quotes reflect older information and can be
filled at those prices. Decisions pipeline, so a busy period does not freeze the
agent behind one in-flight order.

This is the agent's own reaction time. The venue's block interval and the network
time to the venue apply on top of it.

### 4.6 Design

Pre-registered before the simulator existed, with two dated amendments both made
before any holdout day was loaded. Calibration 2026-08-09 to 08-12. Holdout
2026-08-13 to 08-18 excluding a partial day, run once with the configuration
frozen. A 25-day extension set left untouched until the holdout result was
written down, then run once.

Fees are Hyperliquid perpetuals base tier, maker 0.015% and taker 0.045%, with
tier 4 at maker 0.000% and taker 0.028% reported alongside.

Confidence intervals come from a bootstrap that resamples the **paired**
per-period difference between latency L and the 0.1 ms baseline, not the two
levels independently. Both agents see the same market, and the day-to-day
variation in market conditions is orders of magnitude larger than the effect
being measured. Two resampling units are used: calendar days, as registered, and
hourly blocks with the agent marked to market at each hour boundary.

## 5. Results

### 5.1 The identity below the block interval

Across all nine (gamma, requote) configurations on the holdout, agents at 0.1, 1,
10 and 33 ms produce the same number of fills and the same PnL to the cent: the
paired difference is exactly 0.000000 USD and exactly 0 fills. On the 25-day
extension set the identity holds at gamma = 5 and very nearly holds at gamma = 0,
where 10 ms differs from 0.1 ms by 2 fills out of 76,644 and 0.08 USD out of
28,136, reflecting a handful of prints in those days spaced inside the gap.

This is the paper's central result and it is not statistical. It is a consequence
of the event timestamps, confirmed end to end through a full replay.

### 5.2 The curve

Primary configuration, five holdout days, base tier fees.

| latency | fills | notional USD | net PnL USD | edge bps | markout 5 s bps |
|---|---|---|---|---|---|
| 0.1 ms | 1451 | 814,823 | -215.21 | -2.6412 | -0.4937 |
| 1 ms | 1451 | 814,823 | -215.21 | -2.6412 | -0.4937 |
| 10 ms | 1451 | 814,823 | -215.21 | -2.6412 | -0.4937 |
| 33 ms | 1451 | 814,823 | -215.21 | -2.6412 | -0.4937 |
| 66 ms | 1443 | 814,961 | -212.38 | -2.6060 | -0.4970 |
| 100 ms | 1466 | 831,866 | -207.29 | -2.4918 | -0.5141 |
| 200 ms | 1508 | 830,369 | -203.50 | -2.4507 | -0.5132 |
| 500 ms | 1547 | 864,371 | -210.86 | -2.4394 | -0.5366 |
| 1000 ms | 1577 | 916,348 | -224.35 | -2.4483 | -0.5336 |
| 2000 ms | 1642 | 950,403 | -245.63 | -2.5844 | -0.5952 |
| 5000 ms | 1990 | 1,202,470 | -335.12 | -2.7869 | -0.6140 |

Paired PnL difference against the 0.1 ms baseline first excludes zero at 2000 ms
(-6.084 USD, 95% CI [-11.697, -2.356]) and is decisive at 5000 ms (-23.983 USD,
95% CI [-30.894, -17.302]). The hourly block bootstrap over 120 blocks agrees and
is tighter.

Headline slopes:

| range | slope, bps of notional per ms | 95% CI |
|---|---|---|
| 0.1 to 100 ms, holdout | -0.001495 | [-0.004663, +0.000695] |
| 0.1 to 100 ms, extension | -0.000598 | [-0.005000, +0.001453] |
| 1000 to 5000 ms, holdout | +0.000085 | [+0.000048, +0.000133] |

The registered range gives an interval crossing zero on both the holdout and the
untouched extension set. The second-scale range gives a positive slope whose
interval excludes zero.

### 5.3 Activity is endogenous to latency

The curve is not monotone in edge per unit notional, and the reason is worth
separating out. A slower agent holds a stale quote for longer, so it is filled
more often: fills rise from 1451 at 0.1 ms to 1990 at 5000 ms. When the marginal
fill is unprofitable, more fills means a worse total result even where the result
per unit of notional is flat or improving. Between 100 ms and 500 ms this agent
trades more and loses very slightly less per unit; past 1 second it trades more
and loses more on both measures.

Any study of latency in market making that reports only a per-notional ratio will
miss this, because the denominator moves with the treatment. Both are reported
here.

### 5.4 Sensitivities

The registered-range conclusion survives every variant tested. Headline slope and
interval:

| variant | slope bps/ms | 95% CI |
|---|---|---|
| primary | -0.001495 | [-0.004663, +0.000695] |
| kappa 0.0 | -0.000647 | [-0.002757, +0.000581] |
| kappa 0.5 | -0.000780 | [-0.002647, +0.000470] |
| sweep rule = Queue | -0.000007 | [-0.002231, +0.003689] |
| fee tier 4, maker 0.000% | -0.001495 | [-0.004663, +0.000694] |
| six-day holdout | -0.000536 | [-0.003401, +0.001268] |
| extension, 25 days | -0.000598 | [-0.005000, +0.001453] |

Under kappa 0 and 0.5 the first latency with a significant paired PnL loss moves
from 2000 ms to 66 ms. The effect at 66 ms under kappa 0 is -0.224 USD with a 95%
interval of [-0.417, -0.030], which is 22 cents over five days on 648,921 USD of
notional, or 0.0035 basis points: distinguishable from zero and economically
nothing. It is also not a gradient inside the millisecond range, since 0.1, 1, 10
and 33 ms remain identical under every kappa. It says that crossing one block
boundary is detectable when the fill model gives the agent no credit for queue
decay, which supports the mechanism rather than contradicting it.

### 5.5 Inventory skew

Skew on (gamma = 5) against skew off (gamma = 0), at 0.1 ms on the holdout: the
markout improves by about 0.40 basis points at every horizon from 1 s to 300 s
(5 s: -0.4937 against -0.8952, paired difference +0.4015, 95% CI [+0.1112,
+0.4716]), and peak absolute inventory falls from a daily mean of 25.48 ETH,
pinned at the 25 ETH limit, to 1.84 ETH. Net edge per unit notional is 0.27 basis
points worse with an interval crossing zero, because skew cuts the fill count by
a factor of 7.6 and the remaining fills do not pay for themselves after fees.

## 6. The strategy loses money, and why that is a separate fact

All 36 configurations in the calibration grid have negative net edge, from -1.94
to -3.34 basis points, and the primary configuration is still at -1.1403 basis
points with the maker fee set to zero. The cause is queue position, not fees and
not speed:

- the half spread available at the touch is 0.2650 basis points;
- the base tier maker fee is 1.5 basis points, 5.7 times that;
- the touch holds a few hundred ETH, so a joining order sits at the back and is
  filled mostly when the tape trades through its price, which is the adversely
  selected subset: between 41.0% and 50.1% of this agent's fills, depending on
  latency;
- its 5-second markout is -0.4937 basis points, against +0.0748 per print and
  +0.1140 weighted by size for the average maker over the same five days,
  computed directly from all 135,711 prints with no agent and no fill model
  involved.

Being at the back of a deep one-tick queue is worth about 0.57 basis points of
markout on this instrument. That is a larger number than anything latency does in
the registered range, by two orders of magnitude, and it is the right thing to
spend effort on here.

## 7. What would overturn this

- **A finer book feed.** The tape rules out a trade-side effect below 33 ms. It
  cannot rule out a quote-side effect, because the book is only observed every
  five seconds. If quotes move between blocks in a way that a faster agent could
  exploit, this data cannot see it. A 100 ms depth diff feed would settle it.
- **A continuous-time venue.** The mechanism is a property of block batching. The
  same experiment on a CEX matching engine should give a different answer and the
  gap distribution there should have no hole.
- **A front-of-queue maker.** The fill model places the agent at the back of the
  queue by construction. A maker already holding priority has a different markout
  profile and plausibly more to lose from slowness.
- **Size.** The no-impact assumption is defensible at 0.5 ETH and would not be at
  50.
- **A different regime.** 39 days of one instrument in one period. The block
  cadence itself is a protocol parameter and can change.

## 8. Reproducing

```
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure
./scripts/run_experiment.sh
```

About two minutes on four cores. Every number above is written into `results/` by
that script, and the fidelity check runs first so that a broken engine stops the
run rather than producing plausible-looking output.

## Sources

- Hyperliquid documentation, Trading > Fees.
  https://hyperliquid.gitbook.io/hyperliquid-docs/trading/fees, read 2026-09-14.
- M. Avellaneda and S. Stoikov, "High-frequency trading in a limit order book",
  Quantitative Finance 8(3), 2008, 217-224.
- D. E. Knuth, The Art of Computer Programming, Volume 3, section 6.4,
  Algorithm R, for the open-addressing deletion used in the order-id map.
