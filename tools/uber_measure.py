import asyncio, json, time, uuid, websockets, urllib.request
HOST="stuey3d.tunnel.ubersdr.org"
SID=str(uuid.uuid4())
FREQ=198000; MODE="am"      # Radio 4 LW — always on, and a fair mid-band case

req=urllib.request.Request(f"https://{HOST}/connection", method="POST",
    data=json.dumps({"user_session_id":SID}).encode(),
    headers={"Content-Type":"application/json","User-Agent":"VibeSDR Jr/1.0 (watchOS)"})
print("connection:", json.loads(urllib.request.urlopen(req, timeout=15).read().decode()))

async def meter(url, tag, secs, sub=None):
    n=b=0; t0=None
    try:
        async with websockets.connect(url, max_size=None, ping_interval=None,
                                      additional_headers={"User-Agent":"VibeSDR Jr/1.0 (watchOS)"}) as ws:
            if sub: await ws.send(json.dumps(sub))
            end=time.time()+secs+6
            while time.time()<end:
                try: m=await asyncio.wait_for(ws.recv(), timeout=8)
                except asyncio.TimeoutError: break
                if t0 is None: t0=time.time(); end=t0+secs; continue   # skip the first frame
                if isinstance(m,(bytes,bytearray)): b+=len(m); n+=1
    except Exception as e:
        print(f"  {tag}: {e}"); return 0,0
    return b/secs, n/secs

async def main():
    spec=f"wss://{HOST}/ws/user-spectrum?user_session_id={SID}&mode=binary8"
    aud=f"wss://{HOST}/ws?user_session_id={SID}&frequency={FREQ}&mode={MODE}&format=opus&version=2"
    (sb,sn),(ab,an) = await asyncio.gather(meter(spec,"spectrum",15,{"type":"zoom","frequency":198000,"binBandwidth":100.0}), meter(aud,"audio",15))
    print(f"\n=== UberSDR {HOST} — {MODE} @ {FREQ/1000:.0f} kHz, 15s ===")
    print(f"  spectrum   {sb/1024:6.1f} KB/s ({sb*8/1000:6.1f} kbps)  {sn:5.1f} fr/s")
    print(f"  audio opus {ab/1024:6.1f} KB/s ({ab*8/1000:6.1f} kbps)  {an:5.1f} fr/s")
    T=sb+ab
    print(f"  TOTAL      {T/1024:6.1f} KB/s ({T*8/1000:6.1f} kbps)")
    print(f"\n  BT relay ceiling ~25-62 KB/s → {'OVER' if T/1024>25 else 'COMFORTABLE'}")
asyncio.run(main())
