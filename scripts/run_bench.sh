#!/usr/bin/env bash
# Engine benchmark. Run it on an otherwise idle machine: the numbers are in
# nanoseconds and anything else using the cores or the memory bus will show up.
set -euo pipefail
OUT=${OUT:-results}
SECONDS_PER_PHASE=${SECONDS_PER_PHASE:-30}
CORE=${CORE:-2}
FEED_CORE=${FEED_CORE:-3}
mkdir -p "$OUT"

steal() { awk '/^cpu /{print $9}' /proc/stat; }
S0=$(steal)

for ML in 2000 20000 60000 200000; do
  echo "== steady state book of $ML resting orders =="
  ./build/bench --seconds="$SECONDS_PER_PHASE" --core="$CORE" --feed-core="$FEED_CORE" \
    --max-live="$ML" --csv | tee "$OUT/bench_${ML}.txt" | grep -v '^csv'
  echo
done

# Same binary, same flow, one flag. The book's three big arrays come to about
# 70 MB at this size, which is more pages than the data TLB has entries.
echo "== huge pages, on and off, 200000 resting orders =="
for HP in 0 1; do
  echo "-- hugepages=$HP --"
  ./build/bench --seconds="$SECONDS_PER_PHASE" --core="$CORE" --feed-core="$FEED_CORE" \
    --max-live=200000 --hugepages="$HP" | tee "$OUT/bench_hp${HP}.txt" \
    | grep -E 'book arrays|^all |single thread throughput'
  echo
done

echo "== tick to trade, four transports =="
./build/ticktotrade --all --seconds=20 --feed-core="$FEED_CORE" --engine-core="$CORE" \
  --quiet | tee "$OUT/ticktotrade.txt"
echo

echo "== order entry: soupbintcp sessions carrying ouch, with a forced disconnect =="
./build/ouchgw --orders=20000 --drop-at=5000 --burst=800 --server-core="$CORE" \
  --client-core="$FEED_CORE" | tee "$OUT/ouchgw.txt"
echo

echo "== ml kernels =="
./build/bench --kernels --core="$CORE" | tee "$OUT/kernels.txt" | grep -v '^csv'
echo

S1=$(steal)
echo "cpu steal during the run: $((S1-S0)) ticks"
