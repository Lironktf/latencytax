# Exchange connectivity: the feed in, the orders out

Back to the [README](../README.md).

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
./build/ticktotrade --all --seconds=20
./build/ouchgw   --orders=20000 --drop-at=5000 --burst=800
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

## Order entry

`src/wire/soup.hpp`, `src/wire/ouch.hpp`, `src/wire/session.hpp`,
`tools/ouchgw.cpp`.

The feed above is the market telling everyone what happened. Order entry is the
other half of exchange connectivity, and it has the harder job: a TCP connection
that can drop at any moment, and a client that has to be able to come back and
find out exactly what became of the orders it sent.

**SoupBinTCP** is the session layer. The framing is trivial, a two byte big
endian length and a type byte. The contract is not:

- everything the server sends as Sequenced Data is implicitly numbered, and the
  numbers are not on the wire, both sides count;
- on login the client names the sequence it wants to start from, and the server
  answers with the one it will actually start from and replays from there;
- a client that died after message 900 reconnects asking for 901 and gets 901
  onward, byte for byte, as though nothing had happened.

**OUCH** is what rides inside it. An order is named by a token the client chose,
not by an identifier the exchange handed back, which is exactly what lets a
client that lost its connection still know what to ask about. Enter, Cancel and
Replace go up; Accepted, Executed, Canceled, Replaced and Rejected come back, and
they come back on the sequenced stream so the answer survives a disconnection
even though the question does not.

The gateway binds all of that to the same matching engine: OUCH in becomes engine
calls, engine events become OUCH out, and every outbound message goes through the
store, so it is replayable by construction rather than by remembering to copy it
somewhere.

```
./build/ouchgw --orders=20000 --drop-at=5000 --burst=800
```

| | |
|---|---|
| orders sent | 20,800 |
| sequenced messages published | 20,800 |
| TCP connections used | 2 |
| published while the client was gone | 800 |
| replayed on reconnect | 800 |
| **missing or altered after recovery** | **0 of 20,800** |
| order entry round trip, p50 | 22,972 ns |

The recovery test is deliberately made to matter. A client that waits for each
acknowledgement before sending the next order is never behind, so killing its
connection proves nothing: the first version of this test replayed zero messages
and passed. So the client now fires a burst with nobody reading, waits for the
exchange to work through what is already in the socket buffer, and disappears
without reading a single reply. Those 800 acknowledgements are published to
somebody who is not there. On reconnect it asks for the next sequence it never
saw and gets all 800 back, and every one is compared by digest against what the
server stored.

### Tested without a socket in sight

The session state machine has no networking in it, which is the point. Two cases
in `tests/test_ouch.cpp` carry most of the weight:

- **Fragmentation.** TCP hands you arbitrary byte boundaries, and a parser that
  works only when a read contains whole packets works only in testing. The
  session is fed the same stream in chunks of 1, 2, 3, 7, 13 and 64 bytes, and
  every one has to produce byte identical output to a single large read.
- **Recovery fuzz.** 300 sessions, each published in random bursts, each
  disconnected at a random byte offset part way through what the client was
  reading, each resumed from wherever the client honestly got to. Every message
  has to arrive exactly once, in order, unaltered. That is a few thousand
  simulated disconnections, none of which needed a network.

Plus the usual: framing, the right justified space padded numeric fields in the
login handshake, message layouts by offset, orders refused before login, logout,
and 2,000 rounds of random bytes that have to be survivable rather than fatal.
