# sbstat.py results/sb-TAG.txt — N450-sandbox medians per build × rate: delivered spectrum fps, audio frames per
# second, server CPU (of one i5 thread, capped by the quota), worst audio gap, server dspCpu.
import json, sys, statistics as st, collections
d = collections.defaultdict(lambda: collections.defaultdict(list))
for line in open(sys.argv[1]):
    line = line.strip()
    if not line.startswith('{'): continue
    try: r = json.loads(line)
    except Exception: continue
    l = r['listener']; k = (r['build'], r['rate'])
    d[k]['fps'].append(l['specFps']); d[k]['aud'].append(l['audioFrames'] / (30 - 2.0) if False else l['audioFrames'])
    d[k]['cpu'].append(r['serverCpuPct']); d[k]['gap'].append(l['audioWorstGapMs'] or 0); d[k]['dsp'].append(l.get('dspCpu') or 0)
print(f"{'build':18} {'rate':>8} {'fps':>6} {'audio frames':>12} {'CPU%':>6} {'worst gap ms':>12} {'dspCpu':>6}  n")
for k in sorted(d):
    m = lambda f: st.median(d[k][f])
    print(f"{k[0]:18} {k[1]:8} {m('fps'):6.2f} {m('aud'):12.0f} {m('cpu'):6.1f} {m('gap'):12.0f} {m('dsp'):6.0f}  {len(d[k]['fps'])}  fps={d[k]['fps']}")
