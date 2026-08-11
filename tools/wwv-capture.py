#!/usr/bin/env python3
"""Capture live WWV from a US UberSDR, so the time decoder can be tested against the REAL signal.

    python3 tools/wwv-capture.py [--host n7qxb.tunnel.ubersdr.org] [--freq 10000000] [--secs 240]

★★★ WHY THIS EXISTS. WWV is the one time station neither of us can hear from the UK, so its only
    evidence was a SYNTHETIC test — and this project has already been burned by exactly that: MSF
    decoded a confident "2064-02-22" and the synthetic test AGREED, because both were written from
    the same wrong assumption about bit order. A test built on the author's misunderstanding proves
    only that the two halves match each other. Stuart, 2026-08-11: "to test WWV use a US based
    UberSDR server, then you can test it against the live signal as a user would."

★★ It captures OPUS PACKETS VERBATIM, not decoded audio. UberSDR offers no PCM format (pcm/s16/raw
   are all refused — probed), so the decode happens in wwv-decode.cpp against libopus. Keeping the
   packets means one capture can be replayed through the decoder as many times as the framing takes
   to get right, WITHOUT going back to somebody else's receiver for each attempt. That is the whole
   etiquette argument as well as the practical one.

★ File format: 16-byte header 'WWVCAP01' + uint32 sample rate + uint32 packet count, then each
  packet as uint32 length + bytes. The UTC of the first packet goes in the sidecar .json — the
  decoded minute is meaningless without knowing what it SHOULD have said.
"""
import argparse, asyncio, json, struct, sys, time, uuid, urllib.request
import websockets

# ★ Identify honestly. We are a guest on somebody else's receiver; see the etiquette rules — a
#   real UA is what lets an owner tell a well-behaved client from a scraper.
UA = "VibeSDR/10 (time-decoder harness; https://vibesdr.net)"


async def capture(host: str, freq: int, secs: int, out: str) -> int:
    sid = str(uuid.uuid4())
    req = urllib.request.Request(
        f"https://{host}/connection", method="POST",
        data=json.dumps({"user_session_id": sid}).encode(),
        headers={"Content-Type": "application/json", "User-Agent": UA})
    with urllib.request.urlopen(req, timeout=20) as r:
        print("connection:", r.read().decode()[:200])

    url = (f"wss://{host}/ws?user_session_id={sid}&frequency={freq}"
           f"&mode=am&format=opus&version=2")
    packets, started, rate_seen = [], None, None
    async with websockets.connect(url, max_size=None, ping_interval=None,
                                  additional_headers={"User-Agent": UA}) as ws:
        deadline = time.time() + secs + 10
        while time.time() < deadline:
            try:
                m = await asyncio.wait_for(ws.recv(), timeout=10)
            except asyncio.TimeoutError:
                break
            if not isinstance(m, (bytes, bytearray)):
                continue
            if started is None:
                # ★ The clock is read at the FIRST AUDIO PACKET, not at connect: the handshake and
                #   the server's own buffering sit between them, and a second of error here would
                #   be blamed on the decoder.
                started = time.time()
                deadline = started + secs
            packets.append(bytes(m))
            if rate_seen is None and len(m) >= 13:
                # ★ The stream states its own sample rate in every frame's header — 24 kHz here,
                #   not the 48 kHz an Opus decoder defaults to. Record what the SERVER said.
                rate_seen = int.from_bytes(m[8:12], "little")

    if not packets:
        print("no audio received", file=sys.stderr)
        return 1
    with open(out, "wb") as f:
        f.write(b"WWVCAP01" + struct.pack("<II", rate_seen or 48000, len(packets)))
        for p in packets:
            f.write(struct.pack("<I", len(p)) + p)
    meta = {"host": host, "frequency": freq, "packets": len(packets), "sample_rate": rate_seen,
            "started_unix": started, "started_utc": time.strftime("%Y-%m-%d %H:%M:%S",
                                                                  time.gmtime(started)),
            "seconds": round(time.time() - started, 1)}
    with open(out + ".json", "w") as f:
        json.dump(meta, f, indent=1)
    print(f"wrote {out}  ({len(packets)} packets, {meta['seconds']}s, started {meta['started_utc']} UTC)")
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    # ★ Holladay, Utah by default — about 600 km from WWV's Fort Collins transmitter, which is
    #   close enough for a solid daytime signal on 10 MHz and far enough to be a real path.
    ap.add_argument("--host", default="n7qxb.tunnel.ubersdr.org")
    ap.add_argument("--freq", type=int, default=10_000_000)
    ap.add_argument("--secs", type=int, default=240)
    ap.add_argument("--out", default="/tmp/wwv-capture.bin")
    a = ap.parse_args()
    sys.exit(asyncio.run(capture(a.host, a.freq, a.secs, a.out)))
