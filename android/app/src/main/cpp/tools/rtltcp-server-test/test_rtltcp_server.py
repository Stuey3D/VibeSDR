#!/usr/bin/env python3
"""Host test of the Android RTL-TCP server (rtl_tcp_server.cpp) against a tiny rtl_tcp client.

Silent: no audio anywhere — it only counts bytes. Run via ./run.sh (builds the harness first).

  rtl    — a real dongle: header RTL0 + tuner + gain count, then u8 IQ at the rate the client SETS.
  airspy — a non-RTL radio (Airspy HF+ / R2, HackRF) that librtlsdr "opens" anyway: the server must
           REFUSE to start with a reason, not send the header and then nothing for ever (Sloper,
           2026-11-25: "connected - good", no waterfall, no audio).
"""
import os, socket, struct, subprocess, sys, time

HARNESS = sys.argv[1]
fails = []

def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)

def run(radio, port):
    env = dict(os.environ, FAKE_RADIO=radio)
    p = subprocess.Popen([HARNESS, str(port)], env=env, stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    first = p.stdout.readline().strip()
    return p, first

def cmd(s, code, val):
    s.sendall(struct.pack(">BI", code, val))

def client_session(port, rate=1_024_000, secs=1.5):
    s = socket.create_connection(("127.0.0.1", port), timeout=3)
    hdr = b""
    while len(hdr) < 12:
        chunk = s.recv(12 - len(hdr))
        if not chunk: break
        hdr += chunk
    # What SDR++ / SDR# send straight after the header: rate, freq, gain mode, AGC.
    cmd(s, 0x02, rate); cmd(s, 0x01, 145_800_000); cmd(s, 0x03, 0); cmd(s, 0x08, 0)
    s.settimeout(0.3)
    got, total, sample = 0, 0, bytearray()
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            b = s.recv(65536)
        except socket.timeout:
            continue
        if not b: break
        got += len(b)
        if len(sample) < 1 << 16: sample += b
    s.close()
    return hdr, got, sample

print("rtl: a real RTL-SDR")
p, first = run("rtl", 41234)
check(first.startswith("START OK"), f"server starts ({first})")
if first.startswith("START OK"):
    time.sleep(0.2)
    hdr, got, sample = client_session(41234)
    check(hdr[:4] == b"RTL0", "12-byte header starts RTL0")
    tuner, gains = struct.unpack(">II", hdr[4:12]) if len(hdr) == 12 else (0, 0)
    check(tuner == 5 and gains == 29, f"header carries tuner 5 / 29 gains (got {tuner}/{gains})")
    want = 1_024_000 * 2 * 1.5
    check(got > want * 0.6, f"IQ streams at the client's rate ({got} bytes in 1.5 s, ~{int(want)} expected)")
    mean = sum(sample) / len(sample) if sample else 0
    check(100 < mean < 155, f"u8 offset-binary IQ (mean {mean:.1f} ~ 127.5)")
p.stdin.close(); p.wait(timeout=5)

print("airspy: a non-RTL radio (Airspy / HackRF) behind librtlsdr")
p, first = run("airspy", 41235)
if first.startswith("START OK"):
    time.sleep(0.2)
    hdr, got, _ = client_session(41235, secs=1.0)
    print(f"  (server STARTED: header {hdr[:4]!r} tuner {struct.unpack('>I', hdr[4:8])[0] if len(hdr) >= 8 else '?'}, "
          f"then {got} IQ bytes in 1 s — the client says connected and shows nothing)")
check(first.startswith("START REFUSED"), f"server refuses a radio librtlsdr cannot stream ({first})")
try:
    p.stdin.close(); p.wait(timeout=5)
except Exception:
    p.kill()

print("PASS" if not fails else f"FAILED: {len(fails)}")
sys.exit(1 if fails else 0)
