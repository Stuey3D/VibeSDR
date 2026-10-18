#!/usr/bin/env python3
# Headless listener: opens /ws/user-spectrum (binary8) + /ws/audio (opus), tunes WFM, counts spectrum frames and
# audio frames per second over a window after a warm-up. Silent — nothing is played. Prints one JSON line.
# usage: listener.py BASE_URL FREQ_HZ WARM_S MEASURE_S [bins] [fps]
import asyncio, json, sys, time, random, urllib.request
import websockets
base, freq, warm, meas = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
bins = int(sys.argv[5]) if len(sys.argv) > 5 else 1024
fps = int(sys.argv[6]) if len(sys.argv) > 6 else 20
sid = 'atom-%08x' % random.getrandbits(32)
ws = base.replace('http', 'ws', 1)
async def main():
    spec_t, aud_t, aud_b = [], [], [0]
    async with websockets.connect(f'{ws}/ws/user-spectrum?user_session_id={sid}&mode=binary8&bins={bins}', max_size=None) as s:
        await s.send(json.dumps({'type': 'tune', 'frequency': freq, 'mode': 'wfm'}))
        await s.send(json.dumps({'type': 'fftRate', 'value': fps}))
        async with websockets.connect(f'{ws}/ws/audio?user_session_id={sid}&codec=opus', max_size=None) as a:
            async def rs():
                async for m in s:
                    if isinstance(m, bytes): spec_t.append(time.monotonic())
            async def ra():
                async for m in a:
                    aud_t.append(time.monotonic()); aud_b[0] += len(m) if isinstance(m, bytes) else 0
            ts = [asyncio.create_task(rs()), asyncio.create_task(ra())]
            t0 = time.monotonic(); await asyncio.sleep(warm)
            t1 = time.monotonic(); await asyncio.sleep(meas); t2 = time.monotonic()
            for t in ts: t.cancel()
    sp = [t for t in spec_t if t1 <= t < t2]; au = [t for t in aud_t if t1 <= t < t2]
    gaps = [b - a for a, b in zip(au, au[1:])]
    try: info = json.load(urllib.request.urlopen(base + '/vibeserver.json', timeout=3))
    except Exception: info = {}
    print(json.dumps({'specFps': round(len(sp) / (t2 - t1), 2), 'audioFrames': len(au),
                      'audioWorstGapMs': round(1000 * max(gaps), 1) if gaps else None,
                      'audioGaps150': sum(1 for g in gaps if g > 0.15),
                      'dspCpu': info.get('dspCpu'), 'iqDrops': info.get('iqDrops'), 'fpsServer': info.get('fps')}))
asyncio.run(main())
