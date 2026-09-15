# The matching engine, and how it is checked

Back to the [README](../README.md).

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

| resting orders | p50 | p90 | p99 | p99.9 | mean | throughput | through the ring |
|---|---|---|---|---|---|---|---|
| 2,000 | 48 ns | 84 ns | 189 ns | 458 ns | 63 ns | 17.01 M msg/s | 15.88 M msg/s |
| 20,000 | 81 ns | 145 ns | 383 ns | 976 ns | 106 ns | 9.85 M msg/s | 8.79 M msg/s |
| 60,000 | 131 ns | 251 ns | 506 ns | 7,836 ns | 167 ns | 8.00 M msg/s | 6.83 M msg/s |
| 200,000 | 176 ns | 401 ns | 685 ns | 13,528 ns | 251 ns | 5.06 M msg/s | 5.12 M msg/s |

### Huge pages, which is where most of the large book cost was

At 200,000 resting orders the level table, the order pool and the id map come to
about 70 MB. That is 17,000 pages of 4 KB against a data TLB with roughly a
thousand entries, so a cancel pays a page walk on top of its cache miss and the
benchmark measures the page tables as much as it measures the book.

Same binary, same flow, one flag, 30 seconds each:

| | p50 | mean | p99.9 | throughput |
|---|---|---|---|---|
| 4 KB pages | 203 ns | 271 ns | 15,929 ns | 3.85 M msg/s |
| 2 MB pages | 128 ns | 183 ns | 11,238 ns | 6.24 M msg/s |
| | **-37%** | **-32%** | **-29%** | **+62%** |

`src/util/hugevec.hpp` maps the three arrays itself and asks for huge pages with
`madvise`, rather than relying on a machine wide setting a reader may not be able
to change. The first version of it rounded the mapping's *length* to a huge page
and bought 14% where flipping the system setting bought 46%, which is what sent
me looking: a huge page has to be backed at a huge page *boundary*, so a
correctly sized mapping starting at an odd 4 KB offset still gets small pages.
Over allocating by one page, aligning the start up and handing the slack back
fixed it. `AnonHugePages` in `/proc/meminfo` confirms the mapping is really
backed that way rather than only asking politely.

Run to run spread on this VM is real: the same 200,000 configuration produced
5.06 and 6.24 M msg/s in two 30 second runs half an hour apart. Treat the medians
as solid and the throughput figures as plus or minus about 20%.

The unloaded queue handoff does not depend on the book and was measured once per
row: p50 between 130 and 238 ns, p99 between 180 and 750 ns. The high pair is the
first, cold run; take it as roughly 135 ns with the cold outlier reported rather
than dropped.

Per operation, at the two ends of that range:

| book | operation | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|---|
| 2,000 | add that rests | 50 ns | 83 ns | 195 ns | 454 ns |
| 2,000 | add that trades | 49 ns | 128 ns | 305 ns | 621 ns |
| 2,000 | cancel | 45 ns | 74 ns | 184 ns | 426 ns |
| 2,000 | modify | 44 ns | 128 ns | 215 ns | 479 ns |
| 200,000 | add that rests | 159 ns | 275 ns | 580 ns | 11,521 ns |
| 200,000 | add that trades | 156 ns | 419 ns | 1,161 ns | 14,171 ns |
| 200,000 | cancel | 261 ns | 389 ns | 674 ns | 15,599 ns |
| 200,000 | modify | 244 ns | 470 ns | 781 ns | 15,806 ns |

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
mean column to within the serialisation overhead (59 against 63 ns, 102 against
106, 125 against 167, 198 against 251), which is the cross-check that the timed
numbers are not measuring the timer.

The pipeline column runs the feed on one pinned core and the matcher on another
with bulk push and pop through the lock free ring. The handoff column is a
different experiment: the producer sends only when the ring is empty, so the
reading is the handoff itself rather than the queue wait. A saturated ring makes
the transit time a function of queue depth and says nothing about the handoff,
which is why the two are not the same measurement.

A command on the wire between the feed thread and the matcher is 40 bytes: a
timestamp, two order references, a quantity, a price and three enums, with no
padding wasted. The second reference exists for one message, ITCH Order Replace,
which names both the reference it retires and the one that takes its place.

**Caveats on these numbers.** This is a shared four vCPU cloud VM with a Haswell
class host, a measured TSC of 2.4000 GHz, and no huge pages. CPU steal over the
run was 156 ticks, about 0.2%. The medians and the throughput
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
