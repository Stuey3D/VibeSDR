# BRIEF: Band-activity snapshots on the server splash

**Project:** VibeServer (multi-radio / fixed-span multi-user), VibeSDR instance picker
**Author:** Stuart Carr (Stuey3D) with Claude, 2026-07-26
**Status:** DESIGNED, not built. Depends on fixed-span multi-user mode.
**Prior art:** Stuart's own UberSDR "Band Activity" page (`/band_activity.html`) — a tile per band
with a mini spectrum + short waterfall, P5/peak figures, live UTC clock, plus propagation, user
count and recent spots.

---

## 1. The problem it solves

**You currently choose a server blind.** The picker offers a name, a distance, sometimes a reported
SNR — and nothing about whether the band is actually OPEN on that receiver right now. So a user
spends a connection (and on a single-user server, the whole radio) to find out.

A snapshot beside each profile answers "is this worth my time" BEFORE connecting. On a directory of
dozens of receivers that is the difference between hunting and choosing.

## 2. ★★ It is FREE in the mode that wants it, and absent in the mode that does not

**Fixed-span multi-user mode:** the radio is already pinned to its band with the FFT running for its
listeners. The snapshot is a BYPRODUCT of work already being done — no retune, no sweep, no
interruption, no etiquette problem. It refreshes continuously for nothing.

**Single-SDR-per-user mode: DO NOT DO THIS.** There the radio's value is being parked and instantly
available, and a snapshot would mean sweeping bands — retuning a radio somebody is about to want,
which is precisely the tuner-time cost `BRIEF-dial-and-station-sync.md` exists to avoid. Radios park
fully instead.

★ The two modes want opposite behaviour and that is coherent, not a compromise. There is no sweep
logic to build at all.

## 3. The open decision: an idle multi-user radio

A band radio with ZERO listeners parks its capture (that is where the power goes — the battery work
measured the dongle, not the DSP, as most of the load). So:

- **keep capturing** → snapshot stays live, costs the dongle power we just saved;
- **park** → free, but someone landing on the splash sees an hour-old picture;
- **periodic wake** → park, refresh every N minutes, park again. Most of both, at the cost of some
  start/stop churn.

★ Likely an owner setting, same shape as the other operator ceilings.

## 3b. ★★ Cadence: a TIMELAPSE, not a waterfall (Stuart, 2026-07-26)

An idle radio wakes, snapshots, and sleeps again — "like a timelapse". Cadence ADAPTS:

- **On first draw / boot: fill fast**, so the strip populates and the profile does not present an
  empty box.
- **Then decay to a maintenance rate** (~1 minute), so the radio is ~99% idle and the power saving
  of parking is kept almost intact.

★★ **This makes each row MINUTES apart, so the strip is a timelapse of the band — an hour of history
in ~60 rows.** That is a better answer to "is this band open" than any live waterfall, because it
shows the diurnal pattern rather than an instant.

★ **It must therefore be LABELLED as a timelapse.** A minute-spaced strip looks identical to a live
waterfall and means something entirely different: someone reading it as live would see a sparse band
and conclude the receiver is deaf. Stuart's own Band Activity page labels its time axis (10s..70s);
this one needs minutes/hours and an explicit marker.

## 3c. ★ SIZING, taken from Stuart's own 24h spectrogram (2026-07-26)

His UberSDR spectrogram of 40m over 24 hours reports **400 bins · 500 Hz/bin** across the 200 kHz
band, with rows roughly 2 minutes apart. That is a real, working reference point:

- **A few hundred bins is ENOUGH.** No need for 1024 here — this is a "is the band alive" view, not
  a tuning surface.
- **Storage is trivial:** 400 bins x ~720 rows (24 h at 2 min) = ~288 KB raw per band, far less
  as PNG. A whole day of every band is comfortably cacheable.
- **~1-2 minute rows** confirm the maintenance cadence in 3b.

★★ **What the 24h view reveals, and a live waterfall cannot:** the CW segment (7.000-7.050) is dense
from ~18:00 to ~06:00 and nearly empty in daylight; the noise-floor trace climbs overnight and falls
through the morning; FT8 at 7.075 is a solid 24-hour vertical while everything around it varies with
propagation; and slow drifting carriers trace visible curves across the day. THAT is the payload —
the diurnal pattern, not the instant.

## 4. ★ Honesty requirements

- **Show the age.** A snapshot of a dead band from 40 minutes ago is worse than none, because it
  reads as current. Stuart's own page gets this right: it says "Live" and carries a UTC clock.
- **Have a STALE state**, visibly different from fresh — do not silently show old data.
- Keep it small: a strip per band plus P5/peak is a few KB, which matters when a directory shows
  many servers at once.

## 5. Where it fits

- The instance picker's "which server should I pick" problem (see
  `BRIEF-instance-picker-discovery.md`).
- `BRIEF-vibeserver-multiradio-gui.md` — the multi-user mode this depends on, and the idle/park
  behaviour it interacts with.
- Reuses the same peak-held FFT→bins resample the spectrum path already does; no new DSP.

---

## Keeping an IDLE public server reachable (2026-07-28)

Raised while planning to port-forward a Samsung tablet to the internet: with no
users, will Android power management sleep the app and never wake it for a remote
connection?

★★ **Two real gaps found, before any testing:**

1. ~~**No WifiLock on the VibeServer path.**~~ ★★ **WRONG — I checked the module
   and not the service.** `RtlTcpServerService` DOES acquire one
   (`VibeWifiLock(this, "VibeSDR:RtlTcpServer")`, acquired :101, released :209),
   and `startVibeServerNow` starts that service. The declaration comment in
   VibeLocalSdrModule even says so: "the server path holds its own in the FGS".
   ★ Second time in one day I called something missing by looking at one file —
   the multicast lock was the first. CHECK THE SERVICE, not just the module.
2. **The foreground service is MEDIA-typed.** `VibeStreamService` uses
   `FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK`, which is honest while audio flows and
   questionable for a server with no listeners. If that service is not running,
   nothing is holding the process up at all.

★ Samsung and Motorola both ship aggressive app-standby on top of stock Doze, so
"it works on my Pixel" is not evidence. Test the actual tablet.

### Why the snapshots idea fits

Stuart's instinct: an "idle keep-awake" switch that keeps the SDR IN USE. That is
the same work this brief already describes — periodically sweeping the band to
build an activity map. The server is then never idle in the OS's eyes, and the
sweeping is not make-work: it produces the band-activity data a visitor wants
before they tune anywhere.

★ It also gives the keep-awake an HONEST JUSTIFICATION for the foreground service
notification, which matters if this ever goes near the Play Store: "scanning the
band" is a real ongoing task, where "staying awake in case someone connects" is
the kind of thing OEM power managers exist to kill.

### Order of work

1. Acquire the WifiLock whenever the server is listening, not only when streaming.
   Cheapest fix, and it is the one that stops "unreachable until I touch the phone".
2. Confirm what keeps the process alive with zero listeners; give the service a
   type that matches what it is actually doing.
3. Then the periodic sweep, which subsumes the keep-awake switch.

★ Costs battery and heat — fine on a plugged-in tablet, wrong as a default on a
phone. Make it a setting, defaulting OFF, and say what it costs.
