#!/usr/bin/env python3
"""score_ref.py MODE BAND REF_SSE START_UTC DUR OURS.txt... — decodes (ft8_wav's "D" lines, or jt9's via jt9conv.py)
against the UberSDR decoder stream recorded beside the WAV (curl -sN http://<ubersdr>/api/decoder/stream), message for
message, within one slot either way. MODE FT8|FT4; BAND as the stream names it (40m-ft8). A hashed call matches whether
resolved or not; the stream's AP tags (a1, a7, ?) are ignored. 'calls' = distinct senders, 'grid' = those with a
locator. V=1 lists ours-only and missed messages. (2026-10-26, the sensitivity work: docs/v12/FTX-SENSITIVITY.md)"""
import json, sys, re, calendar, time
mode, band, refp, t0, dur = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), float(sys.argv[5])
P = 15.0 if mode == 'FT8' else 7.5
def utc(s): return calendar.timegm(time.strptime(s, '%Y-%m-%dT%H:%M:%SZ'))
def norm(m):
    t = m.split()
    if t and re.match(r'^(a[0-9]+|\?)$', t[-1]): t = t[:-1]
    t = ['<H>' if x.startswith('<') and x.endswith('>') else x for x in t]   # a hashed call, resolved or not
    return ' '.join(t)
GRID = re.compile(r'^[A-R]{2}[0-9]{2}$')
def sender(m):
    t = m.split()
    if not t: return None, None
    if t[0] == 'CQ':
        if len(t) >= 3 and not GRID.match(t[2]) and len(t[1]) <= 4 and len(t) >= 3 and (len(t) == 4 or not GRID.match(t[-1])): t = t[1:]
        c = t[1] if len(t) > 1 else None; g = t[2] if len(t) > 2 and GRID.match(t[2]) else None
        if len(t) > 2 and not GRID.match(t[2]) and len(t) == 3 and not t[2].startswith(('R','-','+')) : pass
        return c, g
    if len(t) >= 2:
        c = t[1]; g = t[2] if len(t) > 2 and GRID.match(t[2]) and t[2] != 'RR73' else None
        return c, g
    return None, None
import math
lo = (math.floor(t0 / P) + 1) * P; hi = t0 + dur - P   # slots fully inside the recording
ref = {}
for line in open(refp):
    if not line.startswith('data: '): continue
    try: j = json.loads(line[6:])
    except Exception: continue
    if j.get('band') != band or j.get('mode') != mode: continue
    t = utc(j['timestamp'])
    s = ((t + 2) // P) * P - P if mode == 'FT8' else round((t - 6.5) / P) * P
    s = int(((t + 2) // P) * P - P) if mode=='FT8' else s
    if not (lo <= s <= hi): continue
    ref.setdefault(norm(j['message']), []).append((s, j))
refset = {(s, m) for m, L in ref.items() for s, _ in L}
apref = {(s, m) for m, L in ref.items() for s, j in L if re.search(r'\sa[0-9]+$', j['message'].strip())}
def calls(pairs):
    cs = set(); gc = set()
    for s, m in pairs:
        c, g = sender(m)
        if c and not c.startswith('<'):
            cs.add(c)
            if g: gc.add(c)
    return cs, gc
rc, rg = calls(refset)
print(f'{mode} {band}: ref {len(refset)} decodes ({len(apref)} AP), {len(rc)} calls, {len(rg)} with grid; slots {int((hi-lo)/P)+1}')
for path in sys.argv[6:]:
    ours = set(); ms = 0; ns = 0; extra = []
    for line in open(path):
        if line.startswith('D '):
            p = line.rstrip('\n').split(' ', 6); s = float(p[1])
            if lo <= s <= hi: ours.add((s, norm(p[6]), p[2], p[3]))
        elif line.startswith('S '):
            f = line.split(); s = float(f[1])
            if lo <= s <= hi: ms += float(f[3]) + float(f[4]) + float(f[5]); ns += 1
    om = {(s, m) for s, m, _, _ in ours}
    both = set(); only = []
    for s, m, pas, snr in ours:
        hit = any((s + d * P, m) in refset for d in (0, -1, 1))
        if hit: both.add((s, m))
        else: only.append((s, m, pas, snr))
    miss = [x for x in refset if not any((x[0] + d * P, x[1]) in om for d in (0, -1, 1))]
    oc, og = calls(om)
    print(f'  {path.split("/")[-1]}: ours {len(om)}  matched {len(both)}  ref-only {len(miss)} (AP {sum(1 for x in miss if x in apref)})  ours-only {len(only)}  '
          f'calls {len(oc)} (grid {len(og)}) vs ref {len(rc)} ({len(rg)}); calls both {len(oc & rc)} ref-only {len(rc - oc)} ours-only {len(oc - rc)}  ms/slot {ms/max(1,ns):.1f}')
    if '-v' in sys.argv[0] or __import__('os').environ.get('V'):
        for x in sorted(only): print('     OURS-ONLY', x)
        snr = {m: j['snr'] for m, L in ref.items() for s, j in L}
        for x in sorted(miss, key=lambda x: snr.get(x[1], 0)): print('     MISS', x, snr.get(x[1]))
