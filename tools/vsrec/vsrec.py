#!/usr/bin/env python3
"""vsrec.py — record raw IQ from a VibeServer radio (run ON the server; the radio ports are loopback-only).

usage: vsrec.py PORT  FREQ_HZ  MODE  RATE  SECONDS  OUT.cu8  [--back-to-adsb]
Leaves ADS-B if it is on, tunes, asks for raw IQ out at RATE, writes the rtl_tcp stream's 8-bit IQ (header dropped),
then turns IQ off and (with --back-to-adsb) puts the radio back in ADS-B. Standard library only.
"""
import base64, json, os, socket, struct, sys, time

port, freq, mode, rate, secs, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], int(sys.argv[4]), float(sys.argv[5]), sys.argv[6]
back = '--back-to-adsb' in sys.argv

def ws_open(path):
    s = socket.create_connection(('127.0.0.1', port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f'GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
               f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
    buf = b''
    while b'\r\n\r\n' not in buf:
        buf += s.recv(4096)
    head, rest = buf.split(b'\r\n\r\n', 1)
    if b' 101 ' not in head.split(b'\r\n')[0]:
        raise SystemExit('no websocket: ' + head.split(b'\r\n')[0].decode())
    return s, rest

def ws_send(s, text):
    p = text.encode(); m = os.urandom(4)
    hdr = bytes([0x81]) + (bytes([0x80 | len(p)]) if len(p) < 126 else bytes([0x80 | 126]) + struct.pack('>H', len(p)))
    s.sendall(hdr + m + bytes(b ^ m[i % 4] for i, b in enumerate(p)))

class Reader:
    def __init__(self, s, rest): self.s, self.buf = s, rest
    def need(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d: raise EOFError
            self.buf += d
        r, self.buf = self.buf[:n], self.buf[n:]; return r
    def frame(self):
        b0, b1 = self.need(2); n = b1 & 0x7F
        if n == 126: n = struct.unpack('>H', self.need(2))[0]
        elif n == 127: n = struct.unpack('>Q', self.need(8))[0]
        if b1 & 0x80: self.need(4)
        return b0 & 0x0F, self.need(n)

def texts_until(r, pred, t):
    end = time.time() + t
    r.s.settimeout(1.0)
    while time.time() < end:
        try: op, p = r.frame()
        except socket.timeout: continue
        if op == 1:
            try: j = json.loads(p)
            except Exception: continue
            if pred(j): return j
    return None

sid = 'vsrec' + os.urandom(4).hex()
s, rest = ws_open(f'/ws/user-spectrum?user_session_id={sid}&bins=256&proto=1')
r = Reader(s, rest)
time.sleep(1.0)
ws_send(s, json.dumps({'type': 'adsb', 'on': 0})); time.sleep(2.0)
ws_send(s, json.dumps({'type': 'tune', 'frequency': freq, 'mode': mode, 'modeSet': 1})); time.sleep(0.5)
bw = int(os.environ.get('BW', '0'))
if bw: ws_send(s, json.dumps({'type': 'bandwidth', 'bandwidthLow': -bw, 'bandwidthHigh': bw}))
for _ in range(int(float(os.environ.get('SETTLE', '2')) / 5) + 1):   # ★ settle, with a ping so the session is not released
    time.sleep(min(5.0, float(os.environ.get('SETTLE', '2')))); ws_send(s, json.dumps({'type': 'ping'}))
ws_send(s, json.dumps({'type': 'iqout', 'on': 1, 'rate': rate}))
j = texts_until(r, lambda j: j.get('type') == 'iqout', 8)
print('iqout:', j, flush=True)
got = 0
if j and j.get('on') == 1 and j.get('port'):
    c = socket.create_connection(('127.0.0.1', j['port']), timeout=10)
    with open(out, 'wb') as f:
        hdr = b''
        while len(hdr) < 12: hdr += c.recv(12 - len(hdr))
        want = int(secs * rate * 2); end = time.time() + secs + 20
        c.settimeout(2.0); lastPing = time.time()
        s.settimeout(0.05)
        while got < want and time.time() < end:
            try: d = c.recv(min(262144, want - got))
            except socket.timeout: d = b''
            if d: f.write(d); got += len(d)
            try: r.frame()          # keep the spectrum socket drained so the server does not stall it
            except Exception: pass
            if time.time() - lastPing > 5: ws_send(s, json.dumps({'type': 'ping'})); lastPing = time.time()
    c.close()
ws_send(s, json.dumps({'type': 'iqout', 'on': 0})); time.sleep(0.5)
if back:
    ws_send(s, json.dumps({'type': 'adsb', 'on': 1})); time.sleep(3.0)
s.close()
print(f'wrote {got} bytes = {got / 2 / rate:.1f} s at {rate} Hz to {out}', flush=True)
