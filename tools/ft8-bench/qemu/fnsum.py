#!/usr/bin/env python3
# fnsum.py TAG [N] — instructions per function from TAG.insn + TAG.syms
import sys, bisect, collections
T = sys.argv[1]; N = int(sys.argv[2]) if len(sys.argv) > 2 else 25
import os; Q = os.path.expanduser(os.environ.get('FT8Q', '~/Library/Caches/ft8perf')) + '/q/'
syms = []
for l in open(Q + T + '.syms'):
    p = l.split(None, 2)
    if len(p) == 3: syms.append((int(p[0], 16), p[2].strip()))
syms.sort(); addrs = [a for a, _ in syms]
tot = collections.Counter(); total = 0
for l in open(Q + T + '.insn'):
    a, n = l.split()
    if a == 'TOTAL': total = int(n); continue
    i = bisect.bisect_right(addrs, int(a, 16)) - 1
    tot[syms[i][1] if i >= 0 else '?'] += int(n)
print(f'{T}: total {total/1e6:.1f} M instructions')
for name, n in tot.most_common(N):
    print(f'  {n/1e6:9.2f} M  {100*n/total:5.1f} %  {name[:90]}')
