# BRIEF — VibeServer power saving, background survival, and sharing one radio

**Status:** designed 2026-08-01 with Stuart, from a Discord conversation with Saber/Orchid.
Not started. Companion to `BRIEF-band-spectrogram.md` (the snapshot feature came out of the same
conversation and has its own brief).

---

## 1. The three-way power toggle

Stuart's proposal, with the labels corrected. The SHAPE is right — users want the choice, and
Saber asked for a toggle explicitly. The original REASONS were not (see §2).

| Mode | What it does | Honest description |
|---|---|---|
| **Always on** | Radio never stops | Highest draw (~11%/hr on the Moto). The band is live the instant someone connects. |
| **Band snapshot** | Radio sleeps between listeners, wakes on a cadence to capture | Moderate draw (~4% duty for a full HF sweep). You get a record of band activity while nobody is listening — see `BRIEF-band-spectrogram.md`. |
| **Sleep** | Radio powers down with no listeners | Lowest draw. Wakes when someone connects. |

★★ **"Sleep" DOES NOT EXIST YET.** Idle-park drops samples but never closes the device — the bug
found from the Airspy LED still being lit ([[idle_park_never_stops_radio]]). The power saving this
mode promises is not real until the stop/start fix lands. **The dongle is ~80% of the battery, so
an idle server currently costs the same as a busy one.**

---

## 2. ★★★ The anti-kill myth — DO NOT REPEAT IT

The original pitch was that keeping the radio active stops Android terminating the app. **It does
not, and the labels must not say so.**

VibeServer already runs as a **foreground service** — `foregroundServiceType="connectedDevice"`,
persistent notification, **PARTIAL_WAKE_LOCK**, **WifiLock** (`RtlTcpServerService`). Android does
not kill foreground services for being idle, and it has **no visibility into whether the USB radio
is transferring**. Nothing in the OS is watching the dongle and deciding we are "active".

So "keep the radio spinning so we don't get killed" is a causal link that does not exist. It costs
~80% of the battery and buys nothing. Shipping it as a documented benefit breaks AGENTS.md's
"never offer a control whose every use is a no-op".

### What actually kills a background server
1. **OEM battery managers** — Xiaomi, Samsung, Huawei, OnePlus kill foreground services regardless
   of what they are doing. Needs per-OEM guidance, not a code fix.
2. ★★★ **The missing battery-optimisation exemption.**
   `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` appears **ZERO times in the manifest and zero times in
   the Kotlin.** We have never asked for it. **This is the only real lever and it is not wired
   up.**

★ **Do this one first.** It is small, independent of everything else here, and it is the thing
that actually addresses the reported problem (a remote server dying and being unreachable).
Implementation: a one-time prompt (`ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`) offered when
sharing is switched on, plus `isIgnoringBatteryOptimizations()` surfaced in the sharing screen so
the user can see the state.

---

## 3. Sharing ONE radio with another app (Saber's OpenWebRX setup)

Saber runs OWRX and VibeServer on one Linux box (a chrooted phone), wants them to share one SDR,
whichever connects first wins, with a two-button landing page on port 80.

### ★★ PROVEN ON HARDWARE (Pi 500 + Airspy HF+, 2026-08-01)
A second `vibeserver` started against a radio the first one held:

```
VibeServer: failed to start — could not open the Airspy HF+ (is another program using it?)
VibeServer: Airspy HF+ detected
```

1. **Detection already works.** We ENUMERATE the device (so we know it is present) then fail to
   CLAIM it — exactly how you tell "in use" from "not plugged in".
2. ★★★ **A FAILED CLAIM DOES NOT DISTURB THE INCUMBENT.** Stuart was listening at the time:
   *"audio stayed perfectly"*. This is the safety property the whole design rests on — without it,
   two apps polling for one radio would sabotage each other. It holds because **nothing in our
   tree calls `libusb_reset_device`** (some SDR stacks reset on open, which WOULD kill the
   incumbent's stream).
3. ★ **UNPROVEN ON A DONGLE** — librtlsdr is not vendored in this tree so the RTL path could not be
   checked. Expected to match. **Test with an RTL-SDR before promising it works on any radio.**

### ★★★ The blocker is ours
**VibeServer holds the USB handle for its whole lifetime.** So "whichever you connect to first"
does not apply — if VibeServer is running at all it owns the radio permanently and OWRX never gets
it. OWRX starts and stops its source around client sessions and DOES hand back. **The asymmetry is
ours.** Same stop/start fix again.

### Refinements to build in
- ★★ **Distinguish BUSY from ACCESS.** Our message currently GUESSES ("is another program using
  it?"). On Linux the identical failure comes from a missing udev rule, and sending someone
  hunting for a phantom second app is a miserable hour. libusb reports `LIBUSB_ERROR_BUSY` vs
  `LIBUSB_ERROR_ACCESS` distinctly — say which.
- **Name the RADIO, not the app**: "The Airspy HF+ is in use by another application on this
  device" stays true whether the holder is OWRX, another VibeServer, or a stray `rtl_test`.
- **Retry with backoff, never busy-poll** a device another process holds — the fairness principle
  from [[third_party_receiver_etiquette]] applies locally too.
- **There is NO handover.** First-come-first-served, no queue, no bumping a listener. Say so
  plainly rather than letting users expect a graceful switch.

---

## 4. ★★★ The one fix that gates everything

**Release the radio when idle** — properly close the device, and reopen safely on demand.

It blocks **four** separate things:
1. The resume crash ([[vibeserver_idle_resume_libusb_crash]]) — mitigated, not closed
2. Real "Sleep" power saving (§1)
3. The band spectrogram (`BRIEF-band-spectrogram.md`)
4. Sharing a radio with another app (§3)

★ One piece of work, four payoffs, three of them things users have actually asked for.
★ Design note: **reuse one radio session across a sweep's segments.** Retuning is cheap and safe;
open/close is the dangerous operation — minimise how often it happens.
★ The watchdog recovery added 2026-08-01 (`AirspyHfSource::restartStream`, the widening back-off)
already put machinery around this area and is a sensible foundation to build on.
