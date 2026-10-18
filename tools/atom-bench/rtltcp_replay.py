#!/usr/bin/env python3
# Paced rtl_tcp replay: serves u8 IQ files at real time; switches file on a sample-rate command.
# usage: rtltcp_replay.py PORT RATE=FILE [RATE=FILE ...]
import socket, sys, time, threading, struct
port = int(sys.argv[1]); files = {}
for a in sys.argv[2:]:
    r, f = a.split('=', 1); files[int(r)] = open(f, 'rb').read()
def serve(c):
    rate = [max(files)] if 2400000 not in files else [2400000]
    c.sendall(b'RTL0' + struct.pack('>II', 5, 29))
    def rx():
        buf = b''
        while True:
            d = c.recv(64)
            if not d: return
            buf += d
            while len(buf) >= 5:
                cmd, arg = buf[0], struct.unpack('>I', buf[1:5])[0]; buf = buf[5:]
                if cmd == 2:
                    print('rate', arg, flush=True)
                    if arg in files: rate[0] = arg
    threading.Thread(target=rx, daemon=True).start()
    pos = 0; t = time.monotonic(); tick = 0.01
    try:
        while True:
            data = files.get(rate[0]) or files[max(files)]
            n = int(rate[0] * 2 * tick) & ~1
            if pos + n > len(data): pos = 0
            c.sendall(data[pos:pos + n]); pos += n
            t += tick
            d = t - time.monotonic()
            if d > 0: time.sleep(d)
            elif d < -1: t = time.monotonic()
    except OSError:
        print('client gone', flush=True)
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port)); s.listen(4)
while True:
    c, _ = s.accept(); print('client', flush=True)
    threading.Thread(target=serve, args=(c,), daemon=True).start()
