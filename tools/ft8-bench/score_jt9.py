#!/usr/bin/env python3
"""score_jt9.py OURS.txt [OURS2.txt ...] — ours (ft8_wav output) vs UberSDR's jt9 (uber_sse.txt), slot by slot.
Only slots fully inside the reference capture window are scored."""
import json, sys, re, calendar, time, statistics as st
import os
REF = os.environ.get('FT8_REF', 'uber_sse.txt')   # curl -sN http://<ubersdr>/api/decoder/stream > uber_sse.txt while recording
def utc(s): return calendar.timegm(time.strptime(s, '%Y-%m-%dT%H:%M:%SZ'))
ref = {}
first = last = None
for line in open(REF):
    if not line.startswith('data: '): continue
    try: j = json.loads(line[6:])
    except Exception: continue
    if 'timestamp' not in j: continue
    t = utc(j['timestamp'])
    first = t if first is None else min(first, t); last = t if last is None else max(last, t)
    if j.get('band') != '40m-ft8': continue
    slot = (t - 1) // 15 * 15 - 15 + 15 * (1 if (t % 15) not in (0,) and (t % 15) < 12 else 0)
    # emitted at the slot END (:00/:15/...), occasionally a few s early; the slot is the one that ended last
    slot = ((t + 3) // 15) * 15 - 15
    ref.setdefault(slot, {})[j['message'].strip()] = j
lo = (first // 15 + 2) * 15; hi = (last // 15 - 1) * 15
CALL = re.compile(r'^(?:[A-Z0-9]{1,3}/)?[A-Z0-9]{0,3}[0-9][A-Z0-9]{0,3}[A-Z](?:/[A-Z0-9]{1,4})?$|^<[^>]+>$')
def plausible(msg):
    p = msg.split()
    if not p or len(p) > 4: return False
    if p[0] in ('CQ',): p = p[1:] if len(p) < 4 else p[2:]
    calls = [x for x in p[:2]]
    return all(CALL.match(c) or c in ('CQ','QRZ','DE') for c in calls)
TOK = set()
for sl in ref.values():
    for m in sl: TOK.update(m.replace('<','').replace('>','').split())
GRID = re.compile(r'^[A-R]{2}[0-9]{2}$|^R?[-+][0-9]{2}$|^(RRR|RR73|73|CQ|DE|QRZ)$')
def klass(m):
    if m == '?': return 'unparsed'
    toks = [t for t in m.replace('<','').replace('>','').split() if not GRID.match(t) and t != '...']
    if not toks: return 'known'
    return 'known' if all(t in TOK for t in toks) else 'unknown'
for path in sys.argv[1:]:
    ours = {}; ms = []; passes = []
    for line in open(path):
        if line.startswith('D '):
            _, s, pas, snr, dt, hz, msg = line.rstrip('\n').split(' ', 6)
            s = int(float(s))
            if lo <= s <= hi: ours.setdefault(s, {})[msg.strip()] = (pas, int(snr), float(dt), float(hz))
        elif line.startswith('S '):
            f = line.split(); s = int(float(f[1]))
            if lo <= s <= hi: ms.append(float(f[3]) + float(f[4]) + float(f[5])); passes.append(int(f[2]))
    nref = sum(len(v) for s, v in ref.items() if lo <= s <= hi)
    nours = sum(len(v) for v in ours.values())
    both = only_ours = only_ref = 0; bad = []; extra_ok = []; snrd = []; bypass = {}
    for s in range(lo, hi + 1, 15):
        R = ref.get(s, {}); O = ours.get(s, {})
        for m, v in O.items():
            if m in R:
                both += 1; snrd.append(v[1] - R[m]['snr']); bypass[v[0]] = bypass.get(v[0], 0) + 1
            elif m in ref.get(s - 15, {}) or m in ref.get(s + 15, {}):
                both += 1; bypass[v[0]] = bypass.get(v[0], 0) + 1   # slot attribution off by one
            else:
                only_ours += 1
                (extra_ok if klass(m) == 'known' else bad).append((s, m, v, klass(m)))
        only_ref += sum(1 for m in R if m not in O)
    nslots = (hi - lo) // 15 + 1
    print(f"{path.split('/')[-1]}: slots {nslots}  ref {nref} ({nref/nslots:.1f}/slot)  ours {nours} ({nours/nslots:.1f}/slot)  "
          f"matched {both} ({100*both/max(1,nref):.1f}% of ref)  ref-only {only_ref}  ours-only {only_ours} "
          f"[stations heard by jt9 this hour {len(extra_ok)}, unparsed {sum(1 for b in bad if b[3]=='unparsed')}, UNKNOWN calls {sum(1 for b in bad if b[3]=='unknown')}]  by pass {dict(sorted(bypass.items()))}")
    if ms: print(f"   cpu ms/slot mean {st.mean(ms):.1f} max {max(ms):.1f}  passes mean {st.mean(passes):.2f}")
    if snrd: print(f"   snr ours-ref median {st.median(snrd):+.1f}  IQR {st.quantiles(snrd,n=4)[0]:+.1f}..{st.quantiles(snrd,n=4)[2]:+.1f}")
    if '-v' in sys.argv[0:1] or len(sys.argv) == 2:
        for b in bad[:40]:
            if b[3]=='unknown': print('   UNKNOWN', b)
        pass
