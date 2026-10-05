#!/bin/bash
# navtex_bench.sh — the NAVTEX decoder's CER over a matrix of synthetic conditions (synth_navtex.py + score.py), for
# before/after tables (2026-10-05). Each row is the mean CER over the seeds.
#
#   tools/rtty-bench/navtex_bench.sh [cpp_dir=android/app/src/main/cpp] [seeds="0 1 2"]
#
# cpp_dir lets an OLD decoder be measured: e.g. `git worktree`/`git show main:…` the decoders into a folder with the
# same layout (decoders/fsk_decoder.{h,cpp}, decoders/rtty_auto.{h,cpp}) and pass it here.
# ★ Scored against the WHOLE reference (SCORE_FULL=1): a decoder that prints less is not let off the missing tail.
# Env NAVTEX_OPTS is passed through to the decoder (rtty_wav), so one build can be measured with a feature off.
set -u
cd "$(dirname "$0")/../.."
C=${1:-android/app/src/main/cpp}; SEEDS=${2:-0 1 2}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
WAV=tools/rtty-bench/rtty_wav.cpp; [ -f $C/rtty_wav.cpp ] && WAV=$C/rtty_wav.cpp   # an old decoder brings its own harness
c++ -std=c++17 -O2 -I$C -o $T/nav $WAV $C/decoders/fsk_decoder.cpp $C/decoders/rtty_auto.cpp || { echo "build failed"; exit 1; }
# name | env for the synth | snr fade bursts/min burst_db | extra decoder args
ROWS=(
  "clean 15 dB|                |15 0 0 0"
  "SNR 0 dB|                   |0 0 0 0"
  "SNR -3 dB|                  |-3 0 0 0"
  "SNR -6 dB|                  |-6 0 0 0"
  "SNR -9 dB|                  |-9 0 0 0"
  "bursts 20/min +20 dB, 15 dB|  |15 0 20 20"
  "fade 12 dB + bursts, 15 dB| |15 12 12 10"
  "fade 20 dB, 6 dB|           |6 20 0 0"
  "mistune +15 Hz, 3 dB|CF=515  |3 0 0 0"
  "mistune -30 Hz, 6 dB|CF=470  |6 0 0 0"
  "mistune +50 Hz, -3 dB|CF=550 |-3 0 0 0"
  "mistune -50 Hz, -3 dB|CF=450 |-3 0 0 0"
  "clock +0.05 %, 3 dB|BAUD=100.05|3 0 0 0"
  "join mid-message, 6 dB|JOIN=23|6 0 0 0"
  "inverted tones, 10 dB|INV=1  |10 0 0 0"
)
printf "%-30s %s\n" "condition" "CER % (seeds: $SEEDS) → mean"
for row in "${ROWS[@]}"; do
  IFS='|' read -r name env args <<< "$row"
  all=""; sum=0; n=0
  for seed in $SEEDS; do
    set -- $args   # snr fade bursts/min burst_db
    env $env python3 tools/rtty-bench/synth_navtex.py $T/s.wav $1 $2 120 $3 $4 $seed >/dev/null
    $T/nav $T/s.wav CCIR476 > $T/out.txt 2>/dev/null
    c=$(SCORE_FULL=1 python3 tools/rtty-bench/score.py $T/out.txt $T/s.wav.txt | sed 's/^CER \([0-9.]*\).*/\1/')
    all="$all $c"; sum=$(awk "BEGIN{print $sum + $c}"); n=$((n+1))
  done
  printf "%-30s %-24s %6.1f\n" "$name" "$all" "$(awk "BEGIN{print $sum / $n}")"
done
