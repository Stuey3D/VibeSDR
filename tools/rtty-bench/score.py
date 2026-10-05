#!/usr/bin/env python3
"""score.py — how much of the known text a decode got right (synthetic signals from synth_rtty.py).
   score.py decoded.txt reference.txt  →  % of reference characters recovered in order, and garbage chars."""
import sys, difflib
d = open(sys.argv[1]).read().replace('\r', ''); r = open(sys.argv[2]).read().replace('\r', '')
import os
# ★ SCORE_FULL=1 (2026-10-05): score against the whole reference. By default it is trimmed to the decode's length + 200,
#   which suits a short test run but lets a decoder that prints LESS off the missing tail.
if os.environ.get('SCORE_FULL') != '1':
    r = r[:max(len(d) + 200, 1)] if len(d) < len(r) else r
sm = difflib.SequenceMatcher(None, r, d, autojunk=False)
ok = sum(b.size for b in sm.get_matching_blocks())
# character error rate: edit operations to turn the reference (as long as the decode) into the decode
import itertools
ref = r[:len(d)] if len(d) else r
ops = sum(max(i2 - i1, j2 - j1) for tag, i1, i2, j1, j2 in sm.get_opcodes() if tag != 'equal')
print(f'CER {100 * ops / max(1, len(r)):.1f} %  ', end='')
print(f'recovered {100 * ok / max(1, min(len(r), len(d) + 50)):.1f} %   garbage {len(d) - ok} of {len(d)} decoded')
