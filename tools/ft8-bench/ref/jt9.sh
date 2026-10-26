#!/bin/bash
# jt9.sh WAV START_UTC PERIOD(15|7.5) DEPTH OUT.txt — the reference decoder (Debian bookworm's packaged WSJT-X jt9,
# run as a black box, never read) over a recording cut into its T/R slots: decodes in ft8_wav's "D" format for
# score_ref.py, and its CPU (one core, user seconds) on stderr. Work dir under $HOME so Colima can mount it.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
W=$1; T0=$2; P=$3; DEP=$4; OUT=$5
R="${FTXREF:-$HOME/Library/Caches/ftxref}"; name=$(basename "$W" .wav); mkdir -p "$R"
docker image inspect ftxref-jt9 >/dev/null 2>&1 || nice -n 10 docker build -q -t ftxref-jt9 -f "$here/Dockerfile.jt9" "$here"
python3 "$here/cutslots.py" "$W" "$T0" "$P" "$R/$name"
flag=-8; per=15; [ "$P" = "7.5" ] && { flag=-5; per=7; }
n=$(ls "$R/$name"/*.wav | wc -l | tr -d ' ')
docker run --rm --cpus 1 -v "$R:/w" -w "/w/$name" ftxref-jt9 nice -n 10 bash -c \
  "/usr/bin/time -f 'jt9 -d $DEP: CPU user %U s for $n slots' jt9 $flag -p $per -d $DEP -c STUEY3D -G IO92 *.wav > /w/$name.jt9"
day=$(python3 -c "import time;print(time.strftime('%Y-%m-%d', time.gmtime($T0)))")
python3 "$here/jt9conv.py" "$R/$name.jt9" "$day" "$P" > "$OUT"
rm -rf "$R/$name" "$R/$name.jt9"
