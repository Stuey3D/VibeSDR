# Capture RAW PCM from a listener and measure whether the audio is CLEAN — run on the server, where
# loopback is exempt from the uncompressed-audio policy, so we judge the demodulated waveform and
# not a codec's opinion of it.
#
# ★★★ WHAT IT LOOKS FOR. A stale phase reference in the channelizer puts a discontinuity at every
#     block boundary — fs/hop, a fixed 325 Hz at 8 MSPS. That is not "a bit of noise": it is a COMB
#     of spurs at the block rate and its harmonics, and it is audible as harshness/breakup. So
#     rather than eyeballing an RMS, this measures the energy sitting exactly at the block rate
#     against its neighbourhood. Clean audio has nothing there.
#
#   python3 audioprobe.py <port> <freq_hz> [mode]
import socket, base64, os, struct, json, time, sys, math, cmath

PORT  = int(sys.argv[2]) if len(sys.argv) > 2 else 48000
FREQ  = int(sys.argv[3]) if len(sys.argv) > 3 else 7074000
MODE  = sys.argv[4] if len(sys.argv) > 4 else 'usb'
RATE  = 8000000.0                     # capture rate, for the block-rate maths
FFTN  = 32768
HOP   = FFTN - FFTN // 4
BLOCK_HZ = RATE / HOP                 # ~325 Hz at 8 MSPS
SR    = 48000.0


def wsframe(op, payload=b''):
    h = bytearray([0x80 | op])
    if len(payload) < 126:
        h.append(0x80 | len(payload))
    else:
        h.append(0x80 | 126)
        h += struct.pack('>H', len(payload))
    h += b'\x00\x00\x00\x00'
    return bytes(h) + payload


def connect(path):
    s = socket.create_connection(('127.0.0.1', PORT), 5)
    s.settimeout(2.0)
    k = base64.b64encode(os.urandom(16)).decode()
    s.sendall(('GET %s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n'
               'Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n'
               'Sec-WebSocket-Version: 13\r\n\r\n' % (path, k)).encode())
    return [s, bytearray(), False]


def pump(e, sink=None):
    s, buf, hs = e
    try:
        d = s.recv(65536)
    except socket.timeout:
        return
    except Exception:
        return
    if not d:
        return
    buf += d
    if not hs:
        j = buf.find(b'\r\n\r\n')
        if j < 0:
            return
        del buf[:j + 4]
        e[2] = True
    while True:
        if len(buf) < 2:
            return
        op = buf[0] & 0x0f
        ln = buf[1] & 0x7f
        off = 2
        if ln == 126:
            if len(buf) < 4: return
            ln = struct.unpack('>H', bytes(buf[2:4]))[0]; off = 4
        elif ln == 127:
            if len(buf) < 10: return
            ln = struct.unpack('>Q', bytes(buf[2:10]))[0]; off = 10
        if len(buf) < off + ln:
            return
        p = bytes(buf[off:off + ln]); del buf[:off + ln]
        if op == 9:
            s.sendall(wsframe(10, p))
        elif op == 2 and sink is not None and len(p) > 6 and p[1] == 0:
            for i in range(6, len(p) - 1, 2):
                sink.append(struct.unpack_from('<h', p, i)[0])


spec = connect('/ws/user-spectrum?user_session_id=aprobe&bins=1024')
time.sleep(0.4)
aud = connect('/ws/audio?user_session_id=aprobe')
time.sleep(0.5)
spec[0].sendall(wsframe(1, json.dumps({'type': 'tune', 'frequency': FREQ, 'mode': MODE}).encode()))

pcm = []
t_end = time.time() + 3.0
while time.time() < t_end:                 # settle
    pump(spec); pump(aud)
pcm = []
t_end = time.time() + 4.0
while time.time() < t_end:
    pump(spec); pump(aud, pcm)

if len(pcm) < 20000:
    print('  %.1f kHz %s: NOT ENOUGH AUDIO (%d samples)' % (FREQ / 1e3, MODE, len(pcm)))
    raise SystemExit(1)

n = min(len(pcm), 96000)
x = pcm[:n]
mean = sum(x) / n
x = [v - mean for v in x]
rms = math.sqrt(sum(v * v for v in x) / n)
peak = max(abs(v) for v in x)
clip = sum(1 for v in x if abs(v) >= 32700) / n * 100.0


def goertzel(sig, freq, sr):
    """Energy at one frequency — cheaper than a whole FFT and all we need."""
    k = 2.0 * math.pi * freq / sr
    c, s = math.cos(k), math.sin(k)
    re = im = 0.0
    for i, v in enumerate(sig):
        re += v * math.cos(k * i)
        im -= v * math.sin(k * i)
    return math.sqrt(re * re + im * im) / len(sig)


# Energy AT the block rate and its harmonics, against a neighbourhood that should look the same
# if nothing special is happening there.
combs, refs = [], []
for h in (1, 2, 3):
    f = BLOCK_HZ * h
    if f > SR / 2 - 100:
        break
    combs.append(goertzel(x, f, SR))
    refs.append((goertzel(x, f + 37, SR) + goertzel(x, f - 37, SR)) / 2.0)
comb = sum(combs) / max(1, len(combs))
ref = sum(refs) / max(1, len(refs))
ratio_db = 20.0 * math.log10((comb + 1e-9) / (ref + 1e-9))

print('  %.1f kHz %-4s  RMS %6.0f  peak %6d  clipped %5.2f%%  '
      'block-rate comb %+5.1f dB vs neighbours  %s'
      % (FREQ / 1e3, MODE, rms, peak, clip, ratio_db,
         'CLEAN' if ratio_db < 6.0 else 'COMB PRESENT'))
