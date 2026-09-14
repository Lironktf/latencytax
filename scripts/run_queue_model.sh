#!/usr/bin/env bash
# Reproduces every number in experiments/002_queue_model/results.md.
set -euo pipefail

DATA=${DATA:-data/raw}
OUT=${OUT:-results}
BOOT=${BOOT:-10000}
mkdir -p "$OUT"

CAL="2026-08-09,2026-08-10,2026-08-11,2026-08-12"
HOLD="2026-08-13,2026-08-14,2026-08-16,2026-08-17,2026-08-18"
LATS="0.1,1,10,33,66,100,200,500,1000,2000,5000"
K=8.6820
BETA=0.00008052
GATE=${GATE:-0.40}

echo "== 1. datasets =="
./build/mlgen --data="$DATA" --days="$CAL"  --out="$OUT/queue_train.bin" --quiet
./build/mlgen --data="$DATA" --days="$HOLD" --out="$OUT/queue_test.bin"  --quiet

echo
echo "== 2. train, with every choice made on the validation day =="
./build/mltrain --train="$OUT/queue_train.bin" --test="$OUT/queue_test.bin" \
  --save="$OUT/queue_model.bin" | tee "$OUT/queue_model.txt"

echo
echo "== 3. gate threshold, chosen on the calibration days =="
{
  for g in 0.0 0.10 0.20 0.30 0.40 0.50 0.60; do
    printf 'gate=%s  ' "$g"
    ./build/sim --data="$DATA" --days="$CAL" --k=$K --latency=1 --gamma=5 --requote=3 \
      --beta=$BETA --queue-model="$OUT/queue_model.bin" --gate=$g --quiet | tail -1
  done
} | tee "$OUT/gate_calibration.txt"

echo
echo "== 4. the gated agent on the holdout, latency swept =="
./build/sim --data="$DATA" --days="$HOLD" --k=$K --latency=$LATS --gamma=5 --offset=0 \
  --requote=3 --beta=$BETA --kappa=1.0 --queue-model="$OUT/queue_model.bin" --gate=$GATE \
  --csv="$OUT/holdout_gated.csv" --hourly-csv="$OUT/holdout_gated_hourly.csv" --quiet > /dev/null
python3 scripts/analyse.py --daily="$OUT/holdout_gated.csv" \
  --hourly="$OUT/holdout_gated_hourly.csv" --gamma=5 --requote=3 --boot=$BOOT \
  --slope-from=1000 --slope-to=5000 | tee "$OUT/holdout_gated.txt"

echo
echo "== 5. gated against ungated, paired by day =="
if [ ! -f "$OUT/holdout_daily.csv" ]; then
  echo "run scripts/run_experiment.sh first, it produces the ungated baseline" >&2
  exit 1
fi
python3 scripts/compare_runs.py --a="$OUT/holdout_daily.csv" --b="$OUT/holdout_gated.csv" \
  --label-a=ungated --label-b=gated --boot=$BOOT | tee "$OUT/gate_compare.txt"

echo
echo "== 6. kernels =="
./build/bench --kernels --core="${CORE:-2}" | tee "$OUT/kernels.txt"

echo
echo "all outputs are under $OUT/"
