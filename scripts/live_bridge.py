#!/usr/bin/env python3
"""Hyperliquid's live WebSocket, reshaped into the lines the C++ side already reads.

This does one job and no more: TLS and the WebSocket handshake, which are not
worth 3,000 lines of C++ to avoid a dependency. Everything downstream, the
parsing, the book, the agent, is the same code the replay uses on the archived
files, and it cannot tell the difference because the lines are identical in shape
to the ones the collectors write.

Each line is a one character tag, a space, then the JSON:

    B {"rx": <ms>, "book": {...}}
    T {"rx": <ms>, "trade": {...}}

It writes to stdout, so it pipes straight into build/liveshadow.

Nothing here sends anything to the exchange except two subscribe messages. It
cannot place an order: the socket is a market data socket and there is no key
anywhere in this repository.
"""
import argparse
import asyncio
import json
import sys
import time

try:
    import websockets
except ImportError:
    sys.exit("pip install websockets")

URL = "wss://api.hyperliquid.xyz/ws"


async def run(coin, seconds, out):
    deadline = time.time() + seconds if seconds > 0 else None
    books = trades = 0
    backoff = 1.0
    while deadline is None or time.time() < deadline:
        try:
            async with websockets.connect(URL, ping_interval=20, ping_timeout=20,
                                          max_queue=1024) as ws:
                backoff = 1.0
                for kind in ("l2Book", "trades"):
                    await ws.send(json.dumps({
                        "method": "subscribe",
                        "subscription": {"type": kind, "coin": coin},
                    }))
                print(f"# subscribed to l2Book and trades for {coin}", file=sys.stderr,
                      flush=True)
                while deadline is None or time.time() < deadline:
                    timeout = None if deadline is None else max(0.1, deadline - time.time())
                    raw = await asyncio.wait_for(ws.recv(), timeout=timeout)
                    msg = json.loads(raw)
                    ch = msg.get("channel")
                    rx = int(time.time() * 1000)
                    if ch == "l2Book":
                        out.write("B " + json.dumps({"rx": rx, "book": msg["data"]}) + "\n")
                        books += 1
                    elif ch == "trades":
                        for t in msg.get("data", []):
                            out.write("T " + json.dumps({"rx": rx, "trade": t}) + "\n")
                            trades += 1
                    else:
                        continue
                    out.flush()
        except asyncio.TimeoutError:
            break
        except Exception as e:                     # noqa: BLE001
            # A dropped feed is a normal event, not a reason to stop.
            print(f"# reconnecting after {type(e).__name__}: {e}", file=sys.stderr,
                  flush=True)
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 30.0)
    print(f"# {books} book updates, {trades} trades", file=sys.stderr, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--coin", default="ETH")
    ap.add_argument("--seconds", type=float, default=0, help="0 runs until stopped")
    args = ap.parse_args()
    try:
        asyncio.run(run(args.coin, args.seconds, sys.stdout))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
