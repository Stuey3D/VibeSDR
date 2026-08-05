# Load generator meant to run ON THE SERVER, over loopback — so the simulated listeners never
# touch the network the real listener is using. Running them on a laptop instead measures the
# laptop's Wi-Fi as much as the server, which is how an early run wrongly looked like a server
# fault.
#
# ★★★ IT MUST ANSWER PINGS. The server probes every 5 s and drops a peer after 20 s of silence,
#     so a generator that ignores them quietly evaporates — the first version did, and every
#     measurement taken with it was of an EMPTY server that had merely looked busy for twenty
#     seconds. It printed "29 listeners up" and the server was serving one.
#     ★ Hence the "still connected" line every 5 s: a load test that cannot prove the load is
#       still there proves nothing at all.
#
#   python3 load.py <listeners> <port> [seconds]
import socket, threading, base64, os, struct, json, time, sys

N      = int(sys.argv[1])
PORT   = int(sys.argv[2])
DUR    = int(sys.argv[3]) if len(sys.argv) > 3 else 60
CENTRE = int(os.environ.get('CENTRE', '6500000'))
SPAN   = int(os.environ.get('SPAN', '8000000'))
HW     = 15000                      # offset tuning: the radio sits this far above the logical centre

stop = False
live = [0]
lock = threading.Lock()


def wsframe(op, payload=b''):
    h = bytearray([0x80 | op])
    if len(payload) < 126:
        h.append(0x80 | len(payload))
    else:
        h.append(0x80 | 126)
        h += struct.pack('>H', len(payload))
    h += b'\x00\x00\x00\x00'        # mask of zeroes: masked flag set, payload unchanged
    return bytes(h) + payload


def client(i):
    # Spread them over the locked window, as real listeners on different stations would be.
    f = CENTRE + HW + int((i / max(1, N - 1) - 0.5) * SPAN * 0.8)
    socks = []
    counted = False
    try:
        for path in ('/ws/user-spectrum?user_session_id=P%d&bins=1024' % i,
                     '/ws/audio?user_session_id=P%d&codec=opus' % i):
            s = socket.create_connection(('127.0.0.1', PORT), 5)
            s.settimeout(1.0)
            k = base64.b64encode(os.urandom(16)).decode()
            req = ('GET %s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n'
                   'Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n'
                   'Sec-WebSocket-Version: 13\r\n\r\n' % (path, k))
            s.sendall(req.encode())
            socks.append([s, bytearray(), False])
        time.sleep(0.6)
        socks[0][0].sendall(wsframe(1, json.dumps(
            {'type': 'tune', 'frequency': f, 'mode': 'am'}).encode()))
        time.sleep(0.4)
        # Zoomed in, so each one earns its own view channel — the full per-listener cost.
        socks[0][0].sendall(wsframe(1, json.dumps(
            {'type': 'zoom', 'frequency': f, 'binBandwidth': 24000 / 1024}).encode()))
        with lock:
            live[0] += 1
        counted = True
        while not stop:
            for e in socks:
                s, buf, hs = e
                try:
                    d = s.recv(65536)
                except socket.timeout:
                    continue
                except Exception:
                    return
                if not d:
                    return
                buf += d
                if not hs:
                    j = buf.find(b'\r\n\r\n')
                    if j < 0:
                        continue
                    del buf[:j + 4]
                    e[2] = True
                while True:
                    if len(buf) < 2:
                        break
                    op = buf[0] & 0x0f
                    ln = buf[1] & 0x7f
                    off = 2
                    if ln == 126:
                        if len(buf) < 4:
                            break
                        ln = struct.unpack('>H', bytes(buf[2:4]))[0]
                        off = 4
                    elif ln == 127:
                        if len(buf) < 10:
                            break
                        ln = struct.unpack('>Q', bytes(buf[2:10]))[0]
                        off = 10
                    if len(buf) < off + ln:
                        break
                    payload = bytes(buf[off:off + ln])
                    del buf[:off + ln]
                    if op == 9:                      # PING -> PONG, or we are dropped at 20 s
                        try:
                            s.sendall(wsframe(10, payload))
                        except Exception:
                            return
    except Exception:
        pass
    finally:
        if counted:
            with lock:
                live[0] -= 1
        for e in socks:
            try:
                e[0].close()
            except Exception:
                pass


threads = [threading.Thread(target=client, args=(i,), daemon=True) for i in range(N)]
for t in threads:
    t.start()
    time.sleep(0.12)

for _ in range(max(1, DUR // 5)):
    time.sleep(5)
    print('%d still connected' % live[0], flush=True)
stop = True
time.sleep(1)
print('done', flush=True)
