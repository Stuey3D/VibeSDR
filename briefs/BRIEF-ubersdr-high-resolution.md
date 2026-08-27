# BRIEF — surviving UberSDR at 2048 bins / 20 fps

**Written 2026-08-18**, the day UberSDR 0.1.59 made resolution and update rate an
instance setting. Nobody has turned it up yet. When an owner does, we should already
work — the alternative is a user reporting "your app broke on this server" and us
finding out then.

## What the server actually offers

Measured against a live instance (Stuart's, 1024 bins @ 10 fps, "Medium/Medium"):

| Session | Rate | Throughput |
|---|---|---|
| Shared / not zoomed | 5.3 fps | **1.1 KB/s** — cheap, and `set_rate` is IGNORED here |
| Private (zoomed) | 10.3 fps | **15.3 KB/s** |
| Private + `set_rate` divisor 2 | 5.3 fps | **7.2 KB/s** |
| Private + `set_rate` divisor 3 | 3.7 fps | **4.9 KB/s** |

Settings range 512–2048 bins and 5–20 fps, so the worst case is **4× today's**:
2048 @ 20 fps ≈ **60 KB/s ≈ 480 kbps** on a private channel at divisor 1.

## The three fixed facts

1. ★★★ **BIN COUNT CANNOT BE REQUESTED.** MadPsy, 2026-08-18: *"you can certainly
   still set the divider for the update rate, bin count is fixed though."* Confirmed
   by measurement: `&bins=128` and `&fps=5` on the URL change nothing at all.
2. ★★★ **`set_rate` ONLY BITES ON A PRIVATE SESSION** — the shared default channel is
   hardcoded to every 2nd tick and ignores us. A private channel is what a ZOOM
   creates, so the divisor is unavailable until the client has zoomed.
3. ★★★ **THE CONFIG DOES NOT DECLARE THE RATE.** It carries `binCount`,
   `binBandwidth`, `centerFreq`, `totalBandwidth` and the defaults — no fps field.
   So a client cannot read the server's rate; **it must measure it.**

## The work

### 1. Target a FRAME RATE, not a divisor rung  (both clients)
Jr's `LinkManager(ladder: [10, 5, 10.0/3.0])` and the phone's `LADDERS.ubersdr`
encode "divisor 1/2/3 = 10/5/3.3 fps" — true only on a 10 fps server. On a 20 fps
instance divisor 2 gives **10 fps, not 5**, and the client believes it has throttled
when it has not.

Replace the absolute ladder with a target and derive the divisor from the MEASURED
base rate: `divisor = clamp(round(observedBaseFps / targetFps), 1, 8)`.
Measure the base BEFORE applying a divisor, and re-measure after a reconnect — the
instance may have been reconfigured while we were away.

### 2. Re-assert the divisor after every subscribe  (Jr, verify the phone)
The client already warns that a zoom can silently take the rate away, because a zoom
may move the session between the shared and a private channel. Anything that
re-subscribes must re-send `set_rate`.

### 3. Targets
- **Jr**: 5 fps, and never above ~12 KB/s. The iPhone Bluetooth relay is the binding
  constraint and it is not negotiable. At 2048 bins that is divisor 4 on a 20 fps
  instance.
- **Phone on cellular**: 10 fps is comfortable; Wi-Fi can take the server's full rate.

### 4. Render cost at 2048 bins
Jr decimates every row to `WaterfallBuffer.width` on the watch's CPU, so twice the
bins is twice that cost per frame. Measure before assuming it is free — 2048 @ 5 fps
is HALF today's per-second cost, so the target rate probably pays for it, but it
should be a measurement rather than an argument.

### 5. Test bed
Set a real instance to High/Fast temporarily. Nothing else reproduces it, and today
proved that reasoning about this protocol without a live capture goes wrong fast.

## Related
- The delta-only encoder that blacked out Jr's waterfall (`5a83d76d`) — the same
  server release. A capture is worth more than a theory.
