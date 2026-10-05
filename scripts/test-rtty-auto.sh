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
# 2b. ★ AFC (2026-10-05): a DRIFTING station stays decoded. 120 s from synth_rtty.py with DRIFT (Hz/s). Without AFC:
#     DWD 2 Hz/s at 6 dB 3 lines (504 of 717 chars), ham 1 Hz/s at 6 dB 2 lines (369 of 655); AUTO on DWD 2 Hz/s at 10 dB
#     re-searched 9 times and printed 0 complete lines (802 garbage of 1152) — every 25 Hz of drift read as a retune.
DRIFT=2 python3 tools/rtty-bench/synth_rtty.py $T/drift.wav 6 0 120 1 >/dev/null
$T/rtty $T/drift.wav 1000 450 50 5N1.5 1 > $T/drift.txt 2> $T/drift.err
n=$(grep -c 'FREQUENCIES 4583 KHZ 7646 KHZ 10100.8 KHZ' $T/drift.txt)
[ "$n" -ge 4 ]; ok $? "AFC: DWD drifting 2 Hz/s, 6 dB: $n complete frequency lines (no AFC: 3) — $(grep -o 'AFC.*' $T/drift.err)"
DRIFT=1 BAUD=45.45 CF=1500 SHIFT=170 python3 tools/rtty-bench/synth_rtty.py $T/hdrift.wav 6 0 120 0 >/dev/null
$T/rtty $T/hdrift.wav 1500 170 45.45 5N1.5 0 > $T/hdrift.txt 2> $T/hdrift.err
n=$(grep -c 'CQ CQ CQ DE DDK2 DDH7 DDK9' $T/hdrift.txt)
[ "$n" -ge 4 ]; ok $? "AFC: ham 170 Hz drifting 1 Hz/s, 6 dB: $n complete CQ lines (no AFC: 2) — $(grep -o 'AFC.*' $T/hdrift.err)"
DRIFT=2 python3 tools/rtty-bench/synth_rtty.py $T/adrift.wav 10 0 120 1 >/dev/null
$T/rtty $T/adrift.wav auto > $T/adrift.txt 2>/dev/null
n=$(grep -c 'FREQUENCIES 4583 KHZ 7646 KHZ 10100.8 KHZ' $T/adrift.txt); s=$(grep -c 'RTTY auto' $T/adrift.txt)
[ "$n" -ge 4 ] && [ "$s" -eq 1 ]; ok $? "AFC: AUTO on DWD drifting 2 Hz/s: $n complete lines, $s search(es) (no AFC: 0 lines, 9 searches)"
python3 tools/rtty-bench/synth_rtty.py $T/still.wav 6 0 60 1 >/dev/null
RTTY_AFC=0 $T/rtty $T/still.wav 1000 450 50 5N1.5 1 > $T/still0.txt 2>/dev/null
$T/rtty $T/still.wav 1000 450 50 5N1.5 1 > $T/still1.txt 2>/dev/null
cmp -s $T/still0.txt $T/still1.txt; ok $? "AFC: a steady signal decodes byte-identically with AFC on and off"
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
  RTTY_AFC=0 $T/rtty $T/real.wav 1000 450 50 5N1.5 1 > $T/real0.txt 2>/dev/null
  $T/rtty $T/real.wav 1000 450 50 5N1.5 1 > $T/real1.txt 2>/dev/null
  cmp -s $T/real0.txt $T/real1.txt; ok $? "REAL DWD: AFC on decodes byte-identically to AFC off (the station sits on tune)"
else
  echo "  --   REAL DWD recording: not run (needs ffmpeg)"
fi
# 3b. ASCII (the full RTTY spec, 2026-10-04): 7E1 at 110 Bd and 8N2 at 300 Bd decode; a 7E1 signal read as 7O1 does not.
python3 tools/rtty-bench/synth_ascii.py $T/a7e1.wav 7E1 110 >/dev/null
RTTY_ENC=ASCII $T/rtty $T/a7e1.wav 1500 170 110 7E1 0 2>/dev/null > $T/a7e1.txt
grep -q "QUICK BROWN FOX 0123456789" $T/a7e1.txt; ok $? "ASCII 7E1 110 Bd decodes"
python3 tools/rtty-bench/synth_ascii.py $T/a8n2.wav 8N2 300 >/dev/null
RTTY_ENC=ASCII $T/rtty $T/a8n2.wav 1500 170 300 8N2 0 2>/dev/null > $T/a8n2.txt
grep -q "BROWN FOX" $T/a8n2.txt; ok $? "ASCII 8N2 300 Bd decodes"
RTTY_ENC=ASCII $T/rtty $T/a7e1.wav 1500 170 110 7O1 0 2>/dev/null > $T/a7o1.txt
! grep -q "BROWN" $T/a7o1.txt; ok $? "...and the wrong parity (7O1) does not"

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
