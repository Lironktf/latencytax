#!/usr/bin/env bash
# Reproduces every number in experiments/001_latency_tax/results.md and in the
# experiment section of the README. Run from the repository root after
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
set -euo pipefail

DATA=${DATA:-data/raw}
OUT=${OUT:-results}
BOOT=${BOOT:-10000}
mkdir -p "$OUT"

CAL="2026-08-09,2026-08-10,2026-08-11,2026-08-12"
# 2026-08-15 is 99.85% complete and is excluded from the holdout by the
# pre-registration. The six day version is produced below as a sensitivity.
HOLD="2026-08-13,2026-08-14,2026-08-16,2026-08-17,2026-08-18"
HOLD6="2026-08-13,2026-08-14,2026-08-15,2026-08-16,2026-08-17,2026-08-18"
# Untouched until the holdout was written down. 2026-08-19 is 31% collected and
# 2026-09-14 has no trade file, so both are dropped.
EXT_FROM="2026-08-20"
EXT_TO="2026-09-13"

LATS="0.1,1,10,33,66,100,200,500,1000,2000,5000"
K=8.6820
BETA=0.00008052

echo "== 1. engine fidelity over every day present =="
./build/replay --data="$DATA" --csv="$OUT/fidelity_all_days.csv" | tee "$OUT/fidelity.txt"

echo
echo "== 1b. structure of the tape: the gap histogram and the aggressor side check =="
python3 scripts/tape_structure.py --data="$DATA" --from=2026-08-09 --to=2026-08-12 \
  | tee "$OUT/tape_structure_calibration.txt"
python3 scripts/tape_structure.py --data="$DATA" | tee "$OUT/tape_structure_all_days.txt"
python3 scripts/tape_structure.py --data="$DATA" --days="$HOLD" \
  | tee "$OUT/tape_structure_holdout.txt"

echo
echo "== 2. calibration: fit the AS arrival decay and the order flow coefficient =="
./build/sim --data="$DATA" --days="$CAL" --fit-k    | tee "$OUT/fit_k.txt"
./build/sim --data="$DATA" --days="$CAL" --fit-beta | tee "$OUT/fit_beta.txt"

echo
echo "== 3. calibration grid, 36 configurations, calibration days only =="
./build/sim --data="$DATA" --days="$CAL" --k=$K --latency=1 \
  --gamma=0,5,20 --offset=0,1 --requote=0,1,3 --beta=0,$BETA \
  --csv="$OUT/calibration_grid.csv" --quiet | tee "$OUT/calibration_grid.txt"

echo
echo "== 4. holdout, frozen configuration, run once =="
./build/sim --data="$DATA" --days="$HOLD" --k=$K --latency=$LATS \
  --gamma=0,5,20 --offset=0 --requote=0,1,3 --beta=$BETA --kappa=1.0 \
  --csv="$OUT/holdout_daily.csv" --hourly-csv="$OUT/holdout_hourly.csv" --quiet > /dev/null

echo "-- primary: gamma 5, requote 3 --"
python3 scripts/analyse.py --daily="$OUT/holdout_daily.csv" --hourly="$OUT/holdout_hourly.csv" \
  --gamma=5 --requote=3 --boot=$BOOT --slope-from=1000 --slope-to=5000 | tee "$OUT/holdout_primary.txt"
echo "-- companion: gamma 0 (inventory skew off) --"
python3 scripts/analyse.py --daily="$OUT/holdout_daily.csv" --hourly="$OUT/holdout_hourly.csv" \
  --gamma=0 --requote=3 --boot=$BOOT --slope-from=1000 --slope-to=5000 | tee "$OUT/holdout_companion.txt"

echo "-- inventory skew on versus off --"
python3 scripts/skew_compare.py --daily="$OUT/holdout_daily.csv" --boot=$BOOT \
  | tee "$OUT/skew.txt"
echo "-- markout of the average maker on the same days, no agent, no fill model --"
./build/sim --data="$DATA" --days="$HOLD" --tape-markout | tee "$OUT/tape_markout_holdout.txt"

echo
echo "== 5. agent sensitivity: every (gamma, requote) pair on the holdout =="
: > "$OUT/holdout_config_cross.txt"
for g in 0 5 20; do for rq in 0 1 3; do
  echo "--- gamma=$g requote=$rq ---" >> "$OUT/holdout_config_cross.txt"
  python3 scripts/analyse.py --daily="$OUT/holdout_daily.csv" --gamma=$g --requote=$rq \
    --boot=$BOOT >> "$OUT/holdout_config_cross.txt"
done; done
cat "$OUT/holdout_config_cross.txt"

echo
echo "== 6. sensitivities: queue decay, sweep rule, fee tier, sixth day =="
./build/sim --data="$DATA" --days="$HOLD" --k=$K --latency=$LATS --gamma=0,5 --offset=0 \
  --requote=3 --beta=$BETA --kappa=0,0.5,1.0 --csv="$OUT/holdout_kappa.csv" --quiet > /dev/null
./build/sim --data="$DATA" --days="$HOLD" --k=$K --latency=$LATS --gamma=0,5 --offset=0 \
  --requote=3 --beta=$BETA --kappa=1.0 --sweep=queue \
  --csv="$OUT/holdout_sweepqueue.csv" --quiet > /dev/null
./build/sim --data="$DATA" --days="$HOLD" --k=$K --latency=$LATS --gamma=0,5 --offset=0 \
  --requote=3 --beta=$BETA --kappa=1.0 --maker-bps=0.0 --taker-bps=2.8 \
  --csv="$OUT/holdout_tier4.csv" --quiet > /dev/null
./build/sim --data="$DATA" --days="$HOLD6" --k=$K --latency=$LATS --gamma=0,5 --offset=0 \
  --requote=3 --beta=$BETA --kappa=1.0 --csv="$OUT/holdout_6day.csv" --quiet > /dev/null

{
  for kp in 0 0.5 1.0; do
    echo "--- kappa=$kp, gamma=5 ---"
    python3 scripts/analyse.py --daily="$OUT/holdout_kappa.csv" --gamma=5 --requote=3 \
      --kappa=$kp --boot=$BOOT --slope-from=1000 --slope-to=5000
  done
  echo "--- sweep rule = queue, gamma=5 ---"
  python3 scripts/analyse.py --daily="$OUT/holdout_sweepqueue.csv" --gamma=5 --requote=3 \
    --boot=$BOOT --slope-from=1000 --slope-to=5000
  echo "--- fee tier 4 (maker 0.000%), gamma=5 ---"
  python3 scripts/analyse.py --daily="$OUT/holdout_tier4.csv" --gamma=5 --requote=3 \
    --boot=$BOOT --slope-from=1000 --slope-to=5000
  echo "--- six day holdout including 2026-08-15, gamma=5 ---"
  python3 scripts/analyse.py --daily="$OUT/holdout_6day.csv" --gamma=5 --requote=3 \
    --boot=$BOOT --slope-from=1000 --slope-to=5000
} | tee "$OUT/holdout_sensitivity.txt"

echo
echo "== 7. extension set, untouched until the holdout was written down =="
./build/sim --data="$DATA" --from=$EXT_FROM --to=$EXT_TO --k=$K --latency=$LATS \
  --gamma=0,5 --offset=0 --requote=3 --beta=$BETA --kappa=1.0 \
  --csv="$OUT/extension_daily.csv" --hourly-csv="$OUT/extension_hourly.csv" --quiet > /dev/null
{
  echo "--- extension, gamma=5 ---"
  python3 scripts/analyse.py --daily="$OUT/extension_daily.csv" \
    --hourly="$OUT/extension_hourly.csv" --gamma=5 --requote=3 --boot=$BOOT --slope-from=1000 --slope-to=5000
  echo "--- extension, gamma=0 ---"
  python3 scripts/analyse.py --daily="$OUT/extension_daily.csv" \
    --hourly="$OUT/extension_hourly.csv" --gamma=0 --requote=3 --boot=$BOOT --slope-from=1000 --slope-to=5000
} | tee "$OUT/extension.txt"

echo
echo "all outputs are under $OUT/"
