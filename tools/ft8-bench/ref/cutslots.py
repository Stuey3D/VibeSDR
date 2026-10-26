#!/usr/bin/env python3
# cutslots.py WAV START_UTC PERIOD OUTDIR — one WAV per whole T/R slot, named yymmdd_hhmmss.wav (jt9's convention)
import sys, wave, math, time, os
src, t0, P, out = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
os.makedirs(out, exist_ok=True)
w = wave.open(src); fs = w.getframerate(); data = w.readframes(w.getnframes()); n = len(data) // 2
S = math.ceil(t0 / P) * P; k = 0
while S + P <= t0 + n / fs:
    i0 = round((S - t0) * fs); seg = data[2 * i0: 2 * (i0 + round(P * fs))]
    name = time.strftime('%y%m%d_%H%M%S', time.gmtime(S))
    o = wave.open(os.path.join(out, name + '.wav'), 'wb'); o.setnchannels(1); o.setsampwidth(2); o.setframerate(fs); o.writeframes(seg); o.close()
    S += P; k += 1
print(k, 'slots')
