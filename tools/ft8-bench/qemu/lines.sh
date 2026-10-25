#!/bin/bash
# lines.sh TAG BIN FUNCSUBSTR — instructions by source line for TBs inside one function
T=$1; B=$2; F=$3
read lo sz < <(arm-linux-gnueabihf-nm -n -C -S $B | grep -F -- "$F" | grep -v "lambda\|std::" | head -1 | awk '{print $1, $2}')
python3 - "$T" "$B" "$lo" "$sz" <<'PY'
import sys, subprocess, collections
T, B, lo, sz = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16)
rows = []
for l in open(f'/w/q/{T}.insn'):
    a, n = l.split()
    if a != 'TOTAL' and lo <= int(a, 16) < lo + sz: rows.append((a, int(n)))
out = subprocess.run(['arm-linux-gnueabihf-addr2line', '-e', B] + ['0x' + a for a, _ in rows], capture_output=True, text=True).stdout.splitlines()
c = collections.Counter(); t = 0
for (a, n), ln in zip(rows, out):
    k = ln.split()[0].rsplit(':', 1)[-1]; c[k] += n; t += n
for k, v in c.most_common(25): print(f'{v/1e6:10.1f} M {100*v/t:5.1f}%  line {k}')
print(f'{t/1e6:10.1f} M total')
PY
