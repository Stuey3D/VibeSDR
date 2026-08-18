#!/usr/bin/env python3
"""
probe-ubersdr.py — check an UberSDR instance against what our clients ASSUME.

    python3 scripts/probe-ubersdr.py 192.168.86.11:8080

★★★ WHY THIS EXISTS. On 2026-08-18 an UberSDR release changed its spectrum encoder to send
    DELTA frames only. Jr's decoder sized its bin array from a FULL frame, so every sample was
    discarded in silence: frames counted, fps healthy, socket green, waterfall BLACK. It cost
    an evening, and a five-minute packet capture would have found it immediately.

    OWRX, KiwiSDR and FM-DX are mature; UberSDR is evolving fast (Stuart, 2026-08-18: "UberSDR
    on the other hand is constantly evolving"). So the assumptions our clients make about its
    wire format need CHECKING after each update, not remembering.

Every check below is an assumption that lives in real client code. A FAIL is not necessarily a
server bug — it is a place our code is about to be wrong.
"""
import base64, gzip, json, os, socket, sys, time, urllib.request, uuid

def ws_open(host, port, path, ua="probe"):
    s = socket.create_connection((host, port), 8)
    k = base64.b64encode(os.urandom(16)).decode()
    s.send(f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
           f"Connection: Upgrade\r\nSec-WebSocket-Key: {k}\r\nSec-WebSocket-Version: 13\r\n"
           f"User-Agent: {ua}\r\n\r\n".encode())
    s.settimeout(3); hdr = b""
    while b"\r\n\r\n" not in hdr:
        hdr += s.recv(4096)
    return s, hdr.split(b"\r\n")[0].decode()

def ws_send(s, obj):
    d = json.dumps(obj).encode(); m = os.urandom(4)
    s.send(bytes([0x81, 0x80 | len(d)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(d)))

def ws_read(s, secs):
    """→ (spec_frames, bytes, set_of_flag_bytes, last_config_dict)"""
    n = tot = 0; flags = set(); cfg = None; buf = b""; t0 = time.time(); s.settimeout(1)
    while time.time() - t0 < secs:
        try: buf += s.recv(1 << 20)
        except socket.timeout: continue
        while len(buf) >= 2:
            ln = buf[1] & 0x7f; off = 2
            if ln == 126:
                if len(buf) < 4: break
                ln = int.from_bytes(buf[2:4], "big"); off = 4
            elif ln == 127:
                if len(buf) < 10: break
                ln = int.from_bytes(buf[2:10], "big"); off = 10
            if len(buf) < off + ln: break
            p = buf[off:off+ln]; buf = buf[off+ln:]
            tot += len(p)
            if p[:2] == b"\x1f\x8b":
                try:
                    j = json.loads(gzip.decompress(p))
                    if j.get("type") == "config": cfg = j
                except Exception: pass
            elif len(p) >= 22 and p[0:4] == b"SPEC":
                n += 1; flags.add(p[5])
    return n, tot, flags, cfg

def main(target):
    host, _, port = target.partition(":")
    port = int(port or 8080)
    base = f"http://{host}:{port}"
    sid = str(uuid.uuid4())
    fails = []
    def check(ok, name, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}{('  — ' + detail) if detail else ''}")
        if not ok: fails.append(name)

    print(f"\nUberSDR probe → {base}\n")

    r = urllib.request.urlopen(urllib.request.Request(
        base + "/connection", data=json.dumps({"user_session_id": sid}).encode(),
        headers={"Content-Type": "application/json"}), timeout=8)
    reg = json.load(r)
    check(reg.get("allowed") is True, "POST /connection registers a session (body-only)",
          f"allowed={reg.get('allowed')}")

    s, status = ws_open(host, port, f"/ws/user-spectrum?user_session_id={sid}&mode=binary8")
    check("101" in status, "spectrum socket upgrades", status)

    ws_send(s, {"type": "zoom", "frequency": 693000, "binBandwidth": 100})
    n, tot, flags, cfg = ws_read(s, 6)
    check(n > 0, "frames arrive after a zoom subscribe", f"{n/6:.1f} fps")
    check(cfg is not None, "the config is sent (gzipped binary, not text)")
    if cfg:
        check("binCount" in cfg, "config declares binCount — CLIENTS SIZE THEIR BIN ARRAY FROM IT",
              f"binCount={cfg.get('binCount')} binBandwidth={cfg.get('binBandwidth')}")
        check(not any(k in cfg for k in ("fps", "updateRate", "rate")),
              "config still does NOT declare the update rate (clients must measure it)")
    # ★ The one that bit us: full frames are NOT guaranteed. A client that needs one is broken.
    check(0x04 in flags or 0x02 in flags, "delta frames present", f"flags seen: {sorted(hex(f) for f in flags)}")
    if flags and not (flags & {0x01, 0x03}):
        print("        ↑ NOTE: delta-ONLY, no full frames. A decoder that sizes its array from a\n"
              "          full frame will discard every sample IN SILENCE. This is what broke Jr.")

    before = n / 6
    ws_send(s, {"type": "set_rate", "divisor": 2})
    time.sleep(1)
    n2, tot2, _, _ = ws_read(s, 6)
    check(n2 / 6 < before * 0.75, "set_rate divisor is honoured on a zoomed (private) session",
          f"{before:.1f} → {n2/6:.1f} fps, {tot2/1024/6:.1f} KB/s")
    s.close()

    s2, _ = ws_open(host, port, f"/ws/user-spectrum?user_session_id={sid}&mode=binary8&bins=128")
    n3, tot3, _, cfg3 = ws_read(s2, 4)
    binc = (cfg3 or {}).get("binCount")
    check(binc is None or binc != 128, "bin count is still FIXED server-side (&bins= ignored)",
          f"asked 128, got {binc}")
    s2.close()

    print(f"\n{'ALL ASSUMPTIONS HOLD' if not fails else 'BROKEN: ' + ', '.join(fails)}\n")
    return 1 if fails else 0

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    sys.exit(main(sys.argv[1]))
