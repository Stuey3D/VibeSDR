#!/usr/bin/env python3
# jt9conv.py JT9OUT YYYY-MM-DD [PERIOD] — jt9's lines as ft8_wav "D" lines (slot UTC from hhmmss), for score.py
import sys, calendar, time, re
day = sys.argv[2]
for l in open(sys.argv[1]):
    m = re.match(r'^(\d{6})(\d{2})?\s+(-?\d+)\s+(-?[\d.]+)\s+(\d+)\s+[~+]\s+(.*?)\s*$', l)
    if not m: continue
    hh, frac, snr, dt, hz, msg = m.groups()
    t = calendar.timegm(time.strptime(day + ' ' + hh, '%Y-%m-%d %H%M%S')) + (int(frac) / 100 if frac else 0)
    P = float(sys.argv[3]) if len(sys.argv) > 3 else 15.0
    t = round(t / P) * P
    msg = re.sub(r'(\s+\?)?(\s+a\d)?$', '', msg).strip()
    print(f'D {t:.1f} 1 {snr} {dt} {hz} {msg}')
