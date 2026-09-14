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

S1=$(steal)
echo "cpu steal during the run: $((S1-S0)) ticks"
