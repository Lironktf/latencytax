# The latency experiment

Back to the [README](../README.md). The pre-registration, its two amendments
and the results are in
[experiments/001_latency_tax](../experiments/001_latency_tax); a longer write up
is in [block-interval-latency.md](block-interval-latency.md).

## The experiment

Pre-registered before the simulator was written:
[hypothesis.md](../experiments/001_latency_tax/hypothesis.md), including two dated
amendments, both made before any holdout day was loaded. Results:
[results.md](../experiments/001_latency_tax/results.md).

Calibration 2026-08-09 to 08-12. Holdout 2026-08-13 to 08-18 excluding 08-15,
run once. An extension set of 25 further days was left untouched until the
holdout result was written down, then run once as a confirmation.

**Agent.** Avellaneda-Stoikov quoting with an inventory skew and an order flow
term, quotes snapped to the tick grid and clamped to the touch, and a hard
inventory limit that stops quoting the side that would breach it. The arrival
decay k is fitted from the tape: every print is a fill for whoever was resting at
that price, so the distribution of a print's distance from the prevailing mid
identifies it. Over 110,264 calibration prints, k = 8.6820 per USD.

**Fills.** The agent's order is not in the book, so whether it would have filled
is inferred. Volume printing at its price eats the queue in front of it first; a
print through its price fills it, because price-time priority means everything at
that price had to go first. The queue in front also shrinks as orders ahead are
cancelled, and a level that loses size beyond what traded at it lost the rest to
cancellations; the model attributes them uniformly along the queue, scaled by a
parameter kappa. kappa is an assumption, not a fit, since there is no ground
truth for an order that was never placed. It is reported at 0, 0.5 and 1.

**Latency.** An event with venue timestamp t can change the agent's quotes only
from t + L onward. In between, its resting quotes reflect older information and
can be filled at those prices. Decisions pipeline rather than queue behind one
another, so a busy period does not freeze the agent.

**Fees.** Hyperliquid perpetuals base tier, taker 0.045% and maker 0.015%, from
the [Hyperliquid documentation](https://hyperliquid.gitbook.io/hyperliquid-docs/trading/fees),
read 2026-09-14. Results are also given at tier 4, maker 0.000% and taker 0.028%,
which is where a market maker of any size would be.

### The headline

| range | latency tax | 95% CI | |
|---|---|---|---|
| 0.1 to 100 ms, holdout, 5 days | -0.001495 bps/ms | [-0.004663, +0.000695] | crosses zero |
| 0.1 to 100 ms, extension, 25 days | -0.000598 bps/ms | [-0.005000, +0.001453] | crosses zero |
| 1 s to 5 s, holdout | +0.000085 bps/ms | [+0.000048, +0.000133] | excludes zero |

Positive means edge lost as latency rises. Bootstrap over days, 10,000 draws,
resampling the paired per-day difference rather than the two levels separately,
because both agents see the same market and the day to day variation is far
larger than the effect.

### Why the millisecond range is flat

The trade tape has a hole in it. Across the 110,263 gaps between consecutive
prints on the calibration days, 29.66% are 0 to 1 ms and then there is **exactly
nothing until 30 ms**: zero observations in 1-2 ms, 2-5 ms, 5-10 ms, 10-20 ms and
20-30 ms. The first percentile of non-zero spacing is 66 ms. Prints that share a
timestamp are the separate maker fills of one aggressive order inside one block,
and the gaps between them are block intervals. Over all 39 days the hole is not
quite empty but close enough to say the same thing: 26 gaps out of 1,009,458 fall
between 1 and 30 ms, 0.0026%.

Reproduce with `scripts/tape_structure.py`.

So an agent driven by this feed has nothing to react to inside the first 33 ms,
and its behaviour at 0.1, 1, 10 and 33 ms is not similar, it is identical. That
is checked: across all nine configurations on the holdout the paired difference
between those latencies is exactly 0.000000 USD and exactly 0 fills.

On a venue that batches into blocks, reaction latency below the block interval is
not expensive, it is unobservable.

### Inventory skew

Same agent, skew coefficient at its calibrated value and at zero, 0.1 ms,
holdout.

| | skew on | skew off |
|---|---|---|
| markout 1 s | -0.4208 bps | -0.8042 bps |
| markout 5 s | -0.4937 bps | -0.8952 bps |
| markout 30 s | -0.6507 bps | -1.1019 bps |
| markout 300 s | -0.6867 bps | -1.1342 bps |
| net edge | -2.6412 bps | -2.3670 bps |
| peak absolute inventory, daily mean | 1.84 ETH | 25.48 ETH |
| fills | 1,451 | 11,044 |

Paired over days, the markout improvement is +0.3835 bps at 1 s
[95% CI +0.1200, +0.4669], +0.4015 at 5 s [+0.1112, +0.4716] and +0.4511 at 30 s
[+0.0263, +0.5707]; at 300 s it is +0.4475 but the interval crosses zero
[-0.0955, +0.9289]. The edge difference of -0.2741 bps has an interval that
crosses zero [-2.6237, +1.7017].

So skew buys two things and pays for one. It cuts adverse selection by about
0.40 bps at every horizon out to 30 seconds, and it keeps peak inventory at
1.84 ETH against 25.48 for the unskewed agent, which sits pinned at its 25 ETH
limit. It pays by cutting the fill count by a factor of 7.6, and the fills that
remain do not cover the fee.

### The strategy loses money

Worth saying plainly, because it frames everything above. All 36 configurations
in the calibration grid have negative net edge, from -1.94 to -3.34 bps, and the
primary configuration is still at -1.1403 bps with the maker fee set to zero. So
it is not a fee problem. The spread is one tick, which at the holdout median mid
of 1887.0 USD is 0.5300 bps, so a maker at the touch is competing for a half
spread of 0.2650 bps against a base tier maker fee of 1.5 bps. And the touch
holds a few hundred ETH, so an agent joining it sits at the back and fills mostly
when the tape trades through its price, which is the adversely selected subset:
41.0% to 50.1% of this agent's fills, depending on latency. Its 5 second markout
is -0.4937 bps, against +0.0748 for the average maker on the same five days
computed straight from all 135,711 prints with no agent and no fill model
involved. Sitting at the back of the queue is worth about 0.57 bps of markout,
which is two orders of magnitude more than anything latency does in the
registered range.

This says that joining the back of a deep one-tick queue is unprofitable on this
instrument, and that making that decision faster does not help. It says nothing
about a maker who already holds queue position.
