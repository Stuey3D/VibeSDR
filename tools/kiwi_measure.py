import asyncio, json, time, websockets
HOST="oh1ct.sytes.net:8073"
UA="Mozilla/5.0 (iPhone; CPU iPhone OS 18_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1"
SECS=15
async def stream(kind, secs):
    ts=int(time.time()*1000)   # MILLIseconds, as KiwiClient uses
    url=f"ws://{HOST}/ws/kiwi/{ts}/{kind}"
    b=n=0; t0=None
    try:
        async with websockets.connect(url, max_size=None, ping_interval=None,
                additional_headers={"User-Agent":UA,"Origin":f"http://{HOST}"}) as ws:
            await ws.send("SET auth t=kiwi p=")
            await ws.send("SET ident_user=VibeSDR Jr")
            await ws.send(f"SERVER DE CLIENT openwebrx.js {kind}")
            if kind=="SND":
                await ws.send("SET AR OK in=12000 out=44100")
                await ws.send("SET mod=am low_cut=-4900 high_cut=4900 freq=198.000")
            else:
                await ws.send("SET send_dB=1")
                await ws.send("SET zoom=0 start=0")
                await ws.send("SET wf_speed=4")
            end=time.time()+secs+6
            while time.time()<end:
                try: m=await asyncio.wait_for(ws.recv(), timeout=8)
                except asyncio.TimeoutError: break
                if isinstance(m,(bytes,bytearray)):
                    if t0 is None: t0=time.time(); end=t0+secs; continue
                    b+=len(m); n+=1
    except Exception as e:
        print(f"  {kind}: {e}")
    return b/secs, n/secs
async def main():
    (ab,an) = await stream("SND", SECS)
    (wb,wn) = await stream("W/F", SECS)
    print(f"\n=== KiwiSDR {HOST} — AM 198 kHz, {SECS}s each ===")
    print(f"  audio (ADPCM 12k) {ab/1024:6.1f} KB/s ({ab*8/1000:6.1f} kbps)  {an:5.1f} fr/s")
    print(f"  waterfall         {wb/1024:6.1f} KB/s ({wb*8/1000:6.1f} kbps)  {wn:5.1f} fr/s")
    T=ab+wb
    print(f"  TOTAL             {T/1024:6.1f} KB/s ({T*8/1000:6.1f} kbps)")
asyncio.run(main())
