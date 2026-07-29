#!/usr/bin/env python3
"""Capture OWRX WFM audio at 48k and 24k hd_output_rate, decode, and write WAVs for an A/B listen.

The ADPCM decoder is a port of spike/WristSDR/WristSDR/ImaAdpcm.swift, 'owrx' flavour:
index adjusts FIRST, diff uses the step LATCHED at the end of the previous nibble.
Getting that wrong produces plausible-sounding noise rather than an obvious failure, which is
exactly why it is ported rather than reinvented.
"""
import asyncio, json, sys, time, wave
import websockets

HOST = "192.168.86.13:8073"
FREQ = float(sys.argv[1]) if len(sys.argv) > 1 else 97_400_000   # a strong local station
SECS = int(sys.argv[2]) if len(sys.argv) > 2 else 12
OUT = "/private/tmp/claude-501/-Users-stuey3d-VibeSDR/7b7e24cd-79a9-47ac-92d6-74a06c52d572/scratchpad"

STEP = [7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,
        130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
        1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
        5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
        27086,29794,32767]
IDX = [-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8]


class Adpcm:                      # 'owrx' flavour — see ImaAdpcm.swift
    def __init__(self): self.i = 0; self.p = 0; self.s = 0
    def nib(self, n):
        self.i = max(0, min(88, self.i + IDX[n]))
        s = self.s
        d = s >> 3
        if n & 1: d += s >> 2
        if n & 2: d += s >> 1
        if n & 4: d += s
        if n & 8: d = -d
        self.p = max(-32768, min(32767, self.p + d))
        self.s = STEP[self.i]
        return self.p
    def decode(self, b):
        out = bytearray()
        for x in b:
            for v in (self.nib(x & 0x0f), self.nib((x >> 4) & 0x0f)):
                out += int(v).to_bytes(2, 'little', signed=True)
        return bytes(out)


async def grab(hd_rate):
    async with websockets.connect(f"ws://{HOST}/ws/", origin=f"http://{HOST}",
                                  max_size=None, ping_interval=None) as ws:
        await ws.send("SERVER DE CLIENT client=vibesdr-ab type=receiver")
        dec, pcm, started, sel, bytes_in = Adpcm(), bytearray(), None, False, 0
        tags = {}
        end = time.time() + 60
        while time.time() < end:
            try:
                m = await asyncio.wait_for(ws.recv(), timeout=8)
            except asyncio.TimeoutError:
                break
            if isinstance(m, str):
                if m.startswith("CLIENT DE SERVER"):
                    await ws.send(json.dumps({"type": "connectionproperties",
                                              "params": {"output_rate": 12000,
                                                         "hd_output_rate": hd_rate}}))
                    await ws.send(json.dumps({"type": "dspcontrol", "action": "start"}))
                    continue
                try: j = json.loads(m)
                except Exception: continue
                if j.get("type") == "profiles" and not sel:
                    for p in j["value"]:
                        if "fm broadcast" in p["name"].lower() and "middle" in p["name"].lower():
                            await ws.send(json.dumps({"type": "selectprofile",
                                                      "params": {"profile": p["id"]}}))
                            sel = True
                            print(f"  profile: {p['name']}")
                            break
                elif j.get("type") == "config" and sel and started is None:
                    c = j.get("value", {})
                    if c.get("center_freq"):
                        off = int(FREQ - c["center_freq"])
                        await ws.send(json.dumps({"type": "dspcontrol", "params": {
                            "offset_freq": off, "mod": "wfm", "squelch_level": -150,
                            "secondary_mod": False, "low_cut": -100000, "high_cut": 100000}}))
                        started = time.time() + 3          # let the demod settle
                        end = started + SECS
            else:
                if started is None or time.time() < started:
                    continue
                if m and m[0] in (2, 4):                   # audio on EITHER channel
                    tags[m[0]] = tags.get(m[0], 0) + len(m)
                    bytes_in += len(m)
                    pcm += dec.decode(m[1:])
        return pcm, bytes_in, tags


async def main():
    for rate in (48000, 24000, 12000):
        print(f"\n--- hd_output_rate {rate} ---")
        pcm, raw, tags = await grab(rate)
        secs = max(1, SECS)
        path = f"{OUT}/wfm_{rate}.wav"
        with wave.open(path, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
            w.writeframes(pcm)
        print(f"  wire: {raw/secs/1024:5.1f} KB/s ({raw/secs*8/1000:5.1f} kbps)   "
              f"decoded {len(pcm)//2} samples -> {path}")
        print(f"  tags: {{k: round(v/secs/1024,1) for k,v in tags.items()}} KB/s  (2=narrow 12k, 4=HD)")

asyncio.run(main())
