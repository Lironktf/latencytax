# latencytax

A limit order book and matching engine in C++20; an ITCH 5.0 style binary feed
and a sharded feed handler that drive it; a replay that reconstructs the
Hyperliquid ETH-perp book from raw snapshots and checks the engine against the
exchange's own data; and a market making simulation inside that replay that
measures what reaction latency is worth and what a learned queue model is worth.

About 9,600 lines of C++20, of which 1,900 are tests, with no dependency outside
the standard library and zlib. Four things came out of it.

**The engine is correct against 25.6 million level comparisons, twice, by two
different paths.** Replaying 39 days of ETH-perp, the reconstructed top-20 book
matches the exchange's next published snapshot on every one of 25,570,480
compared level positions, over 639,262 five-second windows and 24,183,264 engine
commands. Then the same 39 days are encoded as 24,078,404 ITCH 5.0 messages in
MoldUDP64 packets, parsed back big endian out of the wire by a feed handler that
shares no code with the replay above the book itself, and scored again: 0 wrong
of 25,572,280. Zero mismatches and zero unexpected trades on both paths. The
reconstruction that drives the engine is computed from the raw files alone and
never reads engine state, so this is a check on the engine rather than a
tautology.

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

**Queue clearing is predictable, and predicting it cuts adverse selection.** The
fill model in the experiment above assumes a constant where a market maker wants
a per level opinion, and the constant has an AUC of 0.5 by construction. A
logistic regression over 64 features, trained with FTRL-Proximal on hand written
AVX2 kernels, reaches **AUC 0.7001** on 1,035,984 holdout samples for whether the
queue in front of an order trades away within 30 seconds. Wired into the agent as
a veto on joining a level, out of sample: the share of fills that come from the
tape running through the agent's price falls from 44.0% to 24.8%, 95% CI
[-0.353, -0.107], and the 5 second markout improves by 0.179 bps, 95% CI [+0.050,
+0.447]. The hand written neural network beside it **loses** to the linear model
and is reported as such.

The latency result is a negative one for the question as posed, and it was
pre-registered as the likely outcome before the holdout was opened. The
pre-registration, its two amendments and the results are in
[experiments/001_latency_tax](experiments/001_latency_tax); the queue model is in
[experiments/002_queue_model](experiments/002_queue_model), which was not
pre-registered and says so at the top.

---

## Build and run

Needs a C++20 compiler, CMake 3.16 or newer, and zlib. Nothing else.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure
```

Six binaries, each with `--help`:

```
./build/bench    --seconds=30 --core=2 --feed-core=3 --max-live=2000
./build/bench    --kernels --core=2
./build/replay   --data=data/raw
./build/sim      --data=data/raw --days=2026-08-13 --latency=0.1,1,10,100
./build/itchgen  --days=2026-08-13 --out=results/eth.itch
./build/itchfeed --check --data=data/raw --days=2026-08-13 results/eth.itch
./build/mlgen    --days=2026-08-09 --out=results/queue_train.bin
./build/mltrain  --train=results/queue_train.bin --test=results/queue_test.bin
```

Three scripts reproduce everything quoted here:

```
./scripts/run_bench.sh         # the engine tables, about 7 minutes
./scripts/run_experiment.sh    # fidelity, calibration, holdout, extension, about two minutes
./scripts/run_queue_model.sh   # the queue model and the gated agent, about three minutes
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

`src/engine/`, about 1,250 lines including comments.

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

## The binary feed

`src/wire/`, `tools/itchgen.cpp`, `tools/itchfeed.cpp`.

The replay hands the engine a C++ struct. A venue hands it bytes off a socket,
big endian, unaligned, framed, with a sequence number and a gap to notice if one
goes missing. This is that path, and the point of building it is that the same
39 days go down both and have to land on the same book.

**What is faithful.** The order messages are byte for byte ITCH 5.0: Add Order,
Add Order with MPID, Order Executed, Order Executed With Price, Order Cancel,
Order Delete, Order Replace and Trade, at 36, 40, 31, 36, 23, 19, 35 and 44
bytes, each behind the 11 byte header of type, stock locate, tracking number and
a 48 bit timestamp. Prices are four implied decimals in a `uint32`. Framing is
MoldUDP64: a 20 byte header of session, sequence number and message count, then
length prefixed message blocks.

**What is not, and why.** ITCH's Stock Directory is 39 bytes of equity specific
fields with no meaning for a perpetual future. Rather than reuse the `R` type
code with a different body, which is the kind of thing that bites a reader later,
there is a separate lowercase `z` Symbol Directory carrying the symbol and the
two scales. ITCH has no lowercase type codes, so there is no collision. The
session timestamp counts from the start of the session's first day rather than
from midnight, because this collector partitions files by receive time and the
first few records of a file can carry a venue timestamp from just before
midnight; counting from midnight would wrap the field mid session.

**Three engine operations exist for this path and not for matching.** A market
data feed reports what happened rather than asking for something: `Reduce` for
Order Cancel, `Execute` for Order Executed, `Replace` for Order Replace. All
three keep queue position, because none of them is a new order. The engine
therefore plays both roles, and that is what makes two independent checks
possible from one book.

```
./build/itchgen  --days=2026-08-13 --out=results/eth.itch
./build/itchfeed --check --data=data/raw --days=2026-08-13 results/eth.itch
```

One day is 601,407 messages in 13,634 packets, 18.1 MB, 31.5 bytes per message.
All 39 days is 24,078,404 messages and 718 MB.

| | |
|---|---|
| **ITCH path against the exchange snapshots, 39 days** | **0 wrong of 25,572,280 level positions** |
| decode alone | 53.5 M msg/s, 18.7 ns/msg, 1,604 MB/s |
| decode and apply, one thread | 6.92 M msg/s |
| sequence gaps, malformed packets | 0, 0 |

The count differs slightly from the struct path's 25,570,480 because this scores
every snapshot including the six that bound a feed gap, which the replay skips.

### What building it turned up

The first full run came back with 1,160 wrong level positions out of 25.5
million, 0.0045%, and a single day had been clean. The failures were on exactly
six days, with exactly 200 wrong on five of them, and those six days were exactly
the six in the dataset that contain a feed gap.

The cause: when the book is reseeded after a gap, the deletes that retire
everything the wire believes is resting were stamped with the last event *before*
the hole rather than with the snapshot that replaces the book. That put the
teardown and the rebuild on opposite sides of a snapshot boundary, so a reader
scoring itself against that snapshot saw a book that had already been emptied.
Giving `on_reseed` the timestamp it belongs to fixed it, and the remaining 38
days were unaffected either way.

### Multi symbol and sharding

Every ITCH message carries the symbol index in its header, including the ones
that otherwise name only an order reference. That is not decoration: it means a
router never has to look up which symbol an order belongs to, so routing is a
field read at a fixed offset and a modulo, and a symbol lives entirely inside one
shard for the whole session.

Because of that, the books cannot depend on how many shards there are, and the
tool checks it rather than asserting it. `itchgen --symbols=8` writes the same
day under eight symbol codes with disjoint order references, 4,811,267 messages
and 144 MB, and `itchfeed --digest` prints an order independent digest per
symbol:

| shards | threads | throughput | digests |
|---|---|---|---|
| 1 | 1 (decode and apply together) | 6.92 M msg/s | all eight identical |
| 2 | 3 (one feed, two shards) | 17.56 M msg/s | all eight identical |
| 3 | 4 (one feed, three shards) | 23.34 M msg/s | all eight identical |

All eight symbols produce `a9c76d5cf301191a` at every shard count, which is also
the digest of the single symbol run. The jump from one to two shards is more than
double because the single threaded row decodes and applies on one core while the
sharded rows add a dedicated feed thread; three shards saturates this four vCPU
box.

The eight symbol file is a load and isolation fixture, not more market data, and
no fidelity number is ever quoted on it.

---

## The queue model

`src/ml/`, `tools/mlgen.cpp`, `tools/mltrain.cpp`. Full account in
[experiments/002_queue_model](experiments/002_queue_model).

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
- **The engine has no risk layer.** No cross-book risk, no self-trade
  prevention, no position or credit limits, no order lifecycle beyond what is
  listed, and no persistence. A single book is single threaded; the feed handler
  shards across books, not within one.
- **The binary feed is generated from this data, not captured from a venue.**
  The message layouts follow ITCH 5.0 and the framing follows MoldUDP64, but the
  content is the Hyperliquid reconstruction re-encoded. It exercises a real
  parser against a real format; it is not a NASDAQ capture and is not presented
  as one. The eight symbol file is one day duplicated under eight codes, which
  tests isolation and throughput and nothing about market structure.
- **The queue model predicts trading, not cancelling.** It measures the half of
  queue dynamics that has ground truth in an L2 feed. The cancellation share the
  fill model assumes is still assumed, and experiment 002 does not fix that.
- **Experiment 002 was not pre-registered.** 001 was. 002 says so at the top of
  its design document and lists every time its test set was scored and why.
- **This is a backtest.** It is not evidence of live profitability and nothing
  here places an order anywhere.

---

## Layout

```
src/engine/     types, events, order book, id map, spsc ring, command dispatch,
                multi symbol books
src/wire/       big endian access, ITCH 5.0 layouts, MoldUDP64 framing, decoder,
                sharded sequencer
src/ml/         AVX2 kernels with scalar references, FTRL logistic, MLP,
                features, dataset format, model loader
src/util/       gzip line reader, cpu pinning, cycle timing and histograms
src/replay/     raw file parsing, reconstruction, fidelity binary
src/sim/        fill model, agent, simulation binary
bench/          engine and kernel benchmarks
tools/          itchgen, itchfeed, mlgen, mltrain
tests/          six test binaries, run by ctest
scripts/        run_bench.sh, run_experiment.sh, run_queue_model.sh, analyse.py,
                skew_compare.py, compare_runs.py, tape_structure.py
experiments/    pre-registration, amendments, results
results/        everything the three scripts produce
docs/           a longer write up of the latency result
```
