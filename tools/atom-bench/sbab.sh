#!/bin/bash
# Part of tools/atom-bench (docs/ATOM-PERFORMANCE-2026-10-18.md). Runs on the Lenovo from ~/atomwork; see images.sh.
# sbab.sh "build:plat ..." "rates" rounds quota tag — the N450 sandbox, interleaved
B="$1"; RATES="$2"; R=$3; Q=$4; T=$5; mkdir -p results
for r in $(seq 1 $R); do for bp in $B; do for rate in $RATES; do b=${bp%%:*}; p=${bp##*:}
  ./sandbox.sh $b $p $rate $Q 12 30 20 >> results/sb-$T.txt 2>/dev/null; echo "r$r $b $rate"; done; done; done
echo SBDONE
