# 001: what does a millisecond of reaction latency cost a market maker?

Pre-registered 2026-09-14, before the simulator was written and before any
simulation was run on any day.

## Question

A market maker posting two sided quotes is exposed between the moment the market
moves and the moment its own quotes move. Reaction latency is the length of that
exposure. The question is how much edge, in basis points of traded notional, a
maker gives up per additional millisecond of reaction latency on Hyperliquid
ETH-perp.

## Hypothesis

H1. Net maker edge in basis points of traded notional is a decreasing function
of reaction latency L.

H0. Net maker edge does not depend on L over the tested range.

## What counts as the answer

The primary metric is

    edge_bps(L) = net PnL over the period / gross traded notional * 1e4

with net PnL including Hyperliquid maker fees on every fill and taker fees on the
forced flatten at the end of each day. Net PnL is cash plus inventory marked at
the last snapshot mid of the day.

The headline number is the slope

    latency tax = -(edge_bps(100 ms) - edge_bps(0.1 ms)) / (100 - 0.1)

in basis points per millisecond, over the four latencies named in the plan
(0.1, 1, 10, 100 ms), with a confidence interval from a bootstrap that resamples
holdout days with replacement (10,000 draws, percentile interval).

The sweep is extended past the four required points to 33, 66, 200, 500, 1000,
2000 and 5000 ms so that the shape of the curve, and any knee in it, is visible
rather than assumed. The headline slope is still computed over 0.1 to 100 ms as
specified above.

Secondary metric: markout, defined for a fill at venue time t, agent side s
(+1 bought, -1 sold) and fill price p as

    markout_h = s * (mid(t+h) - p) / p * 1e4   basis points

where mid(t+h) is the mid of the first snapshot at or after t+h. Reported for
h = 1 s, 5 s and 30 s. The 1 s figure is bounded below by the 5 second snapshot
grid and is reported with that caveat rather than dropped.

Third result: markout and net edge with the inventory skew term on versus off
(the same agent with the skew coefficient set to its calibrated value and to
zero).

## Prediction, stated in advance

Two properties of the data were measured on the calibration period before this
document was written, and they drive the prediction.

1. The trade tape has no prints separated by more than 1 ms and less than 33 ms.
   Over 110,264 prints on the calibration days the gap histogram is: 29.66% at
   0 to 1 ms, exactly zero between 1 and 30 ms, then 0.03% at 30 to 50 ms and
   1.55% at 50 to 70 ms. The first percentile of non-zero spacing is 66 ms. This
   is the venue's block interval: prints inside one block share a timestamp.

2. Signed tape volume is a weak predictor of the next mid move. Correlation of
   signed volume over the preceding second with the mid return over the next
   5 seconds is +0.0011 on the calibration days. The strongest pairing measured
   was a 60 second volume window against the 30 second forward return at
   +0.0278.

From (1), no information at all arrives between 1 ms and 33 ms of venue time, so
an agent driven by this data must behave identically at 0.1, 1 and 10 ms. Those
three points are expected to be bit identical, and that will be checked as a
consistency test on the simulator rather than reported as a finding. From (2),
even where information does arrive, reacting to it faster is worth very little.

Predicted result: the latency tax over 0.1 to 100 ms is statistically
indistinguishable from zero, with a confidence interval that crosses zero. A
measurable cost is expected only at latencies comparable to the 5 second
snapshot interval, where the agent quotes against a book it can no longer see.

If that is what comes out, it is the result and it will be reported as such. The
claim being tested is about this venue, this instrument and this data resolution,
not about latency in general.

## Data

Hyperliquid ETH-perp, collected live.

- `00_raw/live/hyperliquid/l2book/ETH`: top 20 levels of each side, one snapshot
  every 5 seconds.
- `00_raw/live/hyperliquid/trades/ETH`: every print, deduplicated on trade id,
  with the venue's own millisecond timestamp.

Both are read only. Nothing in this experiment writes to them.

## Periods

- Calibration: 2026-08-09 through 2026-08-12, four days.
- Holdout: 2026-08-13 through 2026-08-18, six days.
- Excluded: 2026-08-07 (9.6% of the day collected) and 2026-08-08 and 2026-08-15
  (99.8%). 08-07 and 08-08 fall before the calibration window anyway. 08-15 sits
  inside the holdout window and is dropped from the holdout, leaving five days.
- Untouched extension: 2026-08-19 through 2026-09-14. These days exist in the
  same dataset and are deliberately not used for calibration or for the headline.
  They are run once, after the holdout result is written down, as a confirmation
  set, and reported separately.

Every parameter is chosen on the calibration days. The holdout is run once, with
the configuration frozen, and whatever comes out is what gets reported.

## Agent

Avellaneda-Stoikov style two sided quoting.

    reservation = mid - inventory * gamma * sigma^2 * tau + beta * ofi
    half_spread = 0.5 * (gamma * sigma^2 * tau + (2/gamma) * ln(1 + gamma/k))

sigma is an exponentially weighted estimate of mid volatility from the snapshot
series. Quotes are snapped to the 0.1 tick grid and floored at the touch. The
agent stops quoting the side that would push inventory past its limit.

Parameters chosen on the calibration days only, by grid search on net edge:

- gamma, inventory risk aversion: {0, 0.05, 0.2, 1.0}
- base offset from the touch, in ticks: {0, 1}
- beta, the order flow imbalance coefficient: {0, calibrated}
- quote size: 0.5 ETH, fixed, not searched
- inventory limit: 25 ETH, fixed, not searched

The grid is 16 points. It is small on purpose: with five holdout days there is
no room to spend degrees of freedom on parameter search.

## Fill model

The agent's resting order at price P fills as cumulative real tape volume works
through the quantity that was ahead of it.

    queue_ahead at join = the resting quantity at P in the reconstructed book at
                          the moment the order becomes live
    a print at P on the opposite side of the agent's order consumes queue_ahead
      first, then the agent's order
    a print that trades through P fills the agent's order completely, because
      price-time priority means everything at P has to go first
    queue_ahead also falls as orders ahead are cancelled. Level size changes
      between snapshots that are not explained by trades are cancellations.
      The fraction of them that happened ahead of the agent is taken to be
      queue_ahead / level_size, that is, cancellations uniformly distributed
      along the queue, scaled by kappa.

kappa is an assumption, not a fit. There is no observable ground truth for an
order this simulation never actually placed, so calibrating it would be
calibrating against nothing. It is set to 1.0, the neutral uniform assumption,
and the holdout is also reported at kappa = 0.0 and 0.5 as a sensitivity band.

The agent is assumed not to move the market. Its quote size of 0.5 ETH sits
against a touch that holds a few hundred ETH, so this is a small assumption, but
it is an assumption.

## Latency

Reaction latency L is the delay between a market event becoming observable at the
venue and the agent's response taking effect at the venue. An event with venue
timestamp t can change the agent's quotes only from t + L onward. Between t and
t + L the agent's resting quotes reflect information from before t and can be
filled at those prices.

This is the agent's own reaction time. It is not the venue's block interval, and
it is not network time to the venue, both of which apply on top of it in reality.

## Fees

Hyperliquid perpetuals, base tier (tier 0): taker 0.045%, maker 0.015%. Source:
Hyperliquid documentation, Trading > Fees,
https://hyperliquid.gitbook.io/hyperliquid-docs/trading/fees, read 2026-09-14.

The base tier maker fee of 1.5 bps is larger than the entire half spread on this
instrument, which is 0.264 bps at the median. Results are therefore also reported
at tier 4 (maker 0.000%, taker 0.028%), the tier an actual market maker of any
size would be in. The fee tier changes the level of edge_bps; the latency slope
is reported at both so the reader can see whether it changes the answer.

## Things that would make this result wrong

- The book is a 5 second snapshot. Everything that happens to the book between
  snapshots is invisible except through the trade tape. Adverse selection inside
  the first 5 seconds after a fill cannot be measured here at all.
- Orders added and cancelled inside a single 5 second window never existed as far
  as this reconstruction is concerned.
- Queue position inside a price level is not observable in an L2 feed. The fill
  model is a model.
- The agent is a price taker in the sense that it is assumed not to change
  anyone else's behaviour. At 0.5 ETH that is reasonable and still unverifiable.
- Five holdout days is a small sample. The bootstrap resamples days, so the
  interval reflects that, but it cannot manufacture information that is not
  there.
- A backtest is not evidence of live profitability, and nothing here places an
  order anywhere.

---

## Amendment 1, 2026-09-14

Made after writing the simulator and after exploratory runs on the calibration
days, and before any run on any holdout day. No holdout day had been loaded by
the simulator at the time this was written.

Four changes.

1. The gamma grid is rescaled to {0, 5, 20} from {0, 0.05, 0.2, 1.0}. The
   original numbers were written before the volatility units were computed. On
   the calibration days the exponentially weighted standard deviation of the mid
   over one 5 second interval is about 0.012 USD, so at tau = 60 s the inventory
   term gamma * inventory * sigma^2 * tau is worth about 0.0044 USD per ETH of
   inventory per unit of gamma. At the original top of the grid, gamma = 1.0, a
   maximum 25 ETH position would skew the reservation price by 0.11 USD, about
   one tick. The grid could not have distinguished any skew setting from no skew
   at all. The rescaled grid spans 0 to about 5.5 ticks of skew at the inventory
   limit. Three values instead of four, to hold the grid size down.

2. A parameter is added to the agent: requote_ticks, the drift in ticks the
   agent tolerates before it cancels a resting quote and rejoins at a new price.
   Grid {0, 1, 3}. Reason: the spread on this instrument is one tick 99.8% of
   the time and the touch holds a few hundred ETH, so an agent that re-pegs
   every time the touch moves is permanently at the back of a queue it never
   advances through. In an exploratory calibration run, 73% of that agent's
   fills came from the tape trading through its price, which is the adversely
   selected subset. Whether to hold queue position is a real decision a market
   maker makes and the experiment should not assume it away. A quote that has
   become crossable, or that the inventory limit says to pull, is still acted on
   immediately regardless of this parameter.

3. The fill model's treatment of a print at a better price than the resting
   order is promoted from an implementation detail to a reported sensitivity,
   with two rules, Through and Queue, defined in src/sim/fill_model.hpp. Through
   is the primary. Neither is verifiable from an L2 feed. The holdout is reported
   under both.

4. beta is fixed at the value from the calibration regression rather than left
   free: 0.00008052 USD per ETH of decayed signed volume, from 69,117 snapshot
   pairs over the four calibration days, correlation +0.01816, r-squared
   0.00033. The grid keeps {0, 0.00008052}.

Calibration grid after these changes: gamma {0, 5, 20} x offset {0, 1} x
requote {0, 1, 3} x beta {0, 0.00008052} = 36 points, selected on pooled net
edge in basis points over the four calibration days. The holdout is then run
once at the selected point. The latency curve is also reported for the runner up
configurations, so that the reader can see whether the conclusion about latency
depends on which agent was picked.
