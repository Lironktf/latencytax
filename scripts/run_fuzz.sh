#!/usr/bin/env bash
# Coverage guided fuzzing of everything that parses untrusted bytes.
#
# libFuzzer is a clang feature, so this builds separately from the main tree.
# Every target runs under the address and undefined behaviour sanitizers, and
# the book targets also assert the engine's own invariants after each operation,
# so a bookkeeping error is caught on the operation that caused it.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/results}
SECS=${SECS:-180}
BUILD=${BUILD:-$ROOT/build-fuzz}
mkdir -p "$OUT"

command -v clang++ >/dev/null || { echo "clang++ is needed for libFuzzer" >&2; exit 1; }

CC=clang CXX=clang++ cmake -B "$BUILD" "$ROOT/fuzz" -DLTX_ROOT="$ROOT" > /dev/null
cmake --build "$BUILD" -j"$(nproc)" > /dev/null

: > "$OUT/fuzz.txt"
STATUS=0
for t in fuzz_itch fuzz_soup fuzz_book fuzz_json; do
  case "$t" in
    fuzz_itch) CORP=itch ;;
    fuzz_soup) CORP=soup ;;
    fuzz_book) CORP=book ;;
    fuzz_json) CORP=json ;;
  esac
  echo "== $t, ${SECS}s ==" | tee -a "$OUT/fuzz.txt"
  mkdir -p "$ROOT/fuzz/corpus/$CORP" "$ROOT/fuzz/artifacts"
  # -rss_limit_mb keeps a pathological input from taking the machine with it.
  if "$BUILD/$t" "$ROOT/fuzz/corpus/$CORP" \
      -max_total_time="$SECS" -rss_limit_mb=2048 -timeout=25 -print_final_stats=1 \
      -artifact_prefix="$ROOT/fuzz/artifacts/$t-" > "$OUT/$t.log" 2>&1; then
    grep -E "^#[0-9]+.*(DONE|pulse)|stat::|cov:" "$OUT/$t.log" | tail -4 | tee -a "$OUT/fuzz.txt"
    echo "  no crashes" | tee -a "$OUT/fuzz.txt"
  else
    STATUS=1
    echo "  FAILED, see $OUT/$t.log" | tee -a "$OUT/fuzz.txt"
    tail -30 "$OUT/$t.log" | tee -a "$OUT/fuzz.txt"
  fi
  echo | tee -a "$OUT/fuzz.txt"
done

echo "corpus after the run:" | tee -a "$OUT/fuzz.txt"
for c in itch soup book json; do
  printf '  %-6s %5s inputs\n' "$c" "$(ls "$ROOT/fuzz/corpus/$c" | wc -l)" | tee -a "$OUT/fuzz.txt"
done
exit $STATUS
