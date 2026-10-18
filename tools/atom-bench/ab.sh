#!/bin/bash
# Part of tools/atom-bench (docs/ATOM-PERFORMANCE-2026-10-18.md). Runs on the Lenovo from ~/atomwork; see images.sh.
# ab.sh "build:plat ..." "rates" rounds cpus tag   → results/ab-<tag>.txt (one JSON per run + perf stat)
B="$1"; RATES="$2"; R=${3:-3}; Q=${4:-2}; T=${5:-x}
mkdir -p results; out=results/ab-$T.txt
for r in $(seq 1 $R); do for bp in $B; do for rate in $RATES; do
  b=${bp%%:*}; p=${bp##*:}
  line=$(./sandbox.sh $b $p $rate $Q 12 25 20 stat 2>/tmp/ab.err)
  ins=$(grep instructions /tmp/ab.err | awk "{gsub(\",\",\"\",\$1); print \$1}")
  cyc=$(grep -w cycles /tmp/ab.err | awk "{gsub(\",\",\"\",\$1); print \$1}")
  echo "{\"round\":$r,\"instructions\":${ins:-0},\"cycles\":${cyc:-0},\"run\":$line}" >> $out
  echo "r$r $b $rate ins=$ins $(echo $line | python3 summ.py | cut -c1-140)"
done; done; done
