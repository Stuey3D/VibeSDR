import asyncio, time, websockets, sys
HOST=sys.argv[1] if len(sys.argv)>1 else "85.183.11.108:8074"
UA="Mozilla/5.0 (iPhone; CPU iPhone OS 18_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1"
async def wf(speed, secs=20, settle=6):
    ts=int(time.time()*1000)
    b=n=0; t0=None; setup=None
    try:
        async with websockets.connect(f"ws://{HOST}/ws/kiwi/{ts}/W/F", max_size=None,
                ping_interval=None, additional_headers={"User-Agent":UA,"Origin":f"http://{HOST}"}) as ws:
            await ws.send("SET auth t=kiwi p=")
            await ws.send("SERVER DE CLIENT openwebrx.js W/F")
            await ws.send("SET send_dB=1")
            await ws.send("SET zoom=0 start=0")
            await ws.send("SET maxdb=0 mindb=-100")
            await ws.send(f"SET wf_speed={speed}")
            t_settle=time.time()+settle
            async def ka():
                while True:
                    await asyncio.sleep(1)
                    try: await ws.send("SET keepalive")
                    except Exception: return
            kt=asyncio.create_task(ka())
            end=t_settle+secs+8
            while time.time()<end:
                try: m=await asyncio.wait_for(ws.recv(),timeout=8)
                except asyncio.TimeoutError: break
                if isinstance(m,str):
                    if "wf_setup" in m or "wf_fps" in m: setup=m.strip()[:120]
                    continue
                if time.time() < t_settle: continue          # discard the connect burst
                if t0 is None: t0=time.time(); end=t0+secs; continue
                b+=len(m); n+=1
    except Exception as e:
        return None, None, f"DROPPED: {type(e).__name__}"
    return b/secs, n/secs, setup
async def main():
    print(f"KiwiSDR {HOST} — waterfall bytes by wf_speed (20s each, 6s settle)\n")
    for sp,label in ((4,"4 fast"),(3,"3 med"),(2,"2 slow"),(1,"1 Hz")):
        kb,fps,note = await wf(sp)
        if kb is None: print(f"  wf_speed={label:8} {note}")
        else: print(f"  wf_speed={label:8} {kb/1024:5.1f} KB/s ({kb*8/1000:5.1f} kbps)  {fps:5.1f} fr/s   {note or ''}")
asyncio.run(main())
