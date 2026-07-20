#!/usr/bin/env python3
"""Measure what an OpenWebRX profile actually pushes at a client, split by stream.

Replicates the handshake in spike/WristSDR/WristSDR/OwrxClient.swift so the numbers are what
VibeSDR Jr would really receive — same connectionproperties, same dspcontrol start.

Binary frame tags (OwrxClient.onBinary):
    1 = FFT / waterfall     2 = audio 12k      4 = audio 48k (HD)

Why this matters: the Apple Watch Bluetooth relay carries ~25-62 KB/s. Knowing the SPLIT tells us
whether to attack the waterfall or the audio — and OWRX ignores fft_fps/fft_size, so if the FFT
dominates there is no lever at all.
"""
import asyncio, json, sys, time
import websockets

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.86.13:8073"
WANT = (sys.argv[2] if len(sys.argv) > 2 else "sdl").lower()
SECS = int(sys.argv[3]) if len(sys.argv) > 3 else 20


async def main():
    url = f"ws://{HOST}/ws/"
    async with websockets.connect(url, origin=f"http://{HOST}", max_size=None) as ws:
        await ws.send("SERVER DE CLIENT client=vibesdr-measure type=receiver")

        totals = {1: 0, 2: 0, 4: 0, "text": 0, "other": 0}
        counts = {1: 0, 2: 0, 4: 0}
        profiles, chosen, started = {}, None, None
        t_end = time.time() + SECS + 12

        while time.time() < t_end:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=6)
            except asyncio.TimeoutError:
                break

            if isinstance(msg, str):
                totals["text"] += len(msg)
                if msg.startswith("CLIENT DE SERVER"):
                    await ws.send(json.dumps({"type": "connectionproperties",
                                              "params": {"output_rate": 12000,
                                                         "hd_output_rate": 48000}}))
                    await ws.send(json.dumps({"type": "dspcontrol", "action": "start"}))
                    continue
                try:
                    j = json.loads(msg)
                except Exception:
                    continue
                if j.get("type") == "profiles":
                    for p in j.get("value", []):
                        profiles[p["id"]] = p["name"]
                    for pid, name in profiles.items():
                        if WANT in name.lower():
                            chosen = (pid, name); break
                    if chosen:
                        print(f"→ selecting profile: {chosen[1]}")
                        await ws.send(json.dumps({"type": "selectprofile",
                                                  "params": {"profile": chosen[0]}}))
                        await ws.send(json.dumps({"type": "dspcontrol", "action": "start"}))
                        # measure only AFTER the profile is running
                        started = time.time() + 3
                        t_end = started + SECS
                    else:
                        print("profiles offered:")
                        for pid, n in profiles.items():
                            print("   ", n)
                        return
                elif j.get("type") == "config":
                    c = j.get("value", {})
                    interesting = {k: c[k] for k in
                                   ("fft_size", "fft_fps", "fft_compression", "audio_compression",
                                    "samp_rate", "start_mod", "start_offset_freq")
                                   if k in c}
                    if interesting:
                        print("   config:", interesting)
            else:
                if started and time.time() < started:
                    continue                      # discard the profile-switch burst
                tag = msg[0] if msg else 0
                if tag in totals:
                    totals[tag] += len(msg); counts[tag] += 1
                else:
                    totals["other"] += len(msg)

        span = SECS
        print(f"\n=== {chosen[1] if chosen else HOST} — {span}s ===")
        fft, a12, a48 = totals[1] / span, totals[2] / span, totals[4] / span
        for label, b, n in (("FFT / waterfall", fft, counts[1]),
                            ("audio 12k", a12, counts[2]),
                            ("audio 48k (HD)", a48, counts[4]),
                            ("text/JSON", totals["text"] / span, 0)):
            if b:
                rate = f"{b/1024:7.1f} KB/s  ({b*8/1000:6.1f} kbps)"
                print(f"  {label:16} {rate}  {n/span:5.1f} frames/s" if n else f"  {label:16} {rate}")
        total = sum(totals[k] for k in (1, 2, 4)) + totals["text"] + totals["other"]
        print(f"  {'TOTAL':16} {total/span/1024:7.1f} KB/s  ({total/span*8/1000:6.1f} kbps)")
        print(f"\n  Apple Watch BT relay ceiling ~25-62 KB/s  →  "
              f"{'OVER BUDGET' if total/span/1024 > 25 else 'within budget'}")


asyncio.run(main())
