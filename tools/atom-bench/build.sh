#!/bin/bash
# Part of tools/atom-bench (docs/ATOM-PERFORMANCE-2026-10-18.md). Runs on the Lenovo from ~/atomwork; see images.sh.
# build.sh NAME PLATFORM(amd64|386) "CFLAGS" "extra cmake args" [targets]
# Builds into ~/atomwork/b/NAME (kept, for PGO and perf symbol lookup). nice 19, -j4, --rm.
set -euo pipefail
n=$1 p=$2 f=$3 x=${4:-} t=${5:-"vibeserver vibeserver-bench gen-iq"}
img=vibeserver-build:bookworm-${p/386/i386}
B=$HOME/atomwork/b/$n; mkdir -p $B
docker run --rm --platform linux/$p --cpus 4 -v $HOME/atom-src:/src:ro -v $HOME/atomwork/tools:/tools:ro \
  -v $HOME/atomwork/wrap:/wrap:ro -v $B:/b -v $HOME/atomwork/pgo:/pgo $img /bin/bash -euo pipefail -c "
  cd /b
  [ -f CMakeCache.txt ] || nice -n 19 cmake -S /wrap -B /b -DCMAKE_BUILD_TYPE=Release -DVIBESERVER_STRICT_RADIOS=OFF \
     -DCMAKE_C_FLAGS='$f' -DCMAKE_CXX_FLAGS='$f' $x > /b/cmake.log
  nice -n 19 cmake --build /b --target $t -j4 > /b/build.log 2>&1 || { tail -30 /b/build.log; exit 1; }
  objdump -d --no-show-raw-insn vs/vibeserver > /tmp/d.txt
  echo \"$n: insns \$(grep -cP '^\\s+[0-9a-f]+:\\t' /tmp/d.txt) sse3 \$(grep -cwE 'haddps|haddpd|hsubps|movddup|movshdup|movsldup|lddqu|addsubps|addsubpd|fisttp' /tmp/d.txt || true) ssse3 \$(grep -cwE 'pshufb|palignr|pmaddubsw|phaddw|phaddd|pabsb|pabsw|pabsd|pmulhrsw|psignb|psignw|psignd' /tmp/d.txt || true) movbe \$(grep -cw movbe /tmp/d.txt || true) x87 \$(grep -cwE 'fld|flds|fldl|fstp|fstps|fstpl|fmul|fmulp|faddp|fxch' /tmp/d.txt || true)\" | tee /b/isa.txt"
