#!/bin/bash
# sitor_bench.sh — the SITOR-A listener's CER over a matrix of synthetic links (synth_sitor_a.py + rtty-bench/score.py).
# Each row: the mean CER over the seeds, the characters printed twice (a block printed again = a failure of the
# repeat handling), and the identities read. The text is scored as one stream — all stations, in order.
#
#   tools/sitor-bench/sitor_bench.sh [seeds="0 1 2"] [secs=150]
# Env SITOR_OPTS is passed to the decoder (combine=0, afc=0, timing=0), so one build can be measured with a feature off.
set -u
cd "$(dirname "$0")/../.."
SEEDS=${1:-0 1 2}; SECS=${2:-150}
C=android/app/src/main/cpp
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
nice c++ -std=c++17 -O2 -I $C -o $T/sitor tools/sitor-bench/sitor_wav.cpp $C/decoders/sitor_a.cpp || { echo "build failed"; exit 1; }
# name | env for the synth | snr fade bursts/min burst_db
ROWS=(
  "clean 20 dB|SLAVE_DB=0           |20 0 0 0"
  "SNR 5 dB|SLAVE_DB=0              |5 0 0 0"
  "SNR 0 dB|SLAVE_DB=0              |0 0 0 0"
  "SNR -3 dB|SLAVE_DB=0             |-3 0 0 0"
  "SNR -4.5 dB|SLAVE_DB=0           |-4.5 0 0 0"
  "SNR -6 dB|SLAVE_DB=0             |-6 0 0 0"
  "SNR -9 dB|SLAVE_DB=0             |-9 0 0 0"
  "fade 20 dB, 10 dB|SLAVE_DB=0     |10 20 0 0"
  "fade 20 dB, 3 dB|SLAVE_DB=0      |3 20 0 0"
  "bursts 20/min +20 dB, 15 dB|SLAVE_DB=0|15 0 20 20"
  "slave -6 dB, 10 dB|              |10 0 0 0"
  "heavy RQ 35 %, 15 dB|RQP=0.35 CSP=0.1 SLAVE_DB=0|15 0 0 0"
  "heavy RQ 35 %, -3 dB|RQP=0.35 CSP=0.1 SLAVE_DB=0|-3 0 0 0"
  "4-signal call, 10 dB|CALL=4 SLAVE_DB=0|10 0 0 0"
  "master only heard, 10 dB|ONLY=M    |10 0 0 0"
  "slave -15 dB, 10 dB|SLAVE_DB=-15   |10 0 0 0"
  "mistune +40 Hz, 10 dB|CF=1040 SLAVE_HZ=52 SLAVE_DB=0|10 0 0 0"
  "clock +0.01 %, 5 dB|BAUD=100.01 SLAVE_DB=0|5 0 0 0"
  "tE 5 ms, 10 dB|D_MS=5 SLAVE_DB=0|10 0 0 0"
  "inverted tones, 10 dB|INV=1 SLAVE_DB=0|10 0 0 0"
)
printf "%-28s %-22s %6s %6s  %s\n" "condition" "CER % per seed" "mean" "dups" "identities (last seed)"
for row in "${ROWS[@]}"; do
  IFS='|' read -r name env args <<< "$row"
  all=""; sum=0; n=0; dups=0
  for seed in $SEEDS; do
    set -- $args
    env $env python3 tools/sitor-bench/synth_sitor_a.py $T/s.wav $1 $2 $SECS $3 $4 $seed >/dev/null
    SITOR_QUIET=1 $T/sitor $T/s.wav > $T/out.txt 2>$T/err.txt
    sed -E 's/^\[[0-9]\] //' $T/out.txt | tr -d '\n' > $T/o.txt; tr -d '\n' < $T/s.wav.txt > $T/r.txt
    c=$(SCORE_FULL=1 python3 tools/rtty-bench/score.py $T/o.txt $T/r.txt | sed 's/^CER \([0-9.]*\).*/\1/')
    # ★ duplicates: 3-character runs the decode has more often than the reference (a repeated block printed twice)
    d=$(python3 - "$T/o.txt" "$T/r.txt" <<'PY'
import sys, collections
o, r = open(sys.argv[1]).read(), open(sys.argv[2]).read()
def grams(s, n=9): return collections.Counter(s[i:i + n] for i in range(len(s) - n + 1))
go, gr = grams(o), grams(r)
print(sum(max(0, c - gr.get(g, 0)) for g, c in go.items() if g in gr and c > gr[g]))
PY
)
    dups=$((dups + d))
    all="$all $c"; sum=$(awk "BEGIN{print $sum + $c}"); n=$((n+1))
  done
  ids=$(grep -o "ids \[[^]]*\] numbers \[[^]]*\]" $T/err.txt | sed 's/ids //')
  printf "%-28s %-22s %6.1f %6d  %s\n" "$name" "$all" "$(awk "BEGIN{print $sum / $n}")" "$dups" "$ids"
done
