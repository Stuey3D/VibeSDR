#!/usr/bin/env python3
"""dab-weak-ladder.py — step white noise into real DAB captures until the receiver fails, and print the table.

  scripts/dab-weak-ladder.py BIN [--secs S] [--jobs N] [--extra "ARGS"] [--json OUT] CLIP:SID[:SKIP] ...

For each clip it runs dab-weak (BIN) with no added noise and then with noise from --hi down to --lo dB below the clip's
total power (default 30 → 6, 1 dB steps), several seeds, and prints per level: the measured in-band SNR of the input,
MER, pre-Viterbi BER, FIB rate, super frames ok/tried (or MP2 frames), lock/label/audio times. The EDGE is the lowest
in-band SNR at which every seed still gets audio (>= 50 % of super frames) — the number a change must not lose.
★ --extra passes options to dab-weak for every run (e.g. "--wide 8000000 --offset 2500000" for the DDC path), so the
  same ladder measures native against wide on identical noise (same seeds).
★ Real captures are other people's programmes: they stay in ~/VibeSDR-test-audio/dab, never in git."""
import argparse, json, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ap = argparse.ArgumentParser()
ap.add_argument('bin'); ap.add_argument('clips', nargs='+')
ap.add_argument('--secs', type=float, default=40); ap.add_argument('--jobs', type=int, default=8)
ap.add_argument('--hi', type=float, default=30); ap.add_argument('--lo', type=float, default=6); ap.add_argument('--step', type=float, default=1)
ap.add_argument('--seeds', type=int, default=2); ap.add_argument('--extra', default=''); ap.add_argument('--json', default='')
a = ap.parse_args()

def run(args):
    p = subprocess.run([a.bin] + args, capture_output=True, text=True, timeout=3600)
    for l in p.stdout.splitlines():
        if l.startswith('{'):
            try: return json.loads(l)
            except Exception: pass
    return None

levels = [None]
x = a.hi
while x >= a.lo - 1e-9: levels.append(round(x, 2)); x -= a.step
jobs = []
for c in a.clips:
    parts = c.split(':'); path, sid = parts[0], parts[1] if len(parts) > 1 else ''
    skip = parts[2] if len(parts) > 2 else '0'
    for lv in levels:
        for seed in range(1, (1 if lv is None else a.seeds) + 1):
            args = [path, '--secs', str(a.secs), '--skip', skip, '--seed', str(seed)] + (['--sid', sid] if sid else []) + shlex.split(a.extra)
            if lv is not None: args += ['--noise', str(lv)]
            jobs.append((path, lv, seed, args))
with ThreadPoolExecutor(max_workers=a.jobs) as ex:
    res = list(ex.map(lambda j: (j, run(j[3])), jobs))
out = []
for c in a.clips:
    path = c.split(':')[0]
    print(f"\n== {path.split('/')[-1]}  {a.extra}")
    print(f"{'noise':>6} {'snrIn':>6} {'MER':>5} {'BER%':>5} {'FIB':>5} {'sf ok/tried':>12} {'mp2 ok/in':>10} {'lock':>5} {'label':>6} {'audio':>6}  seeds")
    edge = None
    for lv in levels:
        rows = [r for (j, r) in res if j[0] == path and j[1] == lv and r]
        if not rows: continue
        def m(k): v = [r[k] for r in rows if r.get(k) is not None and r[k] == r[k]]; return sum(v) / len(v) if v else float('nan')
        sfok = sum(r['sfOk'] for r in rows); sft = sum(r['sfTried'] for r in rows)
        mp2o = sum(r['mp2Out'] for r in rows); mp2b = sum(r['mp2Bad'] for r in rows)
        ok = all((r['sfTried'] and r['sfOk'] >= 0.5 * r['sfTried']) or (r['mp2Out'] and r['mp2Bad'] < 0.5 * r['mp2Out']) for r in rows)
        times = lambda k: max(r[k] for r in rows) if all(r[k] >= 0 for r in rows) else -1
        print(f"{'none' if lv is None else lv:>6} {m('snrIn'):6.2f} {m('merAvg'):5.2f} {100*m('mscBer'):5.1f} {m('fibRate'):5.2f} {sfok:>5}/{sft:<6} {mp2o-mp2b:>4}/{mp2o:<5} {times('tLock'):5.2f} {times('tLabel'):6.2f} {times('tAudio'):6.2f}  {'OK' if ok else '--'}")
        if ok and lv is not None: edge = (lv, m('snrIn'))
        out.append({'clip': path, 'noise': lv, 'rows': rows})
    if edge: print(f"   EDGE: audio on every seed down to noise {edge[0]} dB (in-band SNR {edge[1]:.2f} dB)")
if a.json:
    with open(a.json, 'w') as f: json.dump(out, f)
