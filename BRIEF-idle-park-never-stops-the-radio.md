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

---

# ★★ OPTION: DROP TO THE LOWEST IQ RATE WHILE IDLE?
Stuart: *"would it be worth, rather than sleeping the SDR entirely, maybe dropping it to its lowest
IQ mode to reduce load, then increasing when in use?"*

★★ Lower risk than a power-down — but it probably **saves the wrong part**, and the brief should say
so plainly.

**Where the RTL's power actually goes:** the RTL2832U's ADC runs at a **fixed 28.8 MHz clock and
decimates internally**, and the R820T tuner is powered continuously. So the OUTPUT sample rate
barely touches the dongle's own current. Dropping 2.4 MSPS → 250 kSPS cuts **USB bus activity and
host CPU** — which the measurement above puts at **under 0.4 W of 2.1 W**. That is optimising the
cheap 20% and leaving the expensive 80% running. The real saving needs the tuner and ADC actually
powered down, i.e. the device closed (or its power-down registers hit) — back to the risky option.

**It is still not worthless, and may be the right FIRST step:**
- ✅ The stream stays alive, so the watchdog's liveness test still works and the `connectedDevice`
  FGS precondition is untouched — both hazards from the section above are avoided.
- ✅ Resume is a control transfer, not a device re-open, so first-audio latency stays tiny.
- ★ **CATCH:** changing an RTL's sample rate generally wants the async read stopped and the buffer
  reset — **the same stop/start path the libusb abort lives in.** So it may not dodge that race
  after all. Check before assuming it is the safe option.

## ★★★ STOP SPECULATING — THREE READINGS SETTLE IT
None of us has measured this. An inline **USB power meter** (~£10) answers it in ten minutes:
1. streaming at **2.4 MSPS**
2. streaming at the **minimum rate**
3. device **open but not streaming**

- If (2) ≈ (1), the rate drop saves nothing and only a real power-down will do.
- If (2) ≪ (1), it is a cheap win worth taking on its own merits.
- (3) tells us whether "stop streaming, keep the handle" is worth anything at all.

★ Do this BEFORE writing any of the three options. Between the Airspy's status LED and a phone that
reports %/hr, this rig can settle it better than most.

---

# ★★★ THE BEST OPTION: DUTY-CYCLE IT — power down, wake for a snapshot, sleep again
Stuart: *"we power down the SDR, then every 15–30 seconds we wake it for a snapshot of the spectrum
then sleep it again."*

★★ **Already designed** — `BRIEF-band-activity-snapshots.md` §3 lists "periodic wake" as one of
three options for an idle radio, and §3b has the cadence worked out as *"a timelapse, not a
waterfall"*, decaying to a maintenance rate so the radio is ~99% idle.
★★★ **What is NEW is the framing: the periodic wake is not merely how the snapshot stays fresh — it
is the IDLE STRATEGY ITSELF.** The power fix and the band-activity feature are the same mechanism.

## Why it wins
**1. It gets essentially the whole saving.** A snapshot = open, settle, a few FFTs ≈ 0.5 s. At a
30 s cadence that is **~2% duty**, so the dongle's ~1.7 W averages ~0.05 W. Idle falls to roughly
what the phone alone costs — the ~9 h → 40+ h prize — **while still producing something useful.**

**2. ★★★ IT FIXES WHAT MAKES CLOSE/REOPEN SCARY.** Today the resume path runs RARELY — only when
someone connects — which is precisely why the libusb abort was so nasty: *"armed by an EMPTY server
and fired by the next person to arrive."* **A rare path is where Heisenbugs live.** At 30 s cadence
it runs **~2,880 times a day**, so either it is genuinely solid or we find out within minutes of
switching it on. ★★ That turns the riskiest part of the fix into the **best-exercised code in the
server**, and it is the strongest argument for this option.
★ The flip side: anything that leaks — an fd, a buffer, a thread — compounds 2,880×/day instead of
twice. Also caught fast, but watch for it specifically.

**3. It produces a feature rather than only a saving** — the band-activity timelapse the other brief
wants, for free, out of the idle state.

## ★★★ INCOMPATIBILITY TO RECORD: SNAPSHOT MODE ≠ LOGGING MODE
A half-second sample every 30 s **cannot decode RDS** — that needs SECONDS of CONTINUOUS groups to
establish block sync, let alone confirm a PI. So the unattended logging in `BRIEF-rds-logbook.md` §8
needs the radio genuinely running, and duty-cycling would **silently produce an empty logbook.**

★★ Both are legitimate idle behaviours; they cannot be the SAME one. So the owner setting is three
-way, not two:
| Mode | Radio when idle | For |
|---|---|---|
| **Park** | off | longest battery, nothing observed |
| **Snapshot** | ~2% duty | band-activity timelapse, near-full saving |
| **Monitor** | continuous | unattended RDS logging — full power, and that is the point |

## Open questions carried over
- ★★ Does `foregroundServiceType="connectedDevice"` survive being CLOSED between snapshots?
  Reopening every 30 s may or may not satisfy it. Same check as the section above.
- ★ A user connecting must **trigger a wake immediately**, not wait for the next tick.
- ★ RTL settle time after open (PLL/AGC) sets the floor on the snapshot duration — measure it, since
  it decides the real duty cycle.
