#!/usr/bin/env bash
# Reproduces every number in RESULTS.md. Each configuration runs REPS times;
# we keep all runs so run-to-run noise is visible (skill 6.9).
set -euo pipefail
cd "$(dirname "$0")"
REPS=${REPS:-3}
OUT=${OUT:-results}
mkdir -p "$OUT"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake -S . -B build-native -G Ninja -DCMAKE_BUILD_TYPE=Release -DEXCH_NATIVE=ON >/dev/null
ninja -C build >/dev/null
ninja -C build-native >/dev/null

{
  echo "# host"; uname -r; lscpu | grep -E 'Model name|^CPU\(s\)|L1d|L2|L3'; g++ --version | head -1
} > "$OUT/host.txt"

./build/scenario_test
./build/differential_test

./build/bench_jitter --seconds 5 --pin 2 > "$OUT/jitter.txt"

: > "$OUT/engine.txt"
for live in 40000 1000000; do
  warm=$(( live < 100000 ? 1000000 : 3000000 ))
  for eng in naive fast; do
    for r in $(seq "$REPS"); do
      ./build/bench_engine --engine "$eng" --live "$live" --warmup "$warm" --messages 5000000 >> "$OUT/engine.txt"
    done
  done
  for r in $(seq "$REPS"); do
    echo "[-march=native]" >> "$OUT/engine.txt"
    ./build-native/bench_engine --engine fast --live "$live" --warmup "$warm" --messages 5000000 >> "$OUT/engine.txt"
  done
  for r in $(seq "$REPS"); do
    echo "[hugepages]" >> "$OUT/engine.txt"
    ./build/bench_engine --engine fast --live "$live" --warmup "$warm" --messages 5000000 --hugepages >> "$OUT/engine.txt"
  done
done

: > "$OUT/pipeline.txt"
for rate in 200000 1000000; do
  for cfg in "none mutex" "none spsc --pin" "none seq --pin" \
             "naive mutex" "naive seq --pin" \
             "fast mutex" "fast spsc" "fast spsc --pin" "fast seq --pin"; do
    set -- $cfg
    for r in $(seq "$REPS"); do
      ./build/bench_pipeline --engine "$1" --queue "$2" ${3:-} --rate "$rate" --messages 1000000 --warmup 300000 \
        | tail -1 >> "$OUT/pipeline.txt"
    done
  done
done
echo "done: see $OUT/"
