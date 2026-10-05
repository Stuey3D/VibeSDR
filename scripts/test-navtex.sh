#!/bin/bash
# test-navtex.sh — NAVTEX decoding, measured (audit 2026-10-04, rows 11-13): the real decoder (decoders/fsk_decoder.cpp,
# CCIR476) on synthetic SITOR-B from tools/rtty-bench/synth_navtex.py, scored by character error rate (score.py).
#  1. Impulse bursts (20/min, 20-200 ms, 20 dB over the signal) at 15 dB SNR, four seeds — the old decoder resynced on
#     any 3 bad words, FEC-repaired or not, and threw away the shift and the DX/RX phase: CER 14.1 / 5.6 / 4.7 / 13.4 %
#     (mean 9.5). New: 0.9 / 1.2 / 1.1 / 3.2 %.
#  2. Bursts (12/min, 10 dB) on 12 dB independent mark/space fading — old 14.3 / 16.5 %, new 1.2 / 0.8 %.
#  3. FIGS 0x4B (BEL) is printed as an apostrophe (fldigi), never as the raw control byte.
#  4. ★ NavtexRx (2026-10-05, fldigi's receiver + ML FEC), scored against the WHOLE reference (SCORE_FULL): -3 dB SNR
#     (old 21.1 / 17.9 %), 20 dB selective fading at 6 dB (old 17.4 / 95.2 %), joining mid-message with no phasing
#     (old 8.2 %), swapped tones (old 99.8 % — nothing), and two minutes of noise prints nothing (old: 20 characters).
#     The full table is tools/rtty-bench/navtex_bench.sh.
#  5. ★ REAL AIR: 250 s of 518 kHz off Stuart's UberSDR (2026-10-05 17:19 UTC, USB 517.0 kHz → tones at 1000 Hz) — the
#     weak Baltic stretch: six messages (IB75, IA93, IA91, IA88, IA78, IA24). The old decoder printed NOTHING (its 1 ms
#     level gate fired 17,611 times); the UberSDR NAVTEX addon (fldigi) logged three of the six.
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
cerf() {  # as cer, the whole reference counted; env passed to the synth
  env "$@" python3 tools/rtty-bench/synth_navtex.py $T/s.wav $SNR $FADE 120 0 0 $SEED >/dev/null
  $T/nav $T/s.wav CCIR476 > $T/last.txt 2>/dev/null
  SCORE_FULL=1 python3 tools/rtty-bench/score.py $T/last.txt $T/s.wav.txt | sed 's/^CER \([0-9.]*\).*/\1/'
}
for SEED in 0 1; do
  SNR=-3 FADE=0; c=$(cerf X=0)
  awk "BEGIN{exit !($c <= 3)}"; ok $? "-3 dB SNR, seed $SEED: CER $c % (≤ 3; old 21.1 / 17.9 %)"
  SNR=6 FADE=20; c=$(cerf X=0)
  awk "BEGIN{exit !($c <= 15)}"; ok $? "20 dB selective fading at 6 dB, seed $SEED: CER $c % (≤ 15; old 17.4 / 95.2 %)"
done
SEED=0 SNR=6 FADE=0; c=$(cerf JOIN=23)
awk "BEGIN{exit !($c <= 2)}"; ok $? "joins mid-message, no phasing: CER $c % (≤ 2; old 8.2 %)"
SNR=10; c=$(cerf INV=1)
awk "BEGIN{exit !($c <= 1)}"; ok $? "swapped tones, decoder not told: CER $c % (≤ 1; old 99.8 %)"
python3 tools/rtty-bench/synth_navtex.py $T/s.wav -30 0 120 >/dev/null
$T/nav $T/s.wav CCIR476 > $T/last.txt 2>/dev/null
n=$(tr -d '\n' < $T/last.txt | wc -c | tr -d ' ')
[ "$n" -eq 0 ]; ok $? "two minutes of noise: $n characters printed (old decoder: 20)"
R=tools/rtty-bench/data/navtex-518khz-2026-10-05.m4a
if command -v ffmpeg >/dev/null && [ -f $R ]; then
  ffmpeg -loglevel error -y -i $R -ac 1 -ar 48000 -c:a pcm_s16le $T/real.wav
  $T/nav $T/real.wav CCIR476 1000 > $T/real.txt 2>/dev/null
  n=0; for h in IB75 IA93 IA91 IA88 IA78 IA24; do grep -q "ZCZC *$h" $T/real.txt && n=$((n+1)); done
  [ "$n" -ge 6 ]; ok $? "REAL 518 kHz: $n of 6 message headers (old decoder: 0; fldigi: 3)"
  m=0; for l in 'GERMAN NAV WARN 537/26' 'SEATRIALS WITH UNMANNED SURFACE AND UNDERWATER DRONES' '54-30,96N 010-07,65E' \
                'FROM 27 SEP 2201 UTC TO 07 OCT 2159 UTC|FGOM 27 SEP 2201 UTC TO 07 OCT 2159 UTC'; do
    grep -qE "$l" $T/real.txt && m=$((m+1)); done
  [ "$m" -ge 3 ]; ok $? "REAL 518 kHz: $m of 4 IA91 lines word for word"
else
  echo "  --   REAL 518 kHz recording: not run (needs ffmpeg)"
fi
exit $fail
