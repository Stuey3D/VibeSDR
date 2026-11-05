#!/usr/bin/env python3
# hwd-client.py [SOCK] — exercise vibe-hwd's socket the way the engine does (§0.2), and print every line.
#   hwd-client.py /run/vibe-hwd.sock                      the standard run (state, brightness, volume, errors, ssh)
#   hwd-client.py /run/vibe-hwd.sock listen 30            subscribe and print events for 30 s
#   hwd-client.py /run/vibe-hwd.sock raw '{"get":"state"}' one request
import json, socket, sys, time

sock = sys.argv[1] if len(sys.argv) > 1 else "/run/vibe-hwd.sock"
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(100):                     # ★ the service may still be starting
    try:
        s.connect(sock)
        break
    except (FileNotFoundError, ConnectionRefusedError):
        time.sleep(0.1)
f = s.makefile("rwb", buffering=0)

def req(obj, show=True):
    line = obj if isinstance(obj, str) else json.dumps(obj)
    t0 = time.monotonic()
    f.write((line + "\n").encode())
    while True:
        r = f.readline().decode().strip()
        if '"event"' not in r:
            break
        print("   event:", r)
    if show:
        print("%-52s -> %s  (%.0f ms)" % (line[:52], r, (time.monotonic() - t0) * 1000))
    return json.loads(r)

mode = sys.argv[2] if len(sys.argv) > 2 else "standard"
if mode == "raw":
    req(sys.argv[3]); sys.exit(0)
if mode == "listen":
    req({"subscribe": True})
    s.settimeout(float(sys.argv[3]) if len(sys.argv) > 3 else 30)
    try:
        while True:
            line = f.readline().decode().strip()
            print("%.1f event: %s" % (time.monotonic() % 1000, line), flush=True)
            if '"shutdown"' in line:     # ★ act like the engine: save (1 s), then ack
                time.sleep(1.0)
                f.write(b'{"ack":"shutdown"}\n')
                print("%.1f sent ack" % (time.monotonic() % 1000), flush=True)
    except (socket.timeout, TimeoutError):
        sys.exit(0)

st = req({"get": "state"})
b0, v0 = st["brightness"], st["volume"]
req({"set": "brightness", "value": 60})
req({"set": "brightness", "value": 300})
req({"set": "brightness", "value": b0})
req({"set": "volume", "value": 40})
req({"set": "volume", "value": 101})
req({"set": "volume", "value": v0})
req({"get": "ssh"})
req({"set": "ssh", "on": True})
req({"set": "ssh-password", "password": "short"})
req({"set": "time", "utcMs": 12})
req({"action": "dance"})
req("this is not json")
req({"subscribe": True})
req({"action": "screen-off"})
time.sleep(1)
req({"action": "screen-on"})
req({"get": "state"})
