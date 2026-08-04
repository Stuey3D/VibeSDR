# BUG: one stalled listener freezes every listener — blocking sends under a global mutex

**Found 2026-08-04, after it froze the live Pi demo.** Stuart: *"I tried to connect to it whilst
still being connected in the app and both froze."* Cut back immediately (see below); **the
structural fix is NOT done.**

## The mechanism

`sendWs()` in `android/app/src/main/cpp/local_sdr_shim.cpp`:

```cpp
std::lock_guard<std::mutex> lk(sendMtx);   // ONE global lock, ALL clients
if (!sock->isOpen()) return;
sock->send(hdr.data(), hdr.size());        // BLOCKING write
if (len) sock->send(payload, len);         // BLOCKING write
```

Three facts that only bite together:
1. **One global `sendMtx`** serialises every send to every client.
2. **`send()` is a blocking TCP write** — it waits when the peer's window is full.
3. **Most of these calls happen on the DSP THREAD** (`onSpectrum`).

So a single listener that stops reading — a slow link, a client mid-teardown, a paused browser tab —
blocks its own `send()`, holds `sendMtx`, and stops **every other client's** traffic *and the DSP
thread*. Everyone freezes at once, which is exactly what was reported.

★ **`isOpen()` does not protect you.** A socket can be perfectly open and simply not draining.

## Why it fired now

The hazard is older than the change that triggered it — the spectrum broadcast
(`for (auto& pc : peers) sendWs(pc, 0x2, …)`) has the same shape and has always had it. But on
2026-08-03 I added, on the DSP thread:
- `sig` telemetry **to every peer, every frame** (~20 Hz), and
- `rspstat` moved **from the primary socket to every peer**.

That multiplied the number of blocking writes per second through the shared lock by the listener
count, which is what turned a latent hazard into a reproducible freeze the moment a second client
connected. ★ It also explains why the probe on 2026-08-03 found a second listener receiving
nothing while the first was fine.

## What was done immediately (2026-08-04)
- `sig` → **primary socket only, ~5 Hz** (was every peer, every frame).
- `rspstat` → **primary socket only** (was every peer).
★ This only removes the AMPLIFICATION. The bug is still there, and a slow listener can still freeze
the server through the spectrum broadcast alone.

## The real fix (NOT DONE)
Never do a blocking write to a client from the DSP thread, and never behind a lock shared with
other clients. Options, in rough order of preference:
1. **Per-client send queue + writer thread.** Each client drains its own queue; a stalled client
   backs up ITS OWN queue and nobody else notices. Drop policy per stream: spectrum frames and
   telemetry are **droppable** (newest wins), audio is not.
2. **Non-blocking sockets with a bounded buffer** — same drop policy, no extra threads, but every
   call site has to cope with a partial write.
3. At minimum: **per-client mutex** instead of one global one. Stops a stalled client blocking
   OTHERS, but still blocks the DSP thread on that client. Cheap, partial.

★★ **This is what "multi-listener is BUILT BUT UNPROVEN" was hiding.** `--users 20` is advertised
on the demo; until this is fixed, listener 2 can freeze listeners 1..20.
