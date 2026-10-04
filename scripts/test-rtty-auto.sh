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
exit $fail
