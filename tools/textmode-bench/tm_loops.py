#!/usr/bin/env python3
# tools/textmode-bench/tm_loops.py — the web test's joins, offline (DECODERS-WAVE2.md sections 12 and 20). The web clip's
# audio (tm_clip with VIBE_TM_DUMP=clip.f32: 20 s at 48 kHz, OLIVIA 16/500 at 1500 Hz + MFSK16 at 800 Hz) is looped three
# times from N start offsets, with no noise (seed 0) or white noise of σ 0.02·seed, and run through tm_file's one-key AUTO.
# Each run is checked as test-web-textmodes checks it: nothing printed that was not sent, and no "[MODE · Hz]" line that
# names anything but the two real signals.
#   tm_loops.py TM_FILE CLIP.f32 N SEEDS(comma-separated, e.g. 0,1,2) [MODE=auto]
import os, re, subprocess, sys
import numpy as np

tm, clip, N = sys.argv[1], sys.argv[2], int(sys.argv[3])
seeds = [int(s) for s in sys.argv[4].split(',')]
mode = sys.argv[5] if len(sys.argv) > 5 else 'auto'
d = np.fromfile(clip, dtype=np.float32)
OM, MM = 'CQ DE G4XYZ OLIVIA K', 'CQ CQ DE G4XYZ MFSK TEST PSE K'
NAMES = re.compile(r'^(OLIVIA 16/500 · 1[45]\d\d|MFSK16 · [78]\d\d) Hz$')
tmp = '%s.loop.%d' % (clip, os.getpid())
runs = wrong = badname = whole = wm = wo = 0
try:
    for seed in seeds:
        rng = np.random.default_rng(seed)
        for k in range(N):
            off = int(k * len(d) / N)
            x = np.concatenate([d[off:], d, d, d[:off]]).astype(np.float32)
            if seed: x = x + rng.normal(0, 0.02 * seed, len(x)).astype(np.float32)
            x.tofile(tmp)
            out = subprocess.run([tm, 'mfsk', mode, tmp, '48000'], capture_output=True, text=True, timeout=600).stdout
            segs = [s.replace('\n', '') for s in re.split(r'\n?\[[^\]]*\]\n?', out)]
            bad = [s for s in segs if s.strip() and (OM * 4).find(s) < 0 and (MM * 4).find(s) < 0]
            names = [h for h in re.findall(r'\[([^\]]*)\]', out) if not NAMES.match(h)]
            runs += 1; wrong += bool(bad); badname += bool(names)
            whole += (OM in out or MM in out); wm += out.count(MM); wo += out.count(OM)
            if bad or names: print('seed', seed, 'offset', k, bad, names, flush=True)
finally:
    if os.path.exists(tmp): os.remove(tmp)
print('runs', runs, 'wrong', wrong, 'misnamed', badname, 'whole', whole, 'MFSK16 overs', wm, 'OLIVIA overs', wo)
