#!/bin/bash
# test-rtty-auto.sh — RTTY decoding, measured: the decoder (decoders/fsk_decoder.cpp) and RTTY AUTO (decoders/rtty_auto.cpp)
# on synthetic signals from tools/rtty-bench/synth_rtty.py.
#  1. AUTO finds a ham signal (45.45 baud, 170 Hz, normal) and DWD (50 baud, 450 Hz, reverse) on its own, and decodes them.
#  2. Selective fading (15 dB SNR, 20 dB independent tone fades) decodes with no garbage — the old decoder printed 54
#     garbage characters there (2026-10-04, tools/rtty-bench).
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
C=android/app/src/main/cpp
c++ -std=c++17 -O2 -I$C -o $T/rtty tools/rtty-bench/rtty_wav.cpp $C/decoders/fsk_decoder.cpp $C/decoders/rtty_auto.cpp || { echo "  ✗ build"; exit 1; }
fail=0
ok() { if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fail=1; fi; }
BAUD=45.45 CF=1500 SHIFT=170 python3 tools/rtty-bench/synth_rtty.py $T/ham.wav 15 0 40 0 >/dev/null
$T/rtty $T/ham.wav auto > $T/ham.txt 2> $T/ham.err
grep -q "45.45 baud, 170 Hz shift\]" $T/ham.err; ok $? "AUTO: ham signal → 45.45 baud, 170 Hz, normal ($(sed 's/.*chose: //' $T/ham.err))"
grep -q "CQ CQ CQ DE DDK2 DDH7 DDK9" $T/ham.txt; ok $? "...and decodes it"
python3 tools/rtty-bench/synth_rtty.py $T/dwd.wav 15 0 40 1 >/dev/null
$T/rtty $T/dwd.wav auto > $T/dwd.txt 2> $T/dwd.err
grep -q "50 baud, 450 Hz shift, reverse\]" $T/dwd.err; ok $? "AUTO: DWD signal → 50 baud, 450 Hz, reverse ($(sed 's/.*chose: //' $T/dwd.err))"
grep -q "FREQUENCIES 4583 KHZ 7646 KHZ" $T/dwd.txt; ok $? "...and decodes it"
python3 tools/rtty-bench/synth_rtty.py $T/fade.wav 15 20 90 1 1 >/dev/null
$T/rtty $T/fade.wav 1000 450 50 5N1.5 1 > $T/fade.txt 2>/dev/null
g=$(python3 tools/rtty-bench/score.py $T/fade.txt $T/fade.wav.txt | sed 's/.*garbage \([0-9]*\) of.*/\1/')
[ "$g" -le 10 ]; ok $? "selective fading (15 dB SNR, 20 dB tone fades): $g garbage characters (old decoder: 54)"
# 3. ★ REAL AIR: Stuart's recording of DWD on 4582 kHz USB (Airspy HF+, 2026-10-04 ~20:26 BST) — the one that printed
#    "CQ CQ CQNDZPXX0XXV…" on the old decoder. Old: 1 clean frequency line, 3 runs of 10+ garbage characters.
R=tools/rtty-bench/data/dwd-4582khz-2026-10-04.m4a
if command -v ffmpeg >/dev/null && [ -f $R ]; then
  ffmpeg -loglevel error -y -i $R -ac 1 -ar 48000 -c:a pcm_s16le $T/real.wav
  $T/rtty $T/real.wav auto > $T/real.txt 2> $T/real.err
  grep -q "50 baud, 450 Hz shift, reverse\]" $T/real.err; ok $? "REAL DWD: AUTO → 50 baud, 450 Hz, reverse ($(sed 's/.*chose: //' $T/real.err))"
  n=$(grep -c 'FREQUENCIES   4583 KHZ   7646 KHZ   10100.8 KHZ' $T/real.txt)
  [ "$n" -ge 3 ]; ok $? "REAL DWD: $n complete frequency lines (old decoder: 1)"
  j=$(grep -oE '[A-QS-XZ0-9]{10,}' $T/real.txt | wc -l | tr -d ' ')
  [ "$j" -eq 0 ]; ok $? "REAL DWD: $j runs of 10+ garbage characters (old decoder: 3)"
else
  echo "  --   REAL DWD recording: not run (needs ffmpeg)"
fi
# 4. ★ REAL AIR: PBB Den Helder (Royal Netherlands Navy) on 2474 kHz, 75 Bd, 850 Hz, reverse, ONE stop bit, idle gaps between
#    characters (Stuart's recording, 2026-10-04 ~21:56 UTC). Nothing decoded it before: only 5N1.5 waited for the start bit,
#    the 1-stop path read the start bit as data, and AUTO never tried 1 stop.
R=tools/rtty-bench/data/pbb-2474khz-2026-10-04.m4a
if command -v ffmpeg >/dev/null && [ -f $R ]; then
  ffmpeg -loglevel error -y -i $R -ac 1 -ar 48000 -c:a pcm_s16le $T/pbb.wav
  $T/rtty $T/pbb.wav auto > $T/pbb.txt 2> $T/pbb.err
  grep -q "75 baud, 850 Hz shift, reverse, 1 stop bit\]" $T/pbb.err; ok $? "REAL PBB: AUTO → 75 baud, 850 Hz, reverse, 1 stop ($(sed 's/.*chose: //' $T/pbb.err | tr -d '\n'))"
  n=$(grep -c '02A   04B   06A   08B   12X   17B   22X   26Y' $T/pbb.txt)
  [ "$n" -ge 3 ]; ok $? "REAL PBB: $n complete '02A … 26Y' lines"
  grep -q 'PBB' $T/pbb.txt; ok $? "REAL PBB: the callsign PBB decodes"
else
  echo "  --   REAL PBB recording: not run (needs ffmpeg)"
fi
exit $fail
