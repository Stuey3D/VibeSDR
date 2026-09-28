#!/usr/bin/env python3
"""
SpyServer protocol capture proxy.

Sits between a real SpyServer client (SDR++ / SDR#) and a real SpyServer, and
records every byte in each direction. Needs no root (unlike tcpdump) and gives a
cleaner artifact than a pcap: the two directions are already separated, so there
is no TCP reassembly to do before reading the protocol off the wire.

  ./ss_proxy.py --listen 5556 --server 192.168.86.99:5555

Then point the client at 127.0.0.1:5556.

It listens on the LOOPBACK only by default: an open relay to somebody's receiver
that also records everything it carries should not be reachable from the LAN by
accident. When the client runs on ANOTHER machine (SDR# on a Windows box), say so:

  ./ss_proxy.py --bind 0.0.0.0 --listen 5556 --server 192.168.86.99:5555

Writes, next to this script:
  c2s.bin    every byte client -> server   (commands: hello, set setting, ...)
  s2c.bin    every byte server -> client   (device info, client sync, FFT, IQ)
  log.txt    timestamped chunk index: direction, offset, length, first 64 bytes

The IQ stream is large, so s2c.bin is capped by default (--max-s2c). The header,
handshake and the first FFT/IQ frames are what matter; gigabytes of IQ do not.
"""
import argparse
import os
import socket
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
start = time.time()
log_lock = threading.Lock()


def log(logf, direction, offset, data):
    with log_lock:
        logf.write(
            f"[{time.time() - start:8.3f}] {direction} off={offset:<10} len={len(data):<6} "
            f"{data[:64].hex()}\n"
        )
        logf.flush()


def pump(src, dst, direction, path, logf, max_bytes, counters):
    total = 0
    truncated = False
    with open(path, "wb") as f:
        while True:
            try:
                data = src.recv(65536)
            except OSError:
                break
            if not data:
                break
            # Always log the chunk boundary, even once we stop saving payload —
            # frame timing and sizes stay useful after the byte cap is hit.
            log(logf, direction, total, data)
            if max_bytes <= 0 or total < max_bytes:
                f.write(data)
                f.flush()
            elif not truncated:
                truncated = True
                log(logf, direction, total, b"<<< payload cap reached; still logging headers")
            total += len(data)
            counters[direction] = total
            try:
                dst.sendall(data)
            except OSError:
                break
    try:
        dst.shutdown(socket.SHUT_WR)
    except OSError:
        pass
    print(f"  {direction} closed after {total} bytes")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", type=int, default=5556)
    ap.add_argument("--bind", default="127.0.0.1",
                    help="address to listen on (default 127.0.0.1; 0.0.0.0 for a client on another machine)")
    ap.add_argument("--server", required=True, help="host:port of the real SpyServer")
    ap.add_argument("--max-s2c", type=int, default=8 * 1024 * 1024,
                    help="cap saved server->client bytes (0 = unlimited)")
    args = ap.parse_args()

    host, _, port = args.server.partition(":")
    target = (host, int(port))

    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # ★ CodeQL py/bind-socket-all-network-interfaces (2026-09-28): this was hard-wired to 0.0.0.0
    #   while the banner below claimed 127.0.0.1. Loopback unless the user asks for more.
    ls.bind((args.bind, args.listen))
    ls.listen(1)
    print(f"proxy listening on {args.bind}:{args.listen} -> {target[0]}:{target[1]}")
    print("point SDR++ / SDR# at the listen address, then connect")

    # Serve connections in a LOOP. A single accept() is wrong twice over: any
    # stray probe kills the proxy, and real clients routinely connect, drop and
    # reconnect while the user fiddles with settings. Each session gets its own
    # numbered files so a botched attempt doesn't overwrite a good capture.
    session = 0
    logf = open(os.path.join(HERE, "log.txt"), "w")
    while True:
        cs, addr = ls.accept()
        session += 1
        print(f"[session {session}] client connected from {addr[0]}")
        logf.write(f"\n===== session {session} from {addr[0]} =====\n"); logf.flush()
        try:
            ss = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            ss.connect(target)
        except OSError as e:
            print(f"[session {session}] upstream connect failed: {e}")
            cs.close(); continue
        print(f"[session {session}] upstream connected; recording")
        counters = {}
        sfx = "" if session == 1 else f".{session}"
        t1 = threading.Thread(target=pump, args=(cs, ss, "c2s", os.path.join(HERE, f"c2s{sfx}.bin"), logf, 0, counters))
        t2 = threading.Thread(target=pump, args=(ss, cs, "s2c", os.path.join(HERE, f"s2c{sfx}.bin"), logf, args.max_s2c, counters))
        t1.start(); t2.start()
        t1.join(); t2.join()
        cs.close(); ss.close()
        print(f"[session {session}] done. c2s={counters.get('c2s', 0)} s2c={counters.get('s2c', 0)} bytes")
        print(f"artifacts in {HERE}: c2s{sfx}.bin, s2c{sfx}.bin, log.txt  (still listening)")


if __name__ == "__main__":
    main()
