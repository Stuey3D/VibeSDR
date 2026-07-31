# BRIEF: "idle park" never stops the radio — it only throws the samples away

**Status:** not started. Found 2026-07-31 by Stuart, from an **LED**. Not 10.0.1 (native, and it
touches the crash path); its own piece of work.

## The observation
Stuart, after leaving VibeServer running all day on a Moto G35:
> *"It took an Airspy to discover. When idle the SDR still reports as active. The Airspy has two
> LEDs — orange for plugged in and ready, blue for active/sending IQ data. The Airspy stays BLUE
> whenever plugged in, even with no one connected. I suspect that has something to do with why the
> phone didn't sleep."*

★★★ **Correct, and the code says so.** The HF+ is simply the only one of the three radios with a
light that gives it away.

## ★★★ THE FINDING: park is a DISCARD FLAG, not a stop
```cpp
airspyhf_source.h:109   void setPaused(bool p) { paused_ = p; }
sdrplay_source.h:126    void setPaused(bool p) { paused_ = p; }
local_sdr_shim.cpp      else { idleDiscard.store(false); }   // "never stopped; just start wanting it again"
```
And stated outright in a comment written while fixing something else:
```cpp
// airspyhf_source.h:116 — WHEN THE RADIO LAST DELIVERED, regardless of whether we KEPT the samples.
// The silence watchdog asks "is the radio still producing?", and while idle-parked the answer
// is YES — we are simply throwing the samples away.
```
So with **no listener at all**: the radio streams IQ at full rate, USB runs continuously, the SoC
services every transfer, and we drop every sample. ★ The RTL and RSP do exactly the same; they just
have no LED.

★★ **That is almost certainly why the phone never slept.** Continuous bulk USB transfer plus a
foreground service is about the strongest "stay awake" signal Android has.

---

## ★★ WHY IT WAS BUILT THIS WAY — the fix is not trivial
Two documented constraints pushed it here. Read both before changing anything.

1. ★★★ **Actually stopping the stream is where the libusb ABORT lives.** `pauseCaptureIdle`'s own
   comment: cancelled transfers are still being reaped when the next synchronous control transfer
   runs libusb's event loop, which completes an already-freed transfer, and **libusb ASSERTS** —
   uncatchable, takes the whole server down. *"Armed by an EMPTY server and fired by the next person
   to arrive."* See [[vibeserver_idle_resume_libusb_crash]].
2. ★★★ **The watchdog's liveness test is "is the radio producing?"** Stop the stream and a parked
   radio is indistinguishable from an unplugged one — which produced the **"no radio connected to
   this server" banner over perfectly working audio** (Stuart, 2026-07-27). That is why liveness is
   timed off the SOURCE rather than the sink.

**So a genuine stop needs two things together:**
- teach the watchdog about the parked state, so parked ≠ lost; and
- **close and reopen the device** around the park rather than leaving it streaming.

★★★ The second is already on file as the airtight fix for the libusb race — *"which would also power
the dongle down properly and save more than parking does."* **This finding is that sentence
observed.** One change closes the crash AND the power drain; they are the same fix.

---

## What it costs today
### ★★★ THE RADIO IS THE BATTERY, NOT THE DSP
Stuart: *"the DSP is already extremely light and a very small % of the actual battery usage — the
SDR itself is the biggest drain."* ★★ Which makes this fix **more** valuable than a CPU saving, not
less: an HF+ or an RTL v4 pulls a couple of hundred mA continuously, and on Android the PHONE
supplies it over OTG. We are leaving the single biggest load running while serving nobody.

★★ **Two numbers that are easy to conflate:** the Pi benchmark's core-percentages are about
**CAPACITY** — how many listeners fit. **BATTERY is almost entirely the radio.** Do not reach for
`BRIEF-vibeserver-benchmark.md` figures to reason about runtime.

### ★★★ MEASURED — 10.9 %/hr, and ~80% of it is the dongle
Stuart, Moto G35, **WFM stereo, 2.4 MHz, RTL-SDR**: **10.9 % per hour** ≈ **9 hours** from full.

Arithmetic (★ assumes the 5000 mAh cell, typical RTL draw, and an ESTIMATED boost efficiency — so
treat as an order-of-magnitude, not a measurement):
```
10.9 %/hr × 5000 mAh  ≈ 545 mAh/hr @ ~3.85 V   ≈ 2.1 W total
RTL-SDR v3/v4         ≈ 280–300 mA @ 5 V       ≈ 1.5 W
  … via OTG boost at ~85–90%                   ≈ 1.7 W drawn from the battery
LEAVES                                          < 0.4 W for DSP + Wi-Fi + OS + screen-off
```
★★ So on this measurement **the dongle is roughly 80% of the entire drain** — Stuart's *"the SDR
itself is the biggest drain"* is most of the number, not just the direction.

### ★★★ THEREFORE: AN IDLE SERVER COSTS THE SAME AS A BUSY ONE
Because the radio never stops, a server with **nobody connected** is also burning ~10.9 %/hr. A
phone left serving overnight dies in nine hours whether anyone listens or not.

★★★ **The size of the prize:** if the fix genuinely powers the dongle down when parked, idle falls
from ~2.1 W to whatever the phone alone costs — plausibly under 0.5 W, i.e. **~9 hours → 40+**. That
turns "leave a spare phone as a receiver" from a mains-only proposition into a portable one, which
is the field-kit story in `BRIEF-vibeserver-pi-iso.md`.

### ★★ THE ONE MEASUREMENT THAT WOULD SETTLE IT
**%/hr with the server running and NOBODY connected.** If it comes back near 10.9, the bug is
measured directly instead of inferred, and that single figure justifies the whole piece of work.
★ Re-check [[vibeserver_battery_measured]] against this too — those figures were taken with a
listener, and this brief says the idle case is no cheaper.
- ★ **The Pi field kit** — same code, so a Zero 2 W on a power bank does this too
  (`BRIEF-vibeserver-pi-iso.md`).
- ★★ **Unattended logging** (`BRIEF-rds-logbook.md` §8) genuinely WANTS the radio streaming with
  nobody listening — that is the whole feature. ★ So after this fix, "keep the radio on with no
  listener" becomes a **deliberate choice made when logging is armed**, rather than something that
  happens by accident every time the last listener leaves. That is the right relationship between
  the two: today the accident is doing the feature's job for free, and badly.
- ★ Thermal and wear on a phone left plugged in permanently, which is the recommended setup.

## ★ VERIFY LIKE THIS
The Airspy HF+ LED is the cheapest instrument we have for this whole class of bug — it is ground
truth about whether the USB stream is running, independent of anything the app claims.
**Blue with no client connected = parked but still streaming.** Use it to confirm any fix, and keep
using it: it found this, and nothing in the software would have.
