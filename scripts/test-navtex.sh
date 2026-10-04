#!/bin/bash
# test-navtex.sh — NAVTEX decoding, measured (audit 2026-10-04, rows 11-13): the real decoder (decoders/fsk_decoder.cpp,
# CCIR476) on synthetic SITOR-B from tools/rtty-bench/synth_navtex.py, scored by character error rate (score.py).
#  1. Impulse bursts (20/min, 20-200 ms, 20 dB over the signal) at 15 dB SNR, four seeds — the old decoder resynced on
#     any 3 bad words, FEC-repaired or not, and threw away the shift and the DX/RX phase: CER 14.1 / 5.6 / 4.7 / 13.4 %
#     (mean 9.5). New: 0.9 / 1.2 / 1.1 / 3.2 %.
#  2. Bursts (12/min, 10 dB) on 12 dB independent mark/space fading — old 14.3 / 16.5 %, new 1.2 / 0.8 %.
#  3. FIGS 0x4B (BEL) is printed as an apostrophe (fldigi), never as the raw control byte.
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
C=android/app/src/main/cpp
c++ -std=c++17 -O2 -I$C -o $T/nav tools/rtty-bench/rtty_wav.cpp $C/decoders/fsk_decoder.cpp $C/decoders/rtty_auto.cpp || { echo "  ✗ build"; exit 1; }
fail=0
ok() { if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fail=1; fi; }
cer() {   # cer snr fade bursts/min burst_db seed  → the CER in %, and the decode in $T/last.txt
  python3 tools/rtty-bench/synth_navtex.py $T/s.wav $1 $2 120 $3 $4 $5 >/dev/null
  $T/nav $T/s.wav CCIR476 > $T/last.txt 2>/dev/null
  python3 tools/rtty-bench/score.py $T/last.txt $T/s.wav.txt | sed 's/^CER \([0-9.]*\).*/\1/'
}
all=""; sum=0
for seed in 0 1 2 3; do
  c=$(cer 15 0 20 20 $seed); all="$all $c"
  awk "BEGIN{exit !($c <= 5)}"; ok $? "bursts 20/min @20 dB, seed $seed: CER $c % (≤ 5)"
  sum=$(awk "BEGIN{print $sum + $c}")
done
mean=$(awk "BEGIN{printf \"%.1f\", $sum / 4}")
awk "BEGIN{exit !($mean <= 3)}"; ok $? "bursts: mean CER $mean % (≤ 3; old decoder 9.5 %)"
for seed in 0 1; do
  c=$(cer 15 12 12 10 $seed)
  awk "BEGIN{exit !($c <= 3)}"; ok $? "bursts on 12 dB selective fading, seed $seed: CER $c % (≤ 3; old 14.3 / 16.5 %)"
done
c=$(cer 15 0 0 0 0)
awk "BEGIN{exit !($c <= 0.5)}"; ok $? "clean 15 dB: CER $c %"
n=$(tr -cd '\007' < $T/last.txt | wc -c | tr -d ' ')
[ "$n" -eq 0 ]; ok $? "no BEL (0x07) in the output ($n; old decoder: 2)"
grep -q "^'NNNN" $T/last.txt; ok $? "...it is printed as an apostrophe, as fldigi does"
exit $fail
