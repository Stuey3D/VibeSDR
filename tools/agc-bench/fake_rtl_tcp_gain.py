#!/usr/bin/env python3
"""fake_rtl_tcp_gain.py — an rtl_tcp source whose GAIN IS REAL, for driving VibeAGC end to end (2026-10-20).

vibeserver/fake-rtl-tcp.mjs acknowledges a gain command and changes nothing, so a gain loop run against it is open
loop. This one applies it: every sample is scaled by 10^((G - G0)/20) for the tuner gain G the server last set, then
quantised to the dongle's 8 bits with the rails CLIPPED — so too much gain overloads exactly the way a dongle does,
and the server's own ADC statistics (peak, rail count) see it. The centre-frequency command shifts the content, so
the server's tuner-follows-the-dial moves are real too.

Sources (one of):
  --file X.cu8 --file-centre HZ   replay a real capture (unsigned 8-bit IQ) recorded with the tuner at HZ, on a loop
  --synth                          two USB stations with a speech envelope (syllables, gaps, 1-3 s phrase pauses):
                                   A at --a-hz (loud: it is the biggest thing in the capture — the worst case for a
                                   headroom rule), B at --b-hz (weaker), over a noise floor. Deterministic (--seed).
Extras:
  --g0 TENTHS        the gain at which the source is "as recorded" (default 280 = 28.0 dB)
  --blowtorch-at S   from S seconds after the first client connects, add a carrier at --blowtorch-hz,
                     --blowtorch-dbfs dBFS at g0 (scaled by the gain like everything else) — a genuine overload
  --rate HZ          sample rate (default 250000); the server's own rate command is honoured if it differs
  --log FILE         one line per command (tune / gain / rate), with a timestamp

Silent: it writes IQ to a socket and nothing else.
"""
import argparse, math, socket, struct, sys, threading, time
import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument('--port', type=int, default=1234)
ap.add_argument('--rate', type=int, default=250000)
ap.add_argument('--file'); ap.add_argument('--file-centre', type=float, default=0.0)
ap.add_argument('--synth', action='store_true')
ap.add_argument('--a-hz', type=float, default=14.200e6); ap.add_argument('--b-hz', type=float, default=14.203e6)
ap.add_argument('--a-dbfs', type=float, default=-12.0); ap.add_argument('--b-dbfs', type=float, default=-30.0)
ap.add_argument('--floor-dbfs', type=float, default=-48.0)
ap.add_argument('--qsb-db', type=float, default=0.0)      # slow fading (QSB), +/- dB, 5-15 s, per station
ap.add_argument('--c-hz', type=float, default=0.0)        # a third, strong SSB neighbour keying up and down
ap.add_argument('--c-dbfs', type=float, default=-20.0)
ap.add_argument('--seed', type=int, default=1)
ap.add_argument('--g0', type=int, default=280)
ap.add_argument('--blowtorch-at', type=float, default=-1.0)
ap.add_argument('--blowtorch-hz', type=float, default=14.230e6)
ap.add_argument('--blowtorch-dbfs', type=float, default=6.0)
ap.add_argument('--log')
a = ap.parse_args()

RATE = a.rate
state = {'centre': a.file_centre if a.file else a.a_hz, 'gain': a.g0, 'rate': RATE, 'first': None}
logf = open(a.log, 'a', buffering=1) if a.log else None
def log(s):
    if logf: logf.write('%.3f %s\n' % (time.time(), s))

# ── the content, as complex baseband at RATE around `content_centre` ─────────────────────────────────────────────
if a.file:
    raw = np.fromfile(a.file, dtype=np.uint8)
    content = ((raw[0::2].astype(np.float32) - 127.5) + 1j * (raw[1::2].astype(np.float32) - 127.5)).astype(np.complex64) / 127.5
    content_centre = a.file_centre
else:
    rng = np.random.default_rng(a.seed)
    secs = 20.0
    n = int(RATE * secs)
    def envelope(rng):
        # syllables at -0 +/- 5 dB, gaps, 12 % phrase pauses of 1-3 s; 100 Hz control rate, smoothed
        fs_e = 100; out = []
        while len(out) < secs * fs_e:
            if rng.random() < 0.12:
                out += [0.0] * int((1 + 2 * rng.random()) * fs_e)
            else:
                lvl = 10 ** ((5.0 * rng.standard_normal()) / 20)
                out += [lvl] * max(1, int((0.15 + 0.25 * rng.random()) * fs_e))
                out += [0.0] * int((0.05 + 0.25 * rng.random()) * fs_e)
        e = np.array(out[:int(secs * fs_e)], dtype=np.float64)
        k = np.ones(3) / 3; e = np.convolve(e, k, mode='same')        # ~30 ms edges
        return np.interp(np.arange(n) / RATE, np.arange(len(e)) / fs_e, e)
    def qsb(rng):
        # slow fading: a sum of three sinusoids with random 5-15 s periods, scaled to +/- qsb_db
        if a.qsb_db <= 0: return 1.0
        tt = np.arange(n) / RATE; f = np.zeros(n)
        for _ in range(3):
            per = 5 + 10 * rng.random(); f += np.sin(2 * np.pi * tt / per + 2 * np.pi * rng.random())
        f /= 3.0
        return 10 ** (a.qsb_db * f / 20)
    def ssb(rng, dbfs):
        # band-limited noise 300-2700 Hz ABOVE the carrier (USB), by FFT synthesis; peak-normalised
        X = np.zeros(n, dtype=np.complex128)
        f = np.fft.fftfreq(n, 1.0 / RATE)
        band = (f >= 300) & (f <= 2700)
        X[band] = rng.standard_normal(band.sum()) + 1j * rng.standard_normal(band.sum())
        x = np.fft.ifft(X)
        x /= np.percentile(np.abs(x), 99.9)
        return (x * envelope(rng) * qsb(rng) * 10 ** (dbfs / 20)).astype(np.complex64)
    content_centre = a.a_hz
    t = np.arange(n) / RATE
    A = ssb(rng, a.a_dbfs)
    B = ssb(rng, a.b_dbfs) * np.exp(2j * np.pi * (a.b_hz - a.a_hz) * t).astype(np.complex64)
    noise = ((rng.standard_normal(n) + 1j * rng.standard_normal(n)) * 10 ** (a.floor_dbfs / 20) / math.sqrt(2)).astype(np.complex64)
    content = (A + B + noise).astype(np.complex64)
    if a.c_hz:
        content += (ssb(rng, a.c_dbfs) * np.exp(2j * np.pi * (a.c_hz - a.a_hz) * t).astype(np.complex64)).astype(np.complex64)
    del A, B, noise, t
print('[fake-rtl-tcp-gain] %d samples of content (%.1f s) at %d S/s' % (len(content), len(content) / RATE, RATE), file=sys.stderr)

def serve(conn):
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    conn.sendall(b'RTL0' + struct.pack('>II', 5, 29))   # R820T, 29 gains
    if state['first'] is None: state['first'] = time.time()
    stop = threading.Event()
    def reader():
        buf = b''
        try:
            while not stop.is_set():
                d = conn.recv(4096)
                if not d: break
                buf += d
                while len(buf) >= 5:
                    cmd, val = buf[0], struct.unpack('>I', buf[1:5])[0]; buf = buf[5:]
                    if cmd == 0x01: state['centre'] = float(val); log('tune %d' % val)
                    elif cmd == 0x02: state['rate'] = val; log('rate %d' % val)
                    elif cmd == 0x04: state['gain'] = int(val); log('gain %d' % val)
                    elif cmd == 0x03: log('gainmode %d' % val)
        except OSError: pass
        stop.set()
    threading.Thread(target=reader, daemon=True).start()
    pos = 0; phase = 0.0; bt_phase = 0.0
    chunk = RATE // 20                                     # 50 ms
    due = time.time()
    try:
        while not stop.is_set():
            idx = (pos + np.arange(chunk)) % len(content); pos = (pos + chunk) % len(content)
            x = content[idx]
            # the tuner sits at state['centre']; content lives around content_centre
            shift = content_centre - state['centre']
            w = 2 * np.pi * shift / RATE
            ph = phase + w * np.arange(chunk); phase = float((phase + w * chunk) % (2 * np.pi))
            x = x * np.exp(1j * ph).astype(np.complex64)
            if a.blowtorch_at >= 0 and time.time() - state['first'] >= a.blowtorch_at:
                wb = 2 * np.pi * (a.blowtorch_hz - state['centre']) / RATE
                pb = bt_phase + wb * np.arange(chunk); bt_phase = float((bt_phase + wb * chunk) % (2 * np.pi))
                x = x + (10 ** (a.blowtorch_dbfs / 20) * np.exp(1j * pb)).astype(np.complex64)
            g = 10 ** ((state['gain'] - a.g0) / 200.0)
            y = x * g * 127.5
            iq = np.empty(chunk * 2, dtype=np.float32); iq[0::2] = y.real; iq[1::2] = y.imag
            out = np.clip(np.round(iq + 127.5), 0, 255).astype(np.uint8)   # ★ the 8-bit converter: rails CLIP
            conn.sendall(out.tobytes())
            due += chunk / RATE
            d = due - time.time()
            if d > 0: time.sleep(d)
            elif d < -0.5: due = time.time()
    except OSError: pass
    stop.set()
    try: conn.close()
    except OSError: pass

srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', a.port)); srv.listen(2)
print('[fake-rtl-tcp-gain] listening on 127.0.0.1:%d' % a.port, file=sys.stderr)
while True:
    c, _ = srv.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
