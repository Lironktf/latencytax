# 002 results: queue clearing is predictable, and predicting it cuts adverse selection

Status: **both hypotheses supported, and 001's conclusion about latency
survives.** Read `design.md` first, in particular the note at the top about what
was scored against the test set and when. This is a holdout result with an audit
trail, not a pre-registered one.

Every number comes from `scripts/run_queue_model.sh`.

---

## 1. Is it predictable?

Test set: 1,035,984 samples over five holdout days, 24.56% positive.

| model | log loss | Brier | AUC |
|---|---|---|---|
| base rate | 0.55763 | 0.18535 | 0.5000 |
| one feature | 0.53203 | 0.17555 | 0.6754 |
| **logistic, 64 features** | **0.52132** | **0.17074** | **0.7001** |
| mlp, 64-32-1 | 0.52761 | 0.17147 | 0.6584 |

H1 holds. Against the constant the fill model currently assumes, which has an
AUC of 0.5 by construction because it has no per level opinion, a linear model on
observable state reaches 0.7001.

Three things in that table are worth saying out loud rather than leaving for a
reader to notice.

**Most of it is one ratio.** A logistic regression on the single quantity
`log1p(q / consuming volume over the last 30 s)` already gets AUC 0.6754. The
other 63 features are worth 0.0247 of AUC and 0.011 of log loss between them.
That is a real improvement and it is not a large one.

**The neural network loses.** The MLP is worse than the linear model on every
metric, on the validation day and on the test set, at every width and learning
rate searched. The bucket indicators in the feature expansion already give the
linear model the one bend the problem needs, and past that there is no structure
for a hidden layer to find. This is reported as the result rather than buried,
and it is the reason the model shipped into the simulator is the logistic one.

**It transfers across days.** Per day on the test set, after recalibration:

| day | samples | positive rate | log loss | AUC |
|---|---|---|---|---|
| 2026-08-13 | 207,204 | 0.2923 | 0.56556 | 0.6710 |
| 2026-08-14 | 207,192 | 0.2811 | 0.54264 | 0.7029 |
| 2026-08-16 | 207,192 | 0.1272 | 0.43755 | 0.7367 |
| 2026-08-17 | 207,192 | 0.2705 | 0.53979 | 0.6946 |
| 2026-08-18 | 207,204 | 0.2570 | 0.52105 | 0.7152 |

AUC runs from 0.671 to 0.737 with no bad day, across days whose base rate moves
by a factor of 2.3.

**Calibration needed help and still is not perfect.** The positive rate is 0.2534
on the fit days, 0.3244 on the validation day and 0.2456 on the test set. A model
fitted on one regime and applied to another is well ranked but wrongly scaled, so
a two parameter Platt scaling is fitted on the validation day. Because the
validation day has a higher base rate than the test period, the scaled model
still over predicts in the middle of the range on test: at a predicted 0.35 the
observed rate is 0.25. The ranking is what the simulator uses, and the ranking is
sound; anyone wanting the probabilities themselves should refit the scaling on a
period closer to the one they care about.

## 2. Does predicting it help?

The agent asks the model, before joining a level, whether the quantity already
resting there is likely to trade away. Gate 0.40, chosen on the calibration days
and frozen. Paired by day over the same five holdout days, 10,000 bootstrap
draws.

| metric | ungated | gated | difference | 95% CI |
|---|---|---|---|---|
| markout 1 s, bps | -0.4208 | -0.2353 | **+0.1854** | **[+0.0726, +0.4356]** |
| markout 5 s, bps | -0.4937 | -0.3148 | **+0.1789** | **[+0.0502, +0.4467]** |
| markout 30 s, bps | -0.6507 | -0.5732 | +0.0775 | [-0.1079, +0.4180] |
| markout 300 s, bps | -0.6867 | -0.3669 | +0.3198 | [-0.4874, +1.7533] |
| swept share of fills | 0.4397 | 0.2481 | **-0.1916** | **[-0.3530, -0.1072]** |
| net edge, bps | -2.6412 | -2.4583 | +0.1829 | [-0.1590, +0.5768] |
| fills | 1,451 | 1,701 | | |
| net PnL, USD | -215.21 | -175.52 | | |

H2 holds at the horizons where the data has resolution. The share of fills that
come from the tape trading *through* the agent's price, which is the adversely
selected subset, falls from 44.0% to 24.8% with an interval that excludes zero.
Short horizon markout improves by about 0.18 bps at 1 and 5 seconds, also
excluding zero. The 30 and 300 second intervals cross zero; the 300 second one is
very wide because few fills have five minutes of day left after them.

The model gets these while taking **more** fills, not fewer: 1,701 against 1,451.
It is not simply trading less.

Net edge improves by 0.18 bps with an interval that crosses zero, and remains
negative at -2.46 bps. The 1.5 bps maker fee is still five times the half spread,
and no amount of queue selection fixes that. What the model does is exactly what
it was asked to do, which is pick better queues, and that is visible in the
markout rather than in the PnL.

## 3. Does 001's answer change?

No.

| agent | latency tax, 0.1 to 100 ms | 95% CI | |
|---|---|---|---|
| 001 primary, no model | -0.001495 bps/ms | [-0.004663, +0.000695] | crosses zero |
| model gated | +0.000693 bps/ms | [-0.001182, +0.002322] | crosses zero |

The gated agent is still bit identical at 0.1, 1, 10 and 33 ms, for the same
reason: the tape has no events in that range to react to. The sign of the point
estimate flips and the interval still contains zero, which is what an effect of
zero looks like when it is measured twice.

One difference is worth reporting. With the gate on, the first latency whose
paired PnL difference excludes zero moves from 2000 ms to 66 ms: -1.508 USD
[-2.634, -0.382]. That is 1.5 dollars over five days on 714,000 dollars of
notional, or 0.021 bps. Distinguishable from zero and economically nothing, the
same pattern 001 found under kappa 0. Crossing one block boundary is detectable
once the agent is selective enough for its fills to matter individually; it is
still not a gradient inside the millisecond range.

## 4. The kernels

All hand written, with a scalar reference beside each and the backward pass
checked against central differences. Measured with `build/bench --kernels` on the
same pinned core as the engine benchmark, 64 wide vectors, a 32 by 64 layer.

| kernel | scalar | vector | speedup |
|---|---|---|---|
| dot, 64 | 50.08 ns | 6.42 ns | 7.80x |
| axpy, 64 | 5.69 ns | 5.54 ns | 1.03x |
| gemv, 32 by 64 | 1534.40 ns | 176.13 ns | 8.71x |
| Adam step, 2048 weights | 7883.66 ns | 2547.04 ns | 3.10x |

| operation | ns/call | GFLOP/s |
|---|---|---|
| logistic inference, 64 | 19.96 | 6.41 |
| mlp inference, 64-32-1 | 275.43 | 15.10 |
| mlp training step | 2018.66 | 6.18 |

### The first version of this table was wrong

It reported the 64 wide dot product at 103 GFLOP/s. This core has two AVX2 fused
multiply add units, eight lanes each, two flops per fused multiply add, at
2.4 GHz, so its ceiling is 76.8 GFLOP/s. A microbenchmark that beats the
machine's peak is not a fast kernel, it is a deleted one: both input vectors were
loop invariant, so the compiler hoisted the entire call out of the repetition
loop and timed an empty loop. Accumulating the result into a checksum, which the
benchmark already did, prevents the call being removed but does nothing to stop
it being hoisted.

The fix is an optimisation barrier on the inputs and the result inside the timed
lambda, and the benchmark now prints the machine's peak underneath the table so
the next impossible number is obvious. Every figure above is from after the fix.
The scalar numbers moved more than the vector ones, which is what you would
expect: a scalar loop is the easier one to hoist.

### What the numbers say

**Vectorising the dot product is worth 7.80x** and lands at 19.93 GFLOP/s, about
a quarter of the machine's peak. A 64 element vector is short enough that loop
setup and the horizontal reduction at the end are a real fraction of the work.

**`axpy` gets 1.03x, and that is correct.** It reads two vectors and writes one
for two flops per element, so it is bound by memory bandwidth rather than
arithmetic, and widening the arithmetic cannot help. Reported rather than
dropped, because a kernel table where everything is faster is a table whose
entries were chosen.

**Blocking the matrix vector product four rows at a time is worth 1.24x**, on top
of the 7.0x that vectorising the inner product already gave: 1534 ns scalar, 218
ns a row at a time, 176 ns blocked. A row at a time walks the whole input vector
once per row; four rows at a time loads each chunk of input once and uses it four
times, taking 32 passes over the input down to 8. The benchmark reports all three
so the two effects are not conflated.

**Widening the Adam step is worth 3.10x**, and it mattered more than the ratio
suggests because it was the single slowest thing in the model. The scalar form
spends almost all of its time in one square root and one division per weight,
each roughly fifteen cycles, neither of which pipelines with anything when it
sits alone in a loop body. Eight lanes at a time hides most of it.

**One inference is dominated by `exp`.** The dot product inside the logistic
costs 6.42 ns and the whole inference costs 19.96 ns; the difference is the
exponential in the sigmoid. The gate in the simulator needs a comparison, not a
probability, so it could compare the logit against a transformed threshold and
skip the exponential. It does not, because the gate is called once per requote
and is nowhere near a hot path. Worth knowing, not worth doing.

## 5. Limitations

- **The label only covers trading.** Cancellations are still unobservable and
  kappa is still an assumption. This measures the half of the question that has
  ground truth, and says nothing about the other half.
- **30 seconds is one horizon.** Nothing here establishes that the same
  predictability holds at 1 second or at 5 minutes.
- **The gate is one way to use the model,** chosen because it is simple and its
  effect is measurable. Sizing the quote by predicted fill probability, or
  choosing between offsets, would be other ways and are not tested.
- **Calibration is fitted on one day.** It transfers imperfectly, as section 1
  shows. Ranking transfers well; absolute probabilities less so.
- **The snapshot grid still bounds everything.** The 1 second markout is really
  the first snapshot at or after one second, which can be up to five seconds
  away, exactly as in 001.
- **This is still a backtest.** Nothing here places an order anywhere.

---

## Addendum, 2026-09-16: a hazard model instead of one classifier

Everything above answers one question: does the queue in front of an order trade
away **within 30 seconds**. That horizon was chosen and then everything was fitted
to it, which throws away most of what a market maker actually wants. The useful
question is not whether the queue clears by some arbitrary horizon but roughly
*when*, so two levels can be compared.

So: the same features, the same FTRL implementation, refitted as a discrete time
hazard. Six buckets with edges at 5, 15, 30, 60, 150 and 300 seconds, one
logistic per bucket trained on the samples that survived to it, and the survival
curve multiplied out:

    S(k) = prod_{j <= k} (1 - hazard_j(x))      P(clears by edge k) = 1 - S(k)

The baseline hazard is fully flexible because every bucket gets its own weights.
31.6% of the fit set is censored, meaning it had not cleared by 300 seconds, and
censored samples are at risk in every bucket, which is what censoring means.

    ./build/mltrain --survival --train=results/queue_train.bin \
                    --test=results/queue_test.bin

| horizon | actual | predicted | log loss | base rate log loss | AUC |
|---|---|---|---|---|---|
| 5 s | 0.0354 | 0.0495 | **0.12886** | 0.15304 | **0.8163** |
| 15 s | 0.1226 | 0.1667 | **0.33379** | 0.37206 | **0.7447** |
| 30 s | 0.2457 | 0.3399 | **0.54220** | 0.55755 | 0.6678 |
| 60 s | 0.3891 | 0.5141 | 0.68330 | 0.66837 | 0.5985 |
| 150 s | 0.5652 | 0.6829 | 0.70381 | 0.68462 | 0.5819 |
| 300 s | 0.6751 | 0.7684 | 0.64672 | 0.63054 | 0.5683 |

Three things, two of which are not flattering.

**It is a large win at short horizons.** AUC 0.8163 at five seconds, against the
0.7001 the single classifier managed at thirty. Whether a queue clears in the
next few seconds is far more predictable than whether it clears eventually, which
is the same shape experiment 003 found in wallet toxicity: the information is
short lived. For a maker deciding whether to join a level right now, the five
second number is the one that matters, and it is the one the original design was
not asking for.

**It loses to the specialist at the horizon the specialist was trained on.** 0.6678
against 0.7001 at thirty seconds. A model fitted for one horizon beats a general
one there, which is unsurprising and worth stating rather than hiding behind the
five second row.

**Past sixty seconds it is worse than useless.** AUC falls to 0.57 and the log
loss is *above* the base rate's at 60, 150 and 300 seconds, meaning a constant
would have scored better. Part of that is calibration: the Platt scaling is
fitted on the validation day, whose base rate is higher than the test period's,
so every horizon over-predicts, and over-predicting 0.77 against an actual 0.68
is expensive in log loss even when the ranking is sound. But the ranking is not
sound either at those horizons, and AUC does not care about calibration. The
honest reading is that queue clearing beyond a minute is not predictable from
this feature set, and the hazard model makes that visible where a single 30
second classifier hid it inside one averaged number.

**What this changes.** The gate in the simulator still uses the 30 second
classifier, because that is what was measured end to end in section 2 above and
swapping it would invalidate those numbers. The hazard model is the better
instrument for the decision a maker actually faces, and wiring it in, with the
five second horizon rather than the thirty, is the obvious next step and would
need its own holdout run to claim anything.
