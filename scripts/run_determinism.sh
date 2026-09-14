#!/usr/bin/env bash
# One input, many configurations, one hash per symbol.
#
# Comparing final book state only proves two runs ended in the same place. For a
# matching engine the way it got there is the product, so what is compared here
# is a rolling hash over every field of every event the engine emitted, in order:
# every fill, every acknowledgement, every rejection.
#
# The configurations are chosen to break a different assumption each:
#   one thread            the baseline
#   two and three shards  breaks any dependence on thread interleaving
#   journal replay        breaks any dependence on where the bytes came from
#   -O0 instead of -O3    breaks any dependence on undefined behaviour that the
#                         optimiser is free to read differently
#   4 KB instead of 2 MB  breaks any dependence on the memory mapping
#
# The hash is per symbol, not global. A global one would depend on how events
# from different symbols interleaved, which is a property of the schedule rather
# than of the engine, and asserting it would be asserting the wrong thing.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT=${OUT:-results}
DAY=${DAY:-2026-08-13}
SYMS=${SYMS:-4}
FEED=$OUT/determinism.itch
JRNL=$OUT/determinism.jrnl
mkdir -p "$OUT"

hashes() { grep '^EVENT HASH' | awk '{print $3, $4}'; }

echo "building the feed: $DAY under $SYMS symbols"
./build/itchgen --days="$DAY" --symbols="$SYMS" --out="$FEED" --quiet > /dev/null

echo "building the same sources at -O0 for the optimisation level check"
cmake -B build-o0 -DCMAKE_BUILD_TYPE=Release -DLTX_EXTRA_FLAGS=-O0 > /dev/null
cmake --build build-o0 --target itchfeed -j"$(nproc)" > /dev/null

declare -a NAMES=()
declare -a FILES=()

run() {
  local name="$1"; shift
  local f="$OUT/det-$(echo "$name" | tr ' /' '__').txt"
  "$@" | hashes > "$f"
  NAMES+=("$name")
  FILES+=("$f")
  printf '  %-34s %s\n' "$name" "$(head -1 "$f" | awk '{print $2}')"
}

echo
echo "running:"
run "one thread"            ./build/itchfeed --event-hash --quiet --journal="$JRNL" "$FEED"
run "two shards"            ./build/itchfeed --event-hash --quiet --shards=2 "$FEED"
run "three shards"          ./build/itchfeed --event-hash --quiet --shards=3 "$FEED"
run "replayed from journal" ./build/itchfeed --event-hash --quiet --from-journal="$JRNL"
run "built at -O0"          ./build-o0/itchfeed --event-hash --quiet "$FEED"
run "4 KB pages"            ./build/itchfeed --event-hash --quiet --hugepages=0 "$FEED"

echo
BASE=${FILES[0]}
FAIL=0
for i in "${!FILES[@]}"; do
  if ! diff -q "$BASE" "${FILES[$i]}" > /dev/null; then
    echo "MISMATCH: ${NAMES[$i]} differs from ${NAMES[0]}"
    diff "$BASE" "${FILES[$i]}" | head -10
    FAIL=1
  fi
done

if [ "$FAIL" = 0 ]; then
  echo "all $(( ${#FILES[@]} )) configurations agree, on all $(wc -l < "$BASE") symbols"
  echo
  cat "$BASE" | while read -r sym h; do printf '  %-10s %s\n' "$sym" "$h"; done
fi
rm -f "$FEED" "$JRNL"
exit $FAIL
