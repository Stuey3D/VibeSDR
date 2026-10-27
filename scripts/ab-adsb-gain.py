#!/usr/bin/env python3
"""ab-adsb-gain.py — a same-dongle A/B of ADS-B receivers, in alternating windows (V12, the ADS-B gain rule, 2026-10-26).

Each ARM is a way of running the one dongle: dump1090-fa, or a VibeServer build (the new gain rule, the old one). The
script starts an arm, lets it settle, measures it for a window, stops it, starts the next, and so on round after round,
so a sky that fills and empties over the day lands on every arm alike (the 10-25 FlightAware run: D / V24 / V256).
One CSV row per window, the same columns as the 10-25 run (fa.csv) plus the VibeServer rule's own reason and warning.

  ★ Read-only towards everything but the services it is told to start and stop. Stdlib only (Python 3.7+): the
    websocket client is the few lines below, so nothing needs installing on the Pi.
  ★ A VibeServer arm is measured over its own websocket as an ordinary listener; with `"enter": true` it asks for ADS-B
    (a radio whose landing is ADS-B, or kept running, is in it already).

Arms come from a JSON file, e.g. for the FlightAware Pi (see docs/v12/ADSB-GAIN.md for the exact commands):
  [
    {"name": "D",    "kind": "dump1090", "start": "sudo systemctl start dump1090-fa", "stop": "sudo systemctl stop dump1090-fa",
     "json": "/run/dump1090-fa"},
    {"name": "Vnew", "kind": "vibe", "start": "sudo systemctl start vibeserver", "stop": "sudo systemctl stop vibeserver",
     "url": "http://127.0.0.1:48001", "enter": true}
  ]
  python3 ab-adsb-gain.py --arms arms.json --rounds 16 --settle 150 --window 600 --out ab.csv [--lat 51.x --lon -1.x]

Only one arm runs at a time; the script stops every arm before it starts (and in a finally) so the dongle is never
fought over. Ctrl-C ends the run cleanly with the dongle handed back to the FIRST arm (normally dump1090-fa).
"""
import argparse, base64, csv, json, math, os, random, socket, struct, subprocess, sys, time, urllib.request


# ── a minimal websocket client (RFC 6455): text frames in, masked text frames out, pings answered ─────────────────
class WS:
    def __init__(self, url, timeout=10):
        assert url.startswith('ws://'), url
        hostport, _, path = url[5:].partition('/')
        host, _, port = hostport.partition(':')
        self.s = socket.create_connection((host, int(port or 80)), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f'GET /{path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
               f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n')
        self.s.sendall(req.encode())
        head = b''
        while b'\r\n\r\n' not in head:
            c = self.s.recv(1)
            if not c: raise IOError('websocket handshake: closed')
            head += c
        if b' 101 ' not in head.split(b'\r\n', 1)[0]: raise IOError('websocket handshake: ' + head[:80].decode('latin1'))
        self.buf = b''

    def _read(self, n):
        while len(self.buf) < n:
            c = self.s.recv(65536)
            if not c: raise IOError('websocket closed')
            self.buf += c
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, text, op=1):
        data = text.encode() if isinstance(text, str) else text
        mask = os.urandom(4)
        n = len(data)
        hdr = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack('>H', n) if n < 65536
                                     else bytes([0x80 | 127]) + struct.pack('>Q', n))
        self.s.sendall(hdr + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(data)))

    def recv_text(self):
        """The next complete text message (binary frames — the waterfall — are skipped)."""
        parts, op0 = [], None
        while True:
            b0, b1 = self._read(2)
            op, fin, n = b0 & 0x0F, b0 & 0x80, b1 & 0x7F
            if n == 126: n = struct.unpack('>H', self._read(2))[0]
            elif n == 127: n = struct.unpack('>Q', self._read(8))[0]
            if b1 & 0x80: mk = self._read(4)
            payload = self._read(n)
            if op == 9: self.send(payload, op=10); continue          # ping → pong
            if op == 8: raise IOError('websocket closed by the server')
            if op in (1, 2): op0, parts = op, [payload]
            elif op == 0: parts.append(payload)
            if fin and op0 is not None:
                msg = b''.join(parts)
                if op0 == 1: return msg.decode('utf-8', 'replace')
                op0, parts = None, []

    def close(self):
        try: self.s.close()
        except Exception: pass


def nm(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * 3440.065 * math.asin(min(1.0, math.sqrt(a)))


def sh(cmd):
    if cmd: subprocess.run(cmd, shell=True, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)


def get_json(url_or_path, timeout=5):
    if url_or_path.startswith('http'):
        with urllib.request.urlopen(url_or_path, timeout=timeout) as r: return json.loads(r.read().decode())
    with open(url_or_path) as f: return json.load(f)


class Window:
    """What one arm did over one window."""
    def __init__(self):
        self.hexes, self.pos, self.far = set(), set(), 0.0
        self.gains, self.caps, self.noises = [], [], []
        self.msg0 = self.msg1 = None; self.t0 = self.t1 = None
        self.why, self.warn = '', ''

    def aircraft(self, now, hexkey, lat, lon, seen, rx):
        if seen is not None and seen > 60: return
        self.hexes.add(hexkey)
        if lat is not None and lon is not None:
            self.pos.add(hexkey)
            if rx: self.far = max(self.far, nm(rx[0], rx[1], lat, lon))

    def messages(self, now, total):
        if total is None: return
        if self.msg0 is None: self.msg0, self.t0 = total, now
        self.msg1, self.t1 = total, now

    def row(self):
        rate = (self.msg1 - self.msg0) / (self.t1 - self.t0) if self.msg0 is not None and self.t1 and self.t1 > self.t0 else ''
        g = [x for x in self.gains if x is not None]
        return {'msg_s': round(rate, 1) if rate != '' else '', 'aircraft': len(self.hexes), 'positioned': len(self.pos),
                'far_nm': round(self.far, 1), 'gain_last': g[-1] if g else '', 'gain_min': min(g) if g else '',
                'gain_max': max(g) if g else '', 'capture_min': min(self.caps) if self.caps else '',
                'noise_med': sorted(self.noises)[len(self.noises) // 2] if self.noises else '', 'why': self.why, 'warn': self.warn}


def measure_dump1090(arm, secs, rx):
    w, d, end = Window(), arm.get('json', '/run/dump1090-fa'), time.time() + secs
    while time.time() < end:
        now = time.time()
        try:
            a = get_json(d + '/aircraft.json')
            w.messages(now, a.get('messages'))
            for ac in a.get('aircraft', []):
                w.aircraft(now, ac.get('hex'), ac.get('lat'), ac.get('lon'), ac.get('seen'), rx)
            s = get_json(d + '/stats.json')
            m = s.get('last1min', {})
            g = (m.get('local', {}) or {}).get('gain_db', s.get('gain_db', (m.get('adaptive', {}) or {}).get('gain_db')))
            if g is not None: w.gains.append(round(float(g), 1))
            nz = (m.get('local', {}) or {}).get('noise')
            if nz is not None: w.noises.append(round(float(nz), 1))
        except Exception as e:
            print('   dump1090 read:', e, file=sys.stderr)
        time.sleep(max(0.0, 10 - (time.time() - now)))
    return w


def measure_vibe(arm, secs, rx, settle_left=0):
    base = arm['url'].rstrip('/')
    sid = '%032x' % random.getrandbits(128)
    ws = WS(base.replace('http://', 'ws://') + f'/ws/user-spectrum?user_session_id={sid}&mode=binary8&bins=256&proto=1', timeout=15)
    w, end, last_ping = Window(), time.time() + secs, 0
    try:
        if arm.get('enter'): ws.send(json.dumps({'type': 'adsb', 'on': 1}))
        while time.time() < end:
            if time.time() - last_ping > 5: ws.send('{"type":"ping"}'); last_ping = time.time()
            try: m = json.loads(ws.recv_text())
            except socket.timeout: continue
            if m.get('type') != 'adsb': continue
            now = time.time()
            w.messages(now, m.get('messages'))
            for ac in m.get('aircraft', []):
                w.aircraft(now, ac.get('hex'), ac.get('lat'), ac.get('lon'), ac.get('seen'), rx)
            if m.get('gain') is not None: w.gains.append(m['gain'])
            if m.get('capture') is not None and m['capture'] >= 0: w.caps.append(m['capture'])
            if m.get('noise') is not None: w.noises.append(m['noise'])
            if m.get('gainWhy'): w.why = m['gainWhy']
            w.warn = m.get('feedWarn', '') or ''
    finally:
        ws.close()
    return w


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arms', required=True, help='JSON file: a list of arms (see the top of this file)')
    ap.add_argument('--rounds', type=int, default=16)
    ap.add_argument('--settle', type=int, default=150, help='seconds after an arm starts before it is measured (our ramp ~60 s)')
    ap.add_argument('--window', type=int, default=600, help='seconds measured per arm per round')
    ap.add_argument('--out', default='ab-adsb-gain.csv')
    ap.add_argument('--lat', type=float); ap.add_argument('--lon', type=float)
    ap.add_argument('--dump1090-json', default='/run/dump1090-fa', help='for the receiver position (receiver.json) if --lat/--lon are not given')
    a = ap.parse_args()
    arms = json.load(open(a.arms))
    rx = (a.lat, a.lon) if a.lat is not None and a.lon is not None else None
    if rx is None:
        try:
            r = get_json(a.dump1090_json + '/receiver.json'); rx = (r['lat'], r['lon'])
        except Exception:
            print('no receiver position (give --lat/--lon): far_nm will read 0', file=sys.stderr)
    cols = ['start', 't_mid', 'kind', 'msg_s', 'aircraft', 'positioned', 'far_nm', 'gain_last', 'gain_min', 'gain_max',
            'capture_min', 'noise_med', 'why', 'warn']
    new = not os.path.exists(a.out)
    f = open(a.out, 'a', newline=''); wr = csv.DictWriter(f, fieldnames=cols)
    if new: wr.writeheader(); f.flush()
    stop_all = lambda: [sh(x.get('stop')) for x in arms]
    try:
        for rnd in range(a.rounds):
            for arm in arms:
                stop_all(); time.sleep(3)
                sh(arm.get('start'))
                t_start = time.time()
                print(time.strftime('%H:%M:%S'), f'round {rnd + 1}/{a.rounds} arm {arm["name"]}: settling {a.settle} s', flush=True)
                if arm['kind'] == 'vibe':
                    # ★ the settle is spent connected (so the ramp runs with a listener, as in use), then measured afresh
                    for _ in range(30):
                        try: urllib.request.urlopen(arm['url'].rstrip('/') + '/vibeserver.json', timeout=3); break
                        except Exception: time.sleep(2)
                    measure_vibe(arm, a.settle, rx)
                    w = measure_vibe(arm, a.window, rx)
                else:
                    time.sleep(a.settle)
                    w = measure_dump1090(arm, a.window, rx)
                row = {'start': time.strftime('%H:%M', time.localtime(t_start)), 't_mid': int(time.time() - a.window / 2),
                       'kind': arm['name'], **w.row()}
                wr.writerow(row); f.flush()
                print('   ', json.dumps(row), flush=True)
    except KeyboardInterrupt:
        print('interrupted', file=sys.stderr)
    finally:
        stop_all(); time.sleep(2)
        sh(arms[0].get('start'))          # ★ the dongle back to the first arm (normally dump1090-fa / the owner's own)
        f.close()


if __name__ == '__main__':
    main()
