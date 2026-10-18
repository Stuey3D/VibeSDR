# abstat.py results/ab-TAG.txt [base1:cand1 ...] — medians per build×rate; instructions per second of signal
import json, sys, statistics as st, collections
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
d = collections.defaultdict(lambda: collections.defaultdict(list))
for r in rows:
    run = r['run']; k = (run['build'], run['rate'])
    d[k]['ins'].append(r['instructions'] / 23.0 / 1e6)      # perf stat window = MEASURE-2 = 23 s
    d[k]['cyc'].append(r['cycles'] / 23.0 / 1e6)
    d[k]['cpu'].append(run['serverCpuPct'])
    for t, v in run['threads'].items(): d[k]['t:' + t].append(v)
def med(k, f): xs = d[k].get(f, []); return st.median(xs) if xs else 0.0
print(f"{'build':16} {'rate':>8} {'Minstr/s':>9} {'Mcyc/s':>8} {'CPU%':>6}  dsp/demod/spec")
for k in sorted(d):
    print(f"{k[0]:16} {k[1]:8} {med(k,'ins'):9.0f} {med(k,'cyc'):8.0f} {med(k,'cpu'):6.1f}  "
          f"{med(k,'t:vibe-dsp'):.1f}/{med(k,'t:vibe-demod'):.1f}/{med(k,'t:vibe-spec'):.1f}   n={len(d[k]['ins'])} ins-spread={min(d[k]['ins']):.0f}-{max(d[k]['ins']):.0f}")
for pair in sys.argv[2:]:
    a, b = pair.split(':')
    for rate in sorted({k[1] for k in d}):
        ka, kb = (a, rate), (b, rate)
        if ka in d and kb in d:
            print(f"{b} vs {a} @ {rate}: instructions {100*(med(kb,'ins')/med(ka,'ins')-1):+.1f} %, cycles {100*(med(kb,'cyc')/med(ka,'cyc')-1):+.1f} %, CPU {100*(med(kb,'cpu')/med(ka,'cpu')-1):+.1f} %")
