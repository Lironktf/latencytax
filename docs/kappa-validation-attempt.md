# Trying to validate kappa against L3 data, and failing

Back to the [README](../README.md).

## The assumption

The fill model in [experiment 001](../experiments/001_latency_tax) contains one
number that is an assumption rather than a measurement. When a price level
shrinks by more than the volume that traded at it, the difference was cancelled,
and the model attributes a fraction **kappa** of those cancellations to orders
*ahead* of the agent in the queue. kappa = 1 means cancellations are spread
uniformly along the queue; kappa = 0 means they all came from behind.

001 reports its results at kappa 0, 0.5 and 1 and says plainly that there is no
ground truth for an order that was never placed. An L2 feed shows a level's total
size and nothing about the orders inside it.

**L3 data would settle it.** With per-order identifiers you can watch individual
orders disappear, work out where each one sat in its queue, and measure the
distribution directly: if cancellations are uniform by quantity, the mean
position of a cancelled order is halfway along its level, and kappa = 1 is right.

## What the archive has

One dataset with per-order identifiers:
`00_raw/live/lighter_tape/lighter_rh_books`. Lighter, two markets, rhSPY/USDC and
rhQQQ/USDC, a snapshot every 30 seconds, ten days, 40 MB. Each order carries an
index, an owner account, its original and remaining size, and its price, so on
the face of it this is exactly the feed the question needs.

## Why it cannot answer the question

Over 4,702 snapshots and 365,261 observed price levels:

| | |
|---|---|
| levels holding exactly one order | **349,676 (95.7%)** |
| levels holding two orders | 15,585 (4.27%) |
| levels holding three or more | **0** |
| distinct owner accounts in the entire book | **10** |
| share of all order observations held by the largest account | **54.3%** |

**There are no queues to measure.** kappa describes how cancellations distribute
*within* a price level, and in this book a price level is almost always one
order. An order that is alone at its price has nothing ahead of it and nothing
behind it, so its removal carries no information about queue position. The 4.27%
of levels with two orders would give a binary observation, ahead or behind, on a
sample drawn from ten accounts of which one is more than half the book.

That last part matters as much as the first. Even if the queues existed, this is
two thinly traded synthetic-equity perpetuals whose book is largely one market
maker quoting against itself. Nothing measured there would transfer to
Hyperliquid ETH-perp, where the touch holds a few hundred ETH across twenty-odd
orders.

## What this changes

Nothing about 001's results, which already report the full kappa range rather
than committing to a value. What it changes is the status of the sentence "this
could be validated with L3 data": it could be, and not with the L3 data available
here.

The honest position stays what it was. kappa is an assumption, the holdout is
reported at 0, 0.5 and 1, and the latency conclusion is the same at all three.
[Experiment 002](../experiments/002_queue_model) measures the half of queue
dynamics that *does* have ground truth in an L2 feed, which is whether the
quantity in front of an order trades away, and gets AUC 0.700 on it. The
cancellation half remains unmeasured.

Recorded because twenty minutes spent eliminating an approach is worth writing
down, and because "we could validate that with better data" is the kind of
sentence that should come with a check of whether the better data exists.
