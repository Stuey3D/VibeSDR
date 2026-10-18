#!/bin/bash
# Part of tools/atom-bench (docs/ATOM-PERFORMANCE-2026-10-18.md). Runs on the Lenovo from ~/atomwork; see images.sh.
# benchab.sh "build:plat ..." rounds — vibeserver-bench (2 s/scenario, offline, the DAB clip), pinned 1,2 (+5,6 idle siblings? no: 1-2 only)
B="$1"; R=${2:-3}; mkdir -p results
for r in $(seq 1 $R); do for bp in $B; do b=${bp%%:*}; p=${bp##*:}
  docker run --rm --privileged --platform linux/$p --cpuset-cpus 1,2 -e VIBE_BENCH_OFFLINE=1 -v $PWD/b/$b:/b:ro \
    -v $HOME/atom-src/vibeserver/bench-clip/dab-bench-clean.vbu8:/clip.vbu8:ro atom-perf:${p/386/i386} bash -c \
    "perf stat -x, -e instructions -o /tmp/ps nice -n 19 /b/vs/vibeserver-bench 3 /clip.vbu8 2>/dev/null; grep instructions /tmp/ps | cut -d, -f1" > /tmp/bab.$$ 
  ins=$(tail -1 /tmp/bab.$$); json=$(head -n -1 /tmp/bab.$$ | tail -1)
  echo "{\"build\":\"$b\",\"round\":$r,\"instructions\":$ins,\"bench\":$json}" >> results/benchab.txt; echo "$b r$r done"
done; done
