# Tick to trade, and the live shadow

Back to the [README](../README.md).

## Tick to trade

`tools/ticktotrade.cpp`.

Every other benchmark in this repository measures the engine. That is the part I
wrote, and quoting it on its own would be misleading, because the number a
trading firm cares about is wire to wire: a market data packet arrives, and the order it provoked leaves.
This measures that, breaks it into stages, and runs it over four transports so
that "kernel bypass would help" stops being a claim and becomes a number.

```
./build/ticktotrade --all --seconds=20 --feed-core=3 --engine-core=2
```

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="latency_budget_dark.svg">
  <img alt="Stacked bars of the tick to trade budget for four transports. UDP on loopback totals 11.1 microseconds, of which the kernel receive path is 5.3, the engine 0.4 and the kernel send path 4.9. AF_UNIX totals 5.6 and the SPSC ring 1.1 microseconds. A second panel shows the ring row to scale: 353 nanoseconds in, 363 in the engine, 51 out and 289 in flight." src="latency_budget.svg" width="900">
</picture>

Regenerated from the measurement rather than drawn, so it cannot drift:

```
./build/ticktotrade --all --seconds=20 --quiet > results/ticktotrade.txt
python3 scripts/latency_svg.py
```

The same numbers as a table, since a picture is not a data source:

| transport | kernel in | engine | kernel out | in flight | total |
|---|---|---|---|---|---|
| UDP loopback | 5,281 | 424 | 4,915 | 466 | 11,086 |
| UDP, connected + recvmmsg | 5,005 | 375 | 4,440 | 565 | 10,385 |
| AF_UNIX datagrams | 2,659 | 341 | 1,468 | 1,097 | 5,565 |
| SPSC ring, no kernel | 353 | 363 | 51 | 289 | 1,056 |

The stages are measured separately and the total end to end, so the gap between
them is the part of the return leg after the send call returns. It is shown as
its own segment rather than absorbed into a neighbour.

And the full breakdown, medians in nanoseconds, from a separate 20 second run:

| transport | in | decode | book | decide | out | engine | **total** |
|---|---|---|---|---|---|---|---|
| udp on loopback | 5,284 | 55 | 229 | 8 | 4,848 | 348 | **11,096** |
| udp, connected sockets and recvmmsg | 4,921 | 71 | 216 | 8 | 4,413 | 351 | **10,258** |
| af_unix datagrams | 2,548 | 58 | 206 | 8 | 1,450 | 326 | **5,426** |
| spsc ring, no kernel in the path | 333 | 66 | 201 | 8 | 50 | 335 | **999** |

| transport | p50 | p99 | p99.9 | engine's share |
|---|---|---|---|---|
| udp on loopback | 11,096 | 54,599 | 436,897 | 3.1% |
| udp, connected and recvmmsg | 10,258 | 54,599 | 218,441 | 3.4% |
| af_unix | 5,426 | 54,599 | 109,213 | 6.0% |
| spsc ring | 999 | 18,415 | 109,213 | 33.5% |

Reading down that ladder:

- **Connecting the sockets is worth 7.5%.** A connected datagram socket pins the
  route, so every later call skips the address copy and the lookup. `recvmmsg` is
  in the same row and does nothing at this pacing, where there is one message
  waiting at a time; it is there because it is what a real handler does when the
  feed bursts, and leaving it out would flatter the row. `SO_BUSY_POLL` is asked
  for and refused, and the tool says so: loopback is not a NAPI device, so busy
  polling had nothing to poll.
- **Dropping IP and UDP halves it.** AF_UNIX has the same syscalls and the same
  scheduling with none of the protocol processing, so the 11,096 to 5,426 gap is
  what the stack costs.
- **Dropping the kernel entirely takes it to 999 ns, eleven times better than
  UDP.** That is the lock free ring from `src/engine`, and it is not a bypass NIC.
  It is the floor a bypass NIC is trying to reach, and having it measured is the
  difference between saying kernel bypass is worth an order of magnitude and
  showing it.
- **The engine only starts to matter at the bottom.** It is 3.1% of the UDP path
  and 33.5% of the ring path. Making the book twice as fast moves the first by
  1.6% and the last by 17%. That is the honest shape of the problem, and it is
  why the industry buys network cards before it buys compilers.

The ring row first reported 1.7 ms on a path whose own stages summed to under
700 ns. The feed was reading whatever reply happened to be waiting, which is
harmless on a transport that drops when it is full and badly wrong on one that
does not: one missed reply left a permanent backlog and every later sample timed
a request from thousands of iterations earlier. Replies are matched to their
request now.

None of this is a network. No NIC, no wire, no switch, no bypass stack. Every
figure is a floor on what a real path would cost.

---

## The live shadow

`scripts/live_bridge.py`, `tools/liveshadow.cpp`.

Everything else in this repository is a replay of files. This is the same engine,
the same reconstruction and the same market making agent, pointed at
Hyperliquid's live WebSocket, with a page to watch it on.

```
pip install websockets
./scripts/run_live.sh          # then open http://localhost:8080
```

**It places no orders.** The bridge opens a market data socket and sends two
subscribe messages. There is no key anywhere in this repository and nothing here
can reach an order entry endpoint. The quotes on the page are quotes the agent
would have posted; the fills are what the queue model says would have happened to
them. It is a simulation standing next to a live market, not participation in
one.

The bridge is 90 lines of Python doing exactly one job: TLS and the WebSocket
handshake, which are not worth a dependency in the C++ tree to avoid. It reshapes
each message into the same line the collectors write, so the C++ side cannot tell
whether it is reading today's market or a file from August:

```
B {"rx": 1789428953160, "book": {"coin":"ETH","time":...,"levels":[[...],[...]]}}
T {"rx": 1789428953171, "trade": {"coin":"ETH","side":"A","px":"2517.5",...}}
```

That is not a coincidence, it is the point. `Reconstructor::run` was rewritten in
terms of two incremental calls, `push_snapshot` and `push_trade`, and the live
path calls the same two. There is one implementation of the reconstruction, not
one for files and one for sockets, and the whole 39 day fidelity result was
re-run afterwards to confirm it had not moved by a single command: 639,262
windows, 24,183,264 commands, 0 of 25,570,480 level positions wrong.

**The reconstruction checks itself live.** Every time a snapshot closes a window,
the same comparison the replay makes is made against the exchange's own book, and
the page shows the running count. If it stops being zero while you are watching,
something is wrong and you can watch it happen.

A sample of what it reports, from a run against the live feed:

```json
{"uptime_s": 25.0, "books": 5, "trades": 103, "parse_errors": 0,
 "windows": 4, "recon_mismatch": 0, "unexpected_trades": 0,
 "best_bid": 2514.6, "best_ask": 2514.7,
 "agent_bid": 2514.6, "agent_ask": 2515.7,
 "inventory": -0.5, "fills": 3, "swept": 3, "equity": -1.19071}
```

The page is served by about 80 lines of socket code inside the tool, refreshes
once a second, follows the reader's colour scheme, and pulls nothing from the
network. It runs on your machine and is not hosted anywhere.
