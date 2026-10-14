#!/bin/sh
S=/private/tmp/claude-501/-Users-stuey3d-VibeSDR/78bb0ae4-b315-479c-92c0-2f89bd725c96/scratchpad
for f in 96.1M 90.1M 105.4M 106.0M; do
  node scripts/agc-sweep.mjs ws://192.168.86.111:48000 --freq $f --dwell 4 > $S/xc_$f.log 2>&1
  sleep 4
done
echo ALLDONE
