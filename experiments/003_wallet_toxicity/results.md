# 003 results: wallet toxicity persists, and it is worth 1.4 basis points

Status: **both pre-registered hypotheses hold, by a wider margin than predicted.**

I wrote in the pre-registration that I expected H1 to survive weakly and H2 to be
marginal. Both cleared zero comfortably, and the effect survived every attempt I
made to explain it away as something other than information. Recording the wrong
prediction next to the right answer is the point of writing it down first.

Every number is from `./build/wallets` and `scripts/wallet_analysis.py`.

---

## 1. The headline

Walk forward, 14 day trailing window, 20 evaluation days from 2026-08-25 to
2026-09-13, aggressor side, 30 second markout. Wallets need 200 prints in the
trailing window and 20 on the day. Median 68 wallets scored per day.

| | | |
|---|---|---|
| **H1, persistence** mean within-day Spearman | **+0.3905** | 95% CI [+0.3333, +0.4461] |
| **H2, separation** next-day markout gap, toxic quartile minus benign | **+1.4024 bps** | 95% CI [+0.8900, +2.0254] |

Broken out:

| flow | next-day aggressor markout, 30 s |
|---|---|
| wallets in the top quartile of trailing toxicity | **+0.9602 bps** [+0.5997, +1.2900] |
| all eligible flow | +0.2446 bps |
| wallets in the bottom quartile | **−0.4423 bps** [−0.9792, +0.0082] |

Positive markout means the aggressor was right and whoever was resting got picked
off. So a maker filled by top-quartile flow is about 0.96 bps underwater after 30
seconds, and a maker filled by bottom-quartile flow is about 0.44 bps ahead.

**1.4 basis points is a large number in this repository.** The half spread a
maker at the touch is competing for is 0.265 bps. The entire queue position
penalty measured in 002 was 0.57 bps. The latency effect 001 went looking for was
0.001 bps per millisecond. This is bigger than all of them, and it comes from a
field the exchange puts in every print for free.

## 2. Trying to break it

A surprising positive result deserves more scrutiny than a null one, so here is
what it survived.

**It is not trade size.** Big trades move the mid mechanically, so if the toxic
wallets were simply the large ones this would be price impact wearing a costume.
They are not: the toxic quartile's mean trade size is **1.837 ETH against the
benign quartile's 1.959**, slightly smaller. And the gap holds inside every
quartile of trade size:

| trailing mean trade size | n | toxic | benign | gap |
|---|---|---|---|---|
| 0.00 to 0.08 ETH | 172 | +1.010 | −0.071 | **+1.081** |
| 0.08 to 0.60 ETH | 171 | +0.674 | −0.270 | **+0.944** |
| 0.60 to 2.95 ETH | 171 | +1.399 | −0.607 | **+2.006** |
| 2.95 to 16.22 ETH | 172 | +0.808 | −0.690 | **+1.499** |

Weighting every wallet equally instead of by size, so that size cannot enter at
all, gives **+1.4692 bps [+1.2906, +1.6553]**, a tighter interval than the
headline.

**It is not direction.** ETH ran from about 1,916 to about 2,517 over this
dataset. In a market that trends that hard, a wallet that happened to be long
would look informed. Splitting the gap by which way the aggressor traded:

| aggressor direction | toxic | benign | gap | 95% CI |
|---|---|---|---|---|
| bought | +0.869 | −0.467 | **+1.335** | [+0.733, +1.997] |
| sold | +1.229 | −1.273 | **+2.502** | [+0.990, +4.516] |

Both exclude zero, and the **sell** side is the larger of the two, which is the
opposite of what a momentum artifact in a rising market would produce.

**It is not one good day.** The gap is positive on **18 of 20** evaluation days,
mean +1.6898, 95% CI [+1.1444, +2.3264].

## 3. Secondary results

Pre-registered as secondary and labelled as such.

| variation | Spearman | gap, bps | |
|---|---|---|---|
| 5 s markout | +0.5946 [+0.5560, +0.6324] | +1.3819 [+1.1921, +1.5725] | excludes zero |
| **30 s markout, the primary** | +0.3905 [+0.3333, +0.4461] | +1.4024 [+0.8900, +2.0254] | excludes zero |
| 300 s markout | +0.0355 [−0.0233, +0.0943] | +0.1605 [−1.4052, +1.8374] | **crosses zero** |
| 100 trailing prints instead of 200 | +0.3402 [+0.2933, +0.3826] | +1.2986 [+0.9862, +1.7259] | excludes zero |
| 500 trailing prints instead of 200 | +0.4253 [+0.3546, +0.4934] | +1.5907 [+1.0773, +2.1272] | excludes zero |
| maker side rather than aggressor | +0.4017 [+0.3562, +0.4484] | +1.0995 [+0.8132, +1.4246] | excludes zero |

Two of those are worth reading twice.

**The information decays, and that is reassuring rather than disappointing.** The
effect is strongest at 5 seconds, still clear at 30, and **gone at 300** with an
interval that crosses zero. Persistent skill at picking off makers on a thirty
second horizon that has vanished by five minutes is what short-horizon informed
flow looks like. A wallet that was simply positioned for a move would show the
opposite shape.

**Makers separate too.** Scoring the resting side the same way gives +1.0995 bps
between the best and worst quartile of makers. Some participants here are
persistently better at not being run over, which is the same phenomenon from the
other side of the trade and a hint that the queue position work in 002 is
measuring something real.

## 4. What this does and does not mean

**What it means.** On a venue that names both counterparties on every print,
adverse selection is not an anonymous statistical fact, it is a property of
identifiable accounts, and it persists well enough to act on two weeks later. On
a centralised exchange this question cannot be asked at all: you never learn who
hit you.

**What it does not mean.** This is not yet an edge, for three reasons that matter.

1. **Recognition speed.** Acting on it means knowing who traded and pulling
   quotes before they come back. Experiment 001 established that this venue gives
   no reaction advantage below its block interval, so how much of 1.4 bps is
   actually capturable is an open question and is the obvious next experiment.
2. **Wallets are not people.** One trader may use many addresses and one address
   may be a venue-side account netting many traders. A persistent score may be
   measuring an account's *function*, a liquidation engine or a bridge, rather
   than anyone's skill. Nothing in this data can see through that.
3. **Survivorship.** A wallet needs prints in both windows to be scored, which
   selects for wallets that keep trading.

## 5. Limitations

- One instrument, one venue, 39 days, 20 of them evaluated. The market trended
  strongly throughout.
- The 5 second snapshot grid bounds every markout, exactly as in 001. The 5
  second number is really "the first snapshot at or after 5 seconds".
- The trailing window, the thresholds and the horizon were fixed in the
  pre-registration and not searched. The sensitivities in section 3 are reported
  because they were promised, not because anything was chosen from them.
- Nine days, 2026-08-09 to 08-18, were spent on the exploration that motivated
  this and are used only as scoring history, never as evaluation.
- A persistent score is not a tradable edge, and nothing here places an order.

## 6. Reproducing

    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
    ./build/wallets --out=results/wallets.csv
    python3 scripts/wallet_analysis.py
