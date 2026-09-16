# 003: does a wallet's toxicity persist, and is it worth acting on?

Pre-registered 2026-09-16, before any evaluation day was scored.

## What is already known, and what that costs

Hyperliquid's trade feed names both counterparties on every print. Over the five
days 2026-08-13 to 08-18, all 136,171 prints carry two addresses, there are
15,027 distinct addresses, and the top 50 touch 51.5% of all print sides.

An exploratory pass has already been run, and it used 2026-08-09 to 08-12 to
score wallets and 2026-08-13 to 08-18 to check them. It found:

- real dispersion in 30 second aggressor markout across wallets with at least
  300 prints, from +2.900 bps to -1.804 bps, p10 -0.684 and p90 +0.796;
- weak rank persistence, Spearman +0.333 at a 100 print threshold and +0.296 at
  300, with Pearson near zero;
- a toxic-half against benign-half gap on the later period of +0.162 bps
  [-0.354, +0.587] and +0.372 bps [-0.244, +0.825].

Every one of those intervals crosses zero. The exploration is suggestive and
settles nothing, which is why this document exists.

**Those nine days are spent.** They may be used to *estimate* wallet scores, the
way any history is, but no result on them counts as evidence here. The
evaluation period below has never been scored for this question.

## Hypotheses

**H1, persistence.** A wallet's aggressive-flow toxicity, measured over a
trailing window, predicts its toxicity on the following day.

**H2, separation.** Flow from wallets scored toxic on the trailing window has a
worse markout for the maker on the following day than flow from wallets scored
benign.

H0 for both: no relationship. The exploration cannot reject H0 and neither may
this, which would be a result.

## Definitions, fixed in advance

**Aggressor.** Hyperliquid lists `users` as [buyer, seller]. Side `B` means the
buyer aggressed and side `A` means the seller did, so the aggressor is `users[0]`
on a `B` print and `users[1]` on an `A` print. This convention is checked against
the book in `scripts/tape_structure.py`, where 89% of `A` prints land at or below
the prevailing bid and 90% of `B` prints at or above the ask.

**Markout.** For a print at venue time t, aggressor sign s (+1 bought, -1 sold)
and price p, the aggressor markout at horizon h is

    s * (mid(t + h) - p) / p * 1e4   basis points

where mid(t+h) is the mid of the first snapshot at or after t + h. Positive means
the aggressor was right and whoever was resting got picked off. **h = 30 s is the
primary**, chosen because it is what the exploration used; 5 s and 300 s are
reported alongside and are secondary.

**Wallet score.** For wallet w on day D, using the trailing K days
[D-K, D-1]: the size weighted mean aggressor markout over every print in that
window where w was the aggressor. **K = 14**, fixed now, not searched.

**Eligibility.** A wallet is scored on day D only if it has at least **200
prints** in the trailing window and at least **20 prints** on day D itself. Both
thresholds are fixed now. A sensitivity at 100 and 500 trailing prints is
reported and is not the headline.

## Periods

- **Score history**: any day from 2026-08-08 onward may feed a trailing window.
- **Evaluation**: **2026-08-25 to 2026-09-13**, 20 days. The first evaluable day
  is 08-25 because 14 trailing days from 08-25 reaches back to 08-11, and days
  before that are either partial or absent.
- **Excluded**: 2026-08-07 (9.6% collected), 2026-08-19 (31.0%), 2026-09-14 (no
  trade file). Partial days inside a trailing window are used as history, since a
  trailing window is a sum and a short day contributes less to it.

Every evaluation day is scored exactly once, by a walk forward that never looks
at day D or later when scoring for day D.

## Statistics

**Primary for H1**: the Spearman rank correlation between a wallet's trailing
score and its next-day size weighted markout, computed within each evaluation day
and then averaged across days. Confidence interval from a bootstrap that
resamples **evaluation days** with replacement, 10,000 draws, percentile
interval. Days are the resampling unit because wallets recur across days and
resampling wallets would treat the same wallet on twenty days as twenty
independent observations.

**Primary for H2**: the difference in next-day size weighted markout between
flow from wallets in the top quartile of trailing score and flow from wallets in
the bottom quartile, pooled over evaluation days, with the same day level
bootstrap.

**Multiple testing.** Two primaries and three horizons is six numbers. The two
listed above at h = 30 s are the pre-registered primaries; everything else is
secondary and labelled as such. No correction is applied to the two primaries and
none is claimed.

## Prediction, stated in advance

The exploration's Spearman of about +0.3 came with an interval crossing zero on
nine days and roughly thirty wallets. Twenty evaluation days with a fourteen day
trailing window is a materially larger sample, so if the effect is real at that
size it should now separate from zero, and if it was sampling noise the interval
should tighten around zero rather than move.

I expect H1 to survive weakly and H2 to be marginal. If both intervals cross
zero that is the result and it will be reported as such.

## What happens if it holds

If and only if both primaries clear zero, a third step follows, and it is **not**
pre-registered here because its design depends on what the first two show: wire
the score into the market making agent so it withdraws quotes after a top
quartile wallet has been active, and measure the markout change the same way
experiment 002 measured its gate. That would be a separate document with its own
holdout.

## Things that would make this wrong

- **Wallets are not identities.** One trader can use many addresses and one
  address can be a venue-side account that nets many traders. Nothing here can
  see through that, and a persistent score may be measuring an account's
  *function* rather than a trader's skill.
- **Survivorship.** A wallet needs prints in both windows to be scored, which
  selects for wallets that keep trading. Whether that biases toxicity is not
  something this design can answer.
- **One instrument, one venue, 39 days.** ETH-perp on Hyperliquid in one period.
- **The 5 second snapshot grid bounds every markout**, exactly as in 001.
- **A persistent score is not a tradable edge.** Even if toxic wallets stay
  toxic, acting on it requires recognising them fast enough to matter, and 001
  established that this venue gives you no reaction advantage below its block
  interval.
