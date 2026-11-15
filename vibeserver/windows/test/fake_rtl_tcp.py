# Minimal rtl_tcp stand-in for the Wine smoke test: header + an endless 8-bit IQ tone (silent test, no audio out).
import socket, math, struct, sys, threading
port = int(sys.argv[1]) if len(sys.argv) > 1 else 1234
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", port)); s.listen(4)
blk = bytearray()
for i in range(65536):
    ph = 2 * math.pi * 10000 * i / 2400000
    blk += bytes((int(127.5 + 60 * math.cos(ph)), int(127.5 + 60 * math.sin(ph))))
def serve(c):
    try:
        c.sendall(b"RTL0" + struct.pack(">II", 5, 29))
        c.setblocking(True)
        while True: c.sendall(blk)
    except Exception: pass
while True:
    c, _ = s.accept(); threading.Thread(target=serve, args=(c,), daemon=True).start()
