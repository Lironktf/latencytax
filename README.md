# latencytax

A limit order book and matching engine in C++20, a replay that reconstructs the
Hyperliquid ETH-perp book from raw snapshots and checks the engine against the
exchange's own data, and a market making simulation inside that replay that
measures what reaction latency is worth.

Three things came out of it.

**The engine is correct against 25.6 million level comparisons.** Replaying 39
days of ETH-perp through it, the reconstructed top-20 book matches the exchange's
next published snapshot on every one of 25,570,480 compared level positions, over
639,262 five-second windows and 24,183,264 engine commands. Zero mismatches, zero
unexpected trades. The reconstruction that drives the engine is computed from the
raw files alone and never reads engine state, so this is a check on the engine
rather than a tautology.

**The engine's speed depends on the size of the book, and the numbers say by how
much.** With 2,000 resting orders it runs at 17.13 million messages per second
with a median operation of 46 ns and a p99.9 of 495 ns. With 200,000 resting
orders the same code runs at 4.05 million messages per second with a median of
205 ns and a p99.9 of 15.7 us. Nothing about the algorithm changed between those
two rows. The working set went from fitting in cache to not fitting, and a cancel
went from costing the same as an add to costing 1.6 times as much, because a
cancel begins with a hash lookup that has become a guaranteed miss.

**Reaction latency below about 33 milliseconds is worth exactly nothing on this
venue, and that is measurable rather than a manner of speaking.** The latency tax
over the pre-registered range of 0.1 to 100 ms is -0.001495 basis points of
notional per millisecond, 95% CI [-0.004663, +0.000695], which crosses zero. An
agent at 0.1 ms and an agent at 33 ms produce identical fills and identical PnL
to the cent, in every configuration tested, because the trade tape has no events
at all between 1 ms and 30 ms: Hyperliquid batches into blocks and there is
nothing inside one to react to. A cost does appear at second scale, +0.000085 bps
per millisecond between 1 s and 5 s, 95% CI [+0.000048, +0.000133].

That last result is a negative one for the question as posed, and it was
pre-registered as the likely outcome before the holdout was opened. The
pre-registration, its two amendments and the results are in
[experiments/001_latency_tax](experiments/001_latency_tax).

---

## Build and run

Needs a C++20 compiler, CMake 3.16 or newer, and zlib. Nothing else.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure
```

Three binaries, each with `--help`:

```
./build/bench   --seconds=30 --core=2 --feed-core=3 --max-live=2000
./build/replay  --data=data/raw
./build/sim     --data=data/raw --days=2026-08-13 --latency=0.1,1,10,100
```

Two scripts reproduce everything quoted here:

```
./scripts/run_bench.sh        # the engine tables, about 7 minutes
./scripts/run_experiment.sh   # fidelity, calibration, holdout, extension, about two minutes
```

A debug build turns on the address and undefined behaviour sanitizers. The lock
free queue has its own build type for the thread sanitizer, since ASan and TSan
cannot be linked together:

```
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug && cmake --build build-debug -j
ctest --test-dir build-debug --output-on-failure

cmake -B build-tsan -DCMAKE_BUILD_TYPE=TSan && cmake --build build-tsan -j
./build-tsan/test_spsc
```

### Getting the data

The two raw datasets live in a private Backblaze B2 bucket behind the
`alphadata` CLI:

```
alphadata get 00_raw/live/hyperliquid/l2book/ETH
alphadata get 00_raw/live/hyperliquid/trades/ETH
```

Each prints a local path. Copy those two directories to `data/raw/l2book_ETH`
and `data/raw/trades_ETH`, unchanged. Nothing in this repository writes to them.

---

## The data, and what it cannot tell you

Hyperliquid ETH-perp, collected live, 2026-08-07 to 2026-09-14.

| | |
|---|---|
| book | top 20 levels of each side, one snapshot every 5 seconds, JSONL.gz |
| trades | every print, deduplicated on trade id, venue millisecond timestamps |
| snapshots present | 639,307 of 673,920 expected, 94.86% |
| tick size | 0.1 USD; the price ran 1853.1 to 2663.0 over the period, median mid 2443.4 |
| spread | one tick in 98.39% of snapshots over the whole period, 99.46% over the holdout |
| daily volume | about 37,000 ETH on the calibration days |
| prints | 1,009,459 |

Both records carry two timestamps: `time`, the venue's own clock, and `rx`, when
the collector received the message. Everything downstream orders on `time`. On
2026-08-09 the median difference between them was 366 ms and the 99th percentile
667 ms.

Days that are not complete, and how they are handled:

| day | coverage | treatment |
|---|---|---|
| 2026-08-07 | 9.59% | excluded everywhere |
| 2026-08-08 | 99.85% | before the calibration window, unused |
| 2026-08-15 | 99.85% | excluded from the holdout by the pre-registration, reported as a sensitivity |
| 2026-08-19 | 31.05% | excluded from the extension set |
| 2026-09-14 | 62.27% | no trade file at all, excluded from the extension set |

The thing that matters most about this data is that **the book is a snapshot
every five seconds, not an order by order feed.** Individual orders, their ids,
their arrival times and their queue positions are not observable. An order added
and cancelled inside one window never existed as far as this repository is
concerned. The only millisecond resolution data here is the trade tape. Every
place where that limit changes what a number means, it is said again.

---

## The matching engine

`src/engine/`, about 1,000 lines including comments. The whole repository is
about 5,000 lines of C++ with no dependency outside the standard library and
zlib.

Price levels live in one contiguous array indexed directly by tick. A level is 24
bytes: resting quantity, order count, and the head and tail of its FIFO queue.
Reaching a level is an array index, not a tree walk and not a hash.

Orders live in a flat pool with a free list, and the queue links are 32 bit slot
indices rather than pointers, so an order is 32 bytes and two of them share a
cache line. Order ids map to pool slots through an open addressing table with
linear probing and Knuth's backward shift deletion, so a long run of cancels does
not leave the table full of tombstones. Ids from an exchange are often
sequential, which is the worst case for identity hashing under linear probing, so
the table uses Fibonacci hashing.

Finding the next price when the touch empties uses a bitmap with one bit per
tick, scanned with count-trailing-zeros. Bids always sit at or below the best bid
and asks at or above the best ask and the two never overlap, so one bitmap serves
both sides: scanning down from the best bid can only find bids.

Nothing on the hot path allocates once the pool and the table are sized. Nothing
anywhere touches a float. Prices are integer ticks and quantities are integers in
units of 1e-8, because 0.1 is not representable in binary floating point and two
orders at the same price have to compare equal.

Limit, market, cancel and modify, with price-time priority, and immediate-or-cancel
and fill-or-kill alongside good-till-cancel. Modify keeps queue position only
when the price is unchanged and the size goes down, which is the only case where
a venue can honestly leave an order where it is; anything else is a cancel and a
new order at the back of the queue. Every reject is decided before anything is
removed, so a rejected message leaves the book exactly as it was and a modify
never degrades into a silent cancel. That includes the awkward case of a full
order pool, which is checked before matching starts rather than after.

Events go out through an `EventSink` interface: one virtual call per event, not
per operation. The benchmark numbers below include that cost, because a real
engine has to get its events onto a wire too.

### Correctness

```
ctest --test-dir build --output-on-failure
```

Four test binaries, no framework.

`test_order_book` has the hand written cases you would expect, and then a
differential fuzz: 320,000 random commands, eight seeds, fed to both the fast
book and a deliberately naive reference built from `std::map` and `std::list`.
Every trade has to match event for event, including maker id, taker id, price,
quantity and the maker's remaining size, and the final book has to match level
for level across the whole price range. There is also a full invariant walk that
checks every level against its queue, every queue link against its neighbour,
every order against the id map, and the occupancy bitmap against the level
contents.

`test_id_map` fuzzes 300,000 operations against `std::unordered_map`, including
the case of sequential ids landing in one probe run, and checks that ids never
inserted always miss.

`test_spsc` pushes 4,000,000 messages across two threads and requires every one
to arrive exactly once, in order, with an intact payload, plus a bulk variant
checked on a checksum. Run it under TSan with the build type above.

`test_reconstruct` covers the fixed point parsing, the two JSON schemas, which
side of the book a print consumes, the mapping from level changes to add, cancel
and modify, feed gap handling, and 2,000 random windows that each have to
reconstruct exactly.

### Speed

```
./scripts/run_bench.sh
```

Four steady state book sizes, identical code and identical synthetic flow, 30
seconds of timed work per phase, matcher pinned to one core and the feed thread
to another. The "all operations" row pools adds, cancels and modifies.

| resting orders | p50 | p90 | p99 | p99.9 | mean | throughput | through the ring | queue handoff p50 |
|---|---|---|---|---|---|---|---|---|
| 2,000 | 46 ns | 85 ns | 204 ns | 495 ns | 65 ns | 17.13 M msg/s | 16.38 M msg/s | 154 ns |
| 20,000 | 104 ns | 199 ns | 446 ns | 2,319 ns | 136 ns | 8.75 M msg/s | 9.13 M msg/s | 136 ns |
| 60,000 | 161 ns | 311 ns | 580 ns | 12,636 ns | 207 ns | 6.32 M msg/s | 5.69 M msg/s | 135 ns |
| 200,000 | 205 ns | 393 ns | 696 ns | 15,740 ns | 270 ns | 4.05 M msg/s | 3.43 M msg/s | 314 ns |

Per operation, at the two ends of that range:

| book | operation | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|---|
| 2,000 | add that rests | 46 ns | 81 ns | 191 ns | 480 ns |
| 2,000 | add that trades | 46 ns | 130 ns | 315 ns | 673 ns |
| 2,000 | cancel | 48 ns | 76 ns | 184 ns | 466 ns |
| 2,000 | modify | 48 ns | 120 ns | 231 ns | 528 ns |
| 200,000 | add that rests | 165 ns | 279 ns | 581 ns | 13,038 ns |
| 200,000 | add that trades | 170 ns | 425 ns | 1,178 ns | 15,733 ns |
| 200,000 | cancel | 263 ns | 415 ns | 690 ns | 15,990 ns |
| 200,000 | modify | 256 ns | 484 ns | 784 ns | 16,138 ns |

The interesting part of that table is not the top row, it is the shape of the
change. In the small book all four operations cost the same, because everything
the engine touches is already in cache. In the large book cancel and modify cost
about 1.6 times an add, and the reason is specific: a cancel starts with an
order-id lookup into a 32 MB hash table, which is a guaranteed cache miss and
usually a TLB miss as well, whereas an add writes into a pool slot the free list
has just handed back. Nothing about the algorithm changed between those rows.

**How this was measured.** Each operation is bracketed by `rdtscp` with `lfence`
on both sides. The fences stop the processor moving work across the measurement,
which is what makes a per-call number mean anything, and they also make the
number pessimistic, because the serialisation they force cannot be subtracted.
The cost of an empty timer pair is measured during warm up, came out at 33 to 36
cycles, and is subtracted from every figure above. Samples go into a histogram
with one bin per cycle up to 65,536 cycles, so the percentiles are exact to a
cycle rather than interpolated from buckets, in half a megabyte that does not
grow with the sample count.

The throughput column is a separate phase with no timers in the loop at all:
commands are generated into a block first, then the block is applied under one
wall clock reading. It is the honest cost per message, and it agrees with the
mean column to within the serialisation overhead (58 against 65 ns, 114 against
136, 158 against 207, 247 against 270), which is the cross-check that the timed
numbers are not measuring the timer.

The pipeline column runs the feed on one pinned core and the matcher on another
with bulk push and pop through the lock free ring. The handoff column is a
different experiment: the producer sends only when the ring is empty, so the
reading is the handoff itself rather than the queue wait. A saturated ring makes
the transit time a function of queue depth and says nothing about the handoff,
which is why the two are not the same measurement.

**Caveats on these numbers.** This is a shared four vCPU cloud VM with a Haswell
class host, a measured TSC of 2.4000 GHz, and no huge pages. CPU steal over the
seven minute run was 128 ticks, about 0.3%. The medians and the throughput
figures reproduce across runs; the maxima, which run to milliseconds, are
scheduler noise and are reported rather than trimmed. Running anything else on
the machine at the same time changes these numbers by a factor of several, which
is why the script asks for an idle box. Real numbers on real hardware with
isolated cores and huge pages would be better than these; these are the ones this
machine produced.

The synthetic flow is 45% adds, 40% cancels, 10% modifies and 5% marketable
orders, with most orders arriving within a few ticks of the touch and the
marketable order size distribution shaped to the measured Hyperliquid tape, which
has a median print of 0.065 ETH and a mean of 1.34. A flat size distribution
there would have every marketable order sweep several levels and turn the
benchmark into a fill test. The mid walks slowly inside a band, because a mid
that drifts further than the quoted depth over the lifetime of a resting order
leaves most of the book stale and crossable and measures something else again.

---

## Replay and fidelity

`src/replay/`.

For each window between consecutive snapshots:

1. Every print in the window is replayed as a marketable immediate-or-cancel
   order at the printed price, in time order.
2. The result is diffed against the next snapshot, and the adds, cancels and
   modifies that close the gap are emitted.

Both steps work on a shadow model built only from the raw files. The shadow model
never reads engine state. The engine is then required to agree with the next
snapshot exactly.

That is what makes this a test rather than a tautology, and the mechanism is
worth spelling out, because the obvious objection is that a diff computed from
snapshot n+1 will of course land on snapshot n+1. It will not. The diff is
computed as the target snapshot minus the *shadow* state, and applied to the
*engine* state. Those two are only the same thing if the engine is right. If the
engine had dropped a fill, mislinked a queue, left a stale occupancy bit or
returned the wrong best price, the diff would be applied to a book it does not
describe and the engine would land somewhere other than the published snapshot.

The book is seeded from a snapshot once per day and after a feed gap, and never
reseeded otherwise, so an error in the first window of a day stays wrong for the
remaining 17,000 windows of that day instead of being washed out.

Since the feed is L2 and individual orders are not observable, each price level
is represented by one synthetic order holding the whole level quantity. Level
changes then map onto the four order types naturally: a level appearing is an
add, a level disappearing is a cancel, a level shrinking is a modify that keeps
queue position, and a level growing is a modify that loses it. Where queue
position inside a level actually matters, which is the market making simulation,
it is modelled explicitly from level quantity rather than taken from the engine.

Reconciliation runs removals before additions, so a book that has repriced never
has a stale level sitting on the wrong side of the new touch when the adds go in.
The engine counts any trade produced during reconciliation and the run fails if
there is one.

```
./build/replay --data=data/raw
```

Over all 39 days:

| | |
|---|---|
| windows scored | 639,262 (6 skipped as feed gaps longer than 30 s) |
| engine commands | 24,183,264: 2,349,329 adds, 2,316,358 cancels, 18,506,355 modifies, 1,009,422 tape prints |
| **reconstructed book vs the exchange snapshot** | **0 mismatches of 25,570,480 level positions** |
| **unexpected trades during reconciliation** | **0** |
| tape alone vs the exchange snapshot | 82.93% of level positions wrong, 87.09% size error in the top 5 |
| tape volume that found resting size at or better than its price | 85.82% |
| of that, filled at a better price than printed | 38.88% |
| prints that found nothing resting | 125,831 of 1,009,422 |

The two fidelity rows answer different questions and both are worth having. The
tape-alone row says that trades explain very little of what a book does in five
seconds: most of the change is quotes being pulled and replaced, not volume. It
is a measurement of the data's resolution, not a defect. The reconstructed row is
the engine check and it has to be zero.

The last three rows measure how stale a five second old book is. 14.2% of traded
volume had no resting size to match against at or better than its printed price,
and 12.5% of prints found nothing resting at all. That is a direct reading of how
far the book moves between snapshots, and it is the reason the market making
simulation models queue position explicitly instead of reading it off the book.

Whole run: 14 seconds for 39 days on one core.

---

## The experiment

Pre-registered before the simulator was written:
[hypothesis.md](experiments/001_latency_tax/hypothesis.md), including two dated
amendments, both made before any holdout day was loaded. Results:
[results.md](experiments/001_latency_tax/results.md).

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

---

## Limitations

- **The book is a 5 second snapshot.** Adverse selection inside the first five
  seconds after a fill is not measurable here. That is exactly where a
  millisecond-scale quote-side effect would live if one existed. The tape rules
  out a trade-side effect below 33 ms; it cannot rule out a quote-side one. A
  100 ms depth diff feed would settle it, and none exists in the archive for this
  period.
- **The 1 second markout is bounded below by the snapshot grid.** It is reported
  because it was asked for, and it should be read as "the first snapshot at or
  after one second", which is up to five seconds away.
- **Queue position inside a level is not observable in an L2 feed.** The fill
  model is a model. Its two least defensible choices, the cancellation
  attribution and what a print through the quote price does, are both reported
  as sensitivities rather than buried.
- **Five holdout days is a small sample.** The bootstrap reflects that and cannot
  fix it. The 25 day extension set exists for that reason and agrees.
- **The agent is assumed not to move the market.** At 0.5 ETH against a touch of
  a few hundred that is reasonable and still unverifiable.
- **The engine benchmark runs on a shared four vCPU cloud VM** with a Haswell
  class host and no huge pages. The medians are stable across runs; the maxima
  are scheduler noise and are reported rather than trimmed. Concurrent work on
  the same machine changes these numbers by a factor of several, which is why the
  script asks for an idle box.
- **The engine is single instrument and single threaded by design.** There is no
  cross-book risk, no self-trade prevention, no order lifecycle beyond what is
  listed, and no persistence.
- **This is a backtest.** It is not evidence of live profitability and nothing
  here places an order anywhere.

---

## Layout

```
src/engine/     types, events, order book, id map, spsc ring, command dispatch
src/util/       gzip line reader, cpu pinning, cycle timing and histograms
src/replay/     raw file parsing, reconstruction, fidelity binary
src/sim/        fill model, agent, simulation binary
bench/          engine benchmark
tests/          four test binaries, run by ctest
scripts/        run_bench.sh, run_experiment.sh, analyse.py, skew_compare.py,
                tape_structure.py
experiments/    pre-registration, amendments, results
results/        everything the two scripts produce
docs/           a longer write up of the latency result
```
