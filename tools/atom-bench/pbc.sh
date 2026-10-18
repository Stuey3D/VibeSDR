#!/bin/bash
# Part of tools/atom-bench (docs/ATOM-PERFORMANCE-2026-10-18.md). Runs on the Lenovo from ~/atomwork; see images.sh.
# pbc.sh SRC TEST [core] — compile one run-tests.sh test from SRC (as run-tests does) and run it pinned
src=$1 t=$2 core=${3:-2} p=${4:-amd64}
docker run --rm --platform linux/$p --cpuset-cpus $core -v $src:/s:ro vibeserver-build:bookworm-${p/386/i386} bash -c "
cp -a /s /v; cd /v; VDSP=android/app/src/main/cpp/vibedsp; KISS=\$VDSP/third_party/kissfft; SRC=vibeserver
eval \"\$(sed -n \"/^flags_for()/,/^}/p;/^deps_for()/,/^}/p\" scripts/run-tests.sh)\"
X=; [ $p = 386 ] && X=\"-march=i686 -msse2 -mfpmath=sse\"
g++ -std=c++17 \$X -I vibeserver -I android/app/src/main/cpp \$(flags_for $t) -o /tmp/t vibeserver/$t.cpp \$(deps_for $t) 2>&1 | tail -3
nice -n 5 /tmp/t"
