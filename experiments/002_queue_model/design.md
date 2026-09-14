# 002: how predictable is queue clearing, and does predicting it help?

## A note on status, first

This is **not** pre-registered the way 001 was. 001's hypothesis was written
before its simulator existed and its holdout was opened once. This document was
written after the pipeline was built and after the test set had been scored.
Saying otherwise would be worth less than nothing, so here is the exact account:

The test set was scored three times.

1. With a broken feature. `f[31]` took `log1p` of a signed price drift, which is
   NaN whenever the mid has fallen, so every model that used the full vector
   trained to NaN and scored NaN. The baselines beside them were unaffected and
   were fine, which is how it was caught. No number from that run was used for
   anything except finding the bug.
2. After fixing the feature, with hyperparameters that had not been searched.
   That run showed the full logistic losing to a single feature baseline on log
   loss and being badly miscalibrated. The response was to add a hyperparameter
   search and a recalibration step, both fitted **on the validation day only**.
   No test number entered either.
3. Once more, with the search and the calibration in place. That is the run
   reported here.

What was held to throughout: every modelling choice, which features exist, the
FTRL hyperparameters, the MLP width and learning rate and number of epochs, the
Platt scaling, and the gate threshold used in the simulator, was made on the
calibration period or on the validation day inside it. The test set is the same
holdout 001 used and nothing was tuned against it.

Treat the numbers below as a holdout result with a documented audit trail rather
than as a pre-registered one.

## The question

Experiment 001's fill model contains one parameter that is an assumption rather
than a measurement. When a price level shrinks by more than the volume that
traded at it, the difference was cancelled, and the model attributes a fraction
kappa of those cancellations to orders in front of the agent. 001 reported
results at kappa 0, 0.5 and 1 because there is no ground truth for an order that
was never placed.

There is, however, ground truth for the part of the same question that only
involves trading:

> An order joins a price level at time t with quantity q already resting in
> front of it. Within the next 30 seconds, does enough volume trade at that
> price to work through q?

That is fully observable from the tape. It is also the half of queue dynamics a
market maker cares most about, because volume working through the queue is what
produces a fill, and it is precisely what the constant kappa cannot express: a
constant has no per level opinion at all.

Two things are measured.

**H1.** Queue clearing is predictable from the state of the book and the recent
tape at the moment of joining, better than the constant the fill model assumes.
The constant has AUC 0.5 by construction.

**H2.** A model of it, used to decide which levels to join, reduces the agent's
adverse selection out of sample.

And one thing is checked: whether 001's conclusion about latency survives an
agent that uses the model.

## Labels

For a level at price P on side S at snapshot time t, scan the tape over
(t, t + 30 s]:

- a print from the opposite aggressor at price P adds to the cumulative volume;
- a print that trades *through* P means everything resting at P had to go first,
  so the answer is yes immediately;
- the label is 1 if the cumulative volume reaches q.

No simulation, no fill model and no agent sits between the data and the label.

## Features

32 measured, then 32 one hot buckets of the three quantities that carry most of
the signal, giving 64. Documented by index in `src/ml/features.hpp`. They cover
the level (size, order count, mean order size, distance from the touch, share of
the top five), the book (spread, imbalance at one and five levels), the tape
(exponentially time decayed volume on both sides at 5, 30 and 300 seconds, order
flow imbalance at two horizons, arrival rate), the price path (realised
volatility, drift over one and six intervals), and the query itself (q, q as a
fraction of the level, and q measured in 30 second windows of consuming volume).

Every feature is computed from state that has seen the snapshot at t and every
trade up to t, and nothing after. The generator refuses to write a dataset
containing a non finite feature.

## Models

All four are written from scratch against hand written AVX2 kernels in
`src/ml/kernels.hpp`, with a scalar reference beside every kernel and the
backward pass checked against finite differences.

- **base rate**: the training positive rate, for every sample.
- **one feature**: logistic regression on the single quantity the question
  reduces to, `log1p` of q measured in 30 second windows of consuming volume.
- **logistic**: FTRL-Proximal over all 64 features, one online pass in time
  order. Reference: McMahan et al., KDD 2013.
- **mlp**: 64 to H to 1, ReLU, Adam, forward and backward by hand.

## Split

- Fit: 2026-08-09 to 08-11.
- Validation: 2026-08-12. Every hyperparameter, the epoch count and the Platt
  recalibration come from this day and nothing else.
- Test: 2026-08-13 to 08-18 excluding 08-15, the same holdout as 001.

## Using it

The simulator gains `--queue-model` and `--gate`. Before joining a level the
agent asks the model whether the quantity already resting there is likely to
trade away inside the horizon, and stands aside when the answer is below the
gate. The gate was chosen on the calibration days by 5 second markout, subject
to keeping at least 1,000 fills over the four days, which selected 0.40.

The latency sweep is then re-run on the holdout with the gated agent, to see
whether 001's conclusion depends on the agent having no model.
