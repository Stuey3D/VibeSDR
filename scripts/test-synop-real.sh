#!/usr/bin/env bash
# test-synop-real.sh — the REAL DDK2 recording through the server's own RTTY decoder and SYNOP translator (V12, 2026-10-23).
#
# ★★ test-synop.cpp pastes this recording's decoded text verbatim; this proves the paste is still what the decoder prints
#    (a decoder change that alters it must update the test, not silently drift), and that the translator claims NOTHING
#    from it as heard — no AAXX, a lost space in the first group, no '='.
#    Recording: tools/rtty-bench/data/ddk-4583khz-2026-10-22.m4a — DDK2 4583 kHz on the Lenovo, 2026-10-22 22:36 UTC,
#    21 s, ALAC (lossless: AAC changed three characters of the decode).
# ★ Exit 3 = not run (no afconvert/ffmpeg to read the m4a). SILENT: nothing is played.
set -uo pipefail
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/vibeserver-tests"; mkdir -p "$OUT"
C=android/app/src/main/cpp
IN=tools/rtty-bench/data/ddk-4583khz-2026-10-22.m4a
WAV="$OUT/ddk-4583.wav"
if command -v afconvert >/dev/null; then afconvert -c 1 -f WAVE -d LEI16@48000 "$IN" "$WAV" || exit 1
elif command -v ffmpeg >/dev/null; then ffmpeg -loglevel error -y -i "$IN" -ac 1 -ar 48000 -c:a pcm_s16le "$WAV" || exit 1
else echo "   not run — needs afconvert (macOS) or ffmpeg to read the m4a"; exit 3; fi
c++ -std=c++17 -O2 -I$C -o "$OUT/rtty_wav" tools/rtty-bench/rtty_wav.cpp $C/decoders/fsk_decoder.cpp $C/decoders/rtty_auto.cpp || exit 1
got="$(SYNOP=1 "$OUT/rtty_wav" "$WAV" 1000 450 50 5N1.5 1 2>"$OUT/ddk-4583.err")"
want=$'XX ZPQRT OY RPPYU WWPM\n0015//;\n62170149514 120 46/97  1015 20125 4005 222\n006 10 70008 90710 9112'
pass=0; fail=0
if [ "$got" = "$want" ]; then echo "   ok   the decoder still prints the text test-synop.cpp pastes"; pass=$((pass+1))
else echo "   FAIL the decoder's text changed — update test-synop.cpp's clip:"; printf '%s\n' "$got" | sed 's/^/        /'; fail=$((fail+1)); fi
if grep -q "SYNOP/SHIP: 0 report(s)" "$OUT/ddk-4583.err"; then echo "   ok   nothing claimed from the clip as heard (no AAXX, no '=')"; pass=$((pass+1))
else echo "   FAIL the translator claimed a report from the clip:"; cat "$OUT/ddk-4583.err"; fail=$((fail+1)); fi
echo "   $pass ok, $fail failed"
[ $fail -eq 0 ]
