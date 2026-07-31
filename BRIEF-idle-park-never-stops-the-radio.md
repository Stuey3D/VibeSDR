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

### ★★ CORROBORATED BY THERMALS — INDEPENDENT OF THE ARITHMETIC
Stuart: *"the phone doesn't get warm."* ★★ A warm phone means the SoC is working; a COOL phone
running a server means the power is leaving through the **USB port**, not being burnt in the chip.
So the thermals reach the same conclusion as the sums above by a completely different route — and it
matches every CPU measurement we hold ([[jr_cpu_measured_2026_07_29]], the Pi bench), where the DSP
has consistently come out cheaper than expected.
★ It also means there is **no thermal argument** against leaving a phone plugged in permanently. The
limiting factor is the dongle's constant draw, not heat.

### ★★★ THEREFORE: AN IDLE SERVER COSTS THE SAME AS A BUSY ONE
Because the radio never stops, a server with **nobody connected** is also burning ~10.9 %/hr. A
phone left serving overnight dies in nine hours whether anyone listens or not.

★★★ **The size of the prize:** if the fix genuinely powers the dongle down when parked, idle falls
from ~2.1 W to whatever the phone alone costs — plausibly under 0.5 W, i.e. **~9 hours → 40+**. That
turns "leave a spare phone as a receiver" from a mains-only proposition into a portable one, which
is the field-kit story in `BRIEF-vibeserver-pi-iso.md`.

### Who else this hits
- ★ **The Pi field kit** — same code, so a Zero 2 W on a power bank does this too
  (`BRIEF-vibeserver-pi-iso.md`), and there the whole point is running off a battery.
- ★★ **Unattended logging** (`BRIEF-rds-logbook.md` §8) genuinely WANTS the radio streaming with
  nobody listening — that is the whole feature. ★ So after this fix, "keep the radio on with no
  listener" becomes a **deliberate choice made when logging is armed**, rather than something that
  happens by accident every time the last listener leaves. That is the right relationship between
  the two: today the accident is doing the feature's job for free, and badly.

### ★★ THE ONE MEASUREMENT THAT WOULD SETTLE IT
**%/hr with the server running and NOBODY connected.** If it comes back near 10.9, the bug is
measured directly instead of inferred, and that single figure justifies the whole piece of work.
★ Re-check [[vibeserver_battery_measured]] against this too — those figures were taken with a
listener, and this brief says the idle case is no cheaper.

## ★ VERIFY LIKE THIS
The Airspy HF+ LED is the cheapest instrument we have for this whole class of bug — it is ground
truth about whether the USB stream is running, independent of anything the app claims.
**Blue with no client connected = parked but still streaming.** Use it to confirm any fix, and keep
using it: it found this, and nothing in the software would have.

---

# ★★★ THE COUNTER-RISK: DOES POWERING DOWN LET ANDROID SLEEP ON US?
Stuart, 2026-07-31: *"if we power down too much, will Android then put us to sleep and prevent us
waking up from a network probe?"* and *"jetsam happens on low-RAM devices because it is detected as
sleeping."*

★★ **The right worry — because today the BUG is doing the job of a wakelock.** Continuous USB
traffic is part of what keeps the phone responsive, so removing it removes something that
accidentally works.

## 1. ✅ The wake path is ALREADY EXPLICIT — this part is safe
```kotlin
// RtlTcpServerService.kt:100-101 — VibeServer's start path calls this service
acquireWakeLock()      // PARTIAL_WAKE_LOCK
wifiLock.acquire()     // WIFI_MODE_FULL_LOW_LATENCY (HIGH_PERF below the API level)
```
The CPU is held awake and Wi-Fi is held out of power-save **by design**, not by USB traffic. Those
locks answer the network probe and they would still be held. ★ Manifest carries `WAKE_LOCK` and
`CHANGE_WIFI_MULTICAST_STATE` (the latter matters for mDNS/`vibesdr.local`).

## 2. ★★★ THE SHARP EDGE: `foregroundServiceType="connectedDevice"`
```xml
<service android:name=".RtlTcpServerService" android:foregroundServiceType="connectedDevice" .../>
```
From **Android 14** the platform enforces that a foreground service type's preconditions actually
hold. ★★★ **If "park" means CLOSING the USB device, we may stop qualifying for the very service type
keeping us alive** — and the result is precisely Stuart's fear: the service torn down on a low-RAM
device that has decided we are doing nothing.

★★ **CHECK THIS AGAINST THE VERSIONED DOCS BEFORE WRITING ANY OF IT.** It could invalidate the
close/reopen approach on Android specifically, while leaving it correct on macOS/Pi.

## 3. ★★ THEREFORE THE MIDDLE OPTION — STOP STREAMING, KEEP THE HANDLE
The RTL's draw is dominated by the **R820T tuner and the ADC**, so halting the stream recovers most
of the power **without** releasing the USB claim, **without** changing FGS eligibility, and
**without** the full close/reopen that the libusb race lives in.
- Less saving than a true power-down; far less risk.
- ★ **Verifiable by eye:** on the HF+ the LED should go from **blue back to orange**. Use it.
- ★ A true close/reopen can still be the answer on macOS and the Pi, where no FGS rules apply.

## 4. Two things that soften the risk
- ★ `VibeServerRestore.kt` already rebuilds the server after a kill, so a jetsam is degraded rather
  than fatal.
- ★ **Doze does not apply while charging.** The mains setup Stuart tested is the SAFEST case; the
  battery/field case is where this has to be proven. Do not generalise from a plugged-in test.

## 5. ★ AND IT SHOULD PROBABLY BE THE USER'S CHOICE
"Keep the radio ready (instant connect, more battery)" vs "power the radio down when idle (a second
or two to first audio, far longer battery)". ★ The answer genuinely differs between a phone on a
shelf at home and one in a bag on a hill — the same reasoning as the per-server idle override in
`BRIEF-idle-handback.md` §2.
