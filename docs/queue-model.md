# The queue model

Back to the [README](../README.md). The full account, including what was
scored against the test set and when, is in
[experiments/002_queue_model](../experiments/002_queue_model).

## The queue model

`src/ml/`, `tools/mlgen.cpp`, `tools/mltrain.cpp`. Full account in
[experiments/002_queue_model](../experiments/002_queue_model).

The fill model in the latency experiment contains one parameter that is an
assumption rather than a measurement: what share of cancellations happen ahead of
the agent in the queue. There is no ground truth for that. But there is ground
truth for the half of the same question that only involves trading, and that half
is the one that produces fills:

> An order joins a price level at time t with quantity q resting in front of it.
> Within the next 30 seconds, does enough volume trade at that price to work
> through q?

That is read straight off the tape, with no simulation and no fill model in
between. 828,828 training samples over the calibration days, 1,035,984 over the
holdout.

**Everything is written from scratch.** AVX2 kernels with a scalar reference
beside each one, FTRL-Proximal logistic regression, and a 64 to 32 to 1 MLP whose
forward and backward passes are hand derived and checked against central
differences. No library is involved at any point.

| model | log loss | Brier | AUC |
|---|---|---|---|
| the constant the fill model assumes | 0.55763 | 0.18535 | 0.5000 |
| one feature | 0.53203 | 0.17555 | 0.6754 |
| **logistic, 64 features** | **0.52132** | **0.17074** | **0.7001** |
| mlp, 64-32-1 | 0.52761 | 0.17147 | 0.6584 |

Three things there are worth stating rather than leaving to be noticed. Most of
the signal is one ratio: a logistic on `log1p(q / consuming volume over the last
30 s)` alone gets 0.6754, and the other 63 features are worth 0.0247 of AUC
between them. Per day AUC on the holdout runs 0.671 to 0.737 with no bad day,
across days whose base rate moves by a factor of 2.3. And **the neural network
loses**, on every metric, at every width and learning rate searched; the bucket
indicators in the feature expansion already give the linear model the one bend
the problem needs.

### Used for something

The agent asks the model, before joining a level, whether the quantity already
resting there is likely to trade away, and stands aside when the answer is below
a threshold chosen on the calibration days. Paired by day over the holdout:

| | ungated | gated | difference | 95% CI |
|---|---|---|---|---|
| swept share of fills | 0.4397 | 0.2481 | **-0.1916** | **[-0.3530, -0.1072]** |
| markout 1 s, bps | -0.4208 | -0.2353 | **+0.1854** | **[+0.0726, +0.4356]** |
| markout 5 s, bps | -0.4937 | -0.3148 | **+0.1789** | **[+0.0502, +0.4467]** |
| markout 30 s, bps | -0.6507 | -0.5732 | +0.0775 | [-0.1079, +0.4180] |
| net edge, bps | -2.6412 | -2.4583 | +0.1829 | [-0.1590, +0.5768] |
| fills | 1,451 | 1,701 | | |

It takes more fills, not fewer, and the ones it takes are less often the
adversely selected kind. Net edge is still negative, because a 1.5 bps maker fee
against a 0.265 bps half spread is not a queue selection problem.

The latency conclusion survives the model: the gated agent's headline slope is
+0.000693 bps/ms, 95% CI [-0.001182, +0.002322], still crossing zero, and still
bit identical at 0.1, 1, 10 and 33 ms.

### Kernels

```
./build/bench --kernels --core=2
```

| kernel | scalar | vector | speedup |
|---|---|---|---|
| dot, 64 | 50.08 ns | 6.42 ns | 7.80x |
| axpy, 64 | 5.69 ns | 5.54 ns | 1.03x |
| gemv, 32 by 64 | 1534.40 ns | 176.13 ns | 8.71x |
| Adam step, 2048 weights | 7883.66 ns | 2547.04 ns | 3.10x |

`axpy` gets nothing from vectorisation and that is correct: two reads and a write
for two flops per element is bound by memory bandwidth, not arithmetic. It is in
the table because a kernel table where everything is faster is a table whose
entries were chosen. Blocking the matrix vector product four rows at a time is
worth 1.24x on top of what vectorising the inner product gave, and the benchmark
reports the unblocked vector version too so the two are not conflated.

**The first version of this table reported the dot product at 103 GFLOP/s.** This
core's AVX2 ceiling is 76.8. Both input vectors were loop invariant, so the
compiler hoisted the call out of the timing loop; accumulating into a checksum
stops a call being deleted but does nothing to stop it being hoisted. The
benchmark now puts an optimisation barrier on the inputs and the result inside
the timed lambda, and prints the machine's peak under the table so the next
impossible number is obvious.
