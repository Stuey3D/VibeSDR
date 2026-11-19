# BRIEF — Satellite Engine & Doppler Lock

**Status:** V12 — parked 2026-10-07 (Stuart: "this is now a V12 feature when we build the decoders into the app and server"). Scoped; see "Review against the codebase" at the end before building.
**Built 2026-11-19 (branch `v12-sat`):** the engine, the CelesTrak supply, the Doppler lock (listener channels and the shared dial), web / app / R36S UI, the decoder hook — see **"BUILT — 2026-11-19"** and **RESUME HERE** at the end. Synthetic tests only; **not yet tried on a real pass.**
**Targets:** VibeServer (all builds incl. VibeServerLite APK), VibeSDR app (iOS/Android), web client
**Related:** `RADIO-ROLES.md`, `DECODERS-GALLERY.md`

---

## 1. Purpose

Let a listener tune to a satellite downlink (e.g. ISS cross-band repeater, 437.800 MHz), press **Doppler Correction**, and have VibeServer keep the signal centred for the whole pass — including through gaps between transmissions.

Motivation: Kiko (Brazil) receiving the ISS on 437.800 MHz on an old Android TV box with a home-made antenna, hand-tuning in 100 Hz steps from 437.7985 to 437.7931 in ~39 s (~140 Hz/s), with the trace still drifting off the VFO between taps.

The satellite engine is **shared infrastructure**: the Doppler lock is its first consumer; SSTV, APRS and Meteor LRPT (see decoders brief) reuse it.

### Design principle
Complexity in the engine, not the settings. One button. The main dial keeps showing the published frequency; the engine does the rest.

---

## 2. Physics (for reference)

| Band | Max Doppler (LEO) | Max rate (overhead pass) |
|---|---|---|
| 2 m (145.8 MHz) | ±3.3 kHz | ~70 Hz/s |
| 70 cm (437.8 MHz) | ±10 kHz | ~215 Hz/s |
| 137 MHz (Meteor) | ±3.1 kHz | ~65 Hz/s |

Rate peaks at TCA (time of closest approach) ≈ `f · v² / (c · d_min)`.

---

## 3. Architecture

Two layers:

1. **Open loop (feed-forward):** SGP4 from orbital elements + server location + time → predicted Doppler `f_pred(t)`. Updated at 10 Hz.
2. **Closed loop (trim):** slow FLL measures residual error of the real signal vs prediction and applies a correction. Absorbs element age, clock error, dongle ppm.

```
applied_offset(t) = f_pred(t) + trim
```

### Why not a pure PLL/FLL
Breaks between transmissions. A free-running loop extrapolates; the orbital model *knows* where the signal will be. On signal loss the loop freezes `trim` and the model keeps moving — when the next transmission starts it is already centred.

### Cross-band FM repeaters
The ISS repeater demodulates and re-transmits, so every station heard shares one Doppler curve (the satellite's). Multiple speakers do not disturb the lock.

### Linear transponders (SSB/CW satellites) — v2
Each uplinking station brings its own Doppler error; the closed loop would chase whoever is talking. For these: **model only, loop disabled.** Out of scope for v1 except as a flag in `sats.json`.

---

## 4. Implementation details

### 4.1 Where the correction is applied
- **Server-side, inside the channel's DDC/NCO**, not by retuning the VFO or dongle.
- Existing IF (e.g. 350 kHz) already covers ±10 kHz — no hardware retune.
- Main digits show **nominal** (437.800 000); channel actually sits at nominal + offset.

### 4.2 Closed loop (NFM)
- Error estimator: mean of FM discriminator output over 0.3–0.5 s.
- **Update only when squelch open AND SNR above threshold.** Otherwise freeze integrator (`COAST`).
- Type-2 loop (tracks a ramp with zero steady-state error); acceleration near TCA is handled by the model, not the loop.
- **Clamp trim to ±2–3 kHz** around the model.

> **TRAP — capture range.** Without the clamp the loop will lock to adjacent carriers or intermod (a steady carrier near 437.82 is visible in Kiko's waterfall).

> **TRAP — server's own updates are not tunes.** The tracker moves frequency at 10 Hz. Tracker updates must use a separate internal path from client tune commands, or the lock will detect its own movement as a tune-away and cancel.

### 4.3 Curve-fit fallback (no elements / uncatalogued satellite) — phase 2
Straight-line flyby model, least-squares fit on squelch-open frequency measurements:

```
f(t) = f₀ − (f_c / c) · v²(t − t₀) / √(d² + v²(t − t₀)²)
```

Parameters: `f₀` (centre + ppm), `t₀` (TCA), `d` (min range); `v ≈ 7.6 km/s`. Usable after ~15–20 s of signal. Also used when cached elements exceed max age (§6.3).

---

## 5. Time

Doppler at TCA is ~215 Hz per second of clock error at 70 cm. Time matters most in `COAST` and for AOS/LOS timing; the closed loop absorbs small errors while signal is present.

### 5.1 Time source priority
1. **Server NTP** (VibeServer already performs periodic NTP sync — reuse it).
2. **Host's own app** (field mode, no internet).
3. **Listener's client device** — applied **only to that listener's own channel**.

### 5.2 Client time offset (NTP-style over existing WebSocket)
```
client sends t0 → server stamps t1 → client receives at t3
offset ≈ t1 − (t0 + t3)/2      uncertainty ≈ RTT/2
```
Take 5–8 samples, keep the lowest-RTT one. Re-sample every few minutes. New message type (ping/pong); old apps never send it and fall back to server time.

> **TRAP — offset vs system clock.** NTP sync on Android/Lite stores an *offset*; it cannot set the system clock unrooted. SGP4 and all Doppler timestamps must read the **same corrected time source** FT8 uses — never raw `System.currentTimeMillis()` / `clock_gettime()`.

> **TRAP — never set the host system clock.** Store offsets only.

> **TRAP — one listener's clock must never set server time.** A wrong or malicious client clock would shift Doppler for everyone and could break FT8 server-wide. Client offsets apply to that client's channel only.

> **TRAP — "synced once" isn't "synced now".** If server has NTP and a client disagrees by > 2 s, trust the server. If neither is synced, use client and show `TIME: CLIENT`.

> **TRAP — Pi has no RTC.** Booting offline = clock may be hours wrong. Refuse to track (`NO TIME`) until at least one successful sync from any source.

> **TRAP — browser clocks.** `Date.now()` is UTC epoch ms (no timezone handling needed) but can jump on OS correction. Take offset from `Date.now()`, measure elapsed time with `performance.now()`.

> **TRAP — borrowed time source.** If the lock relies on a departed client's offset (field mode), keep using the last offset for the rest of the pass, but don't start a *new* lock on it without fresh time.

---

## 6. Data: orbital elements & transmitter frequencies

### 6.1 Sources
- **Orbits:** CelesTrak GP data, `GROUP=amateur` (plus weather group for Meteor), **OMM JSON format**.
- **Frequencies/modes:** SatNOGS DB transmitters API (downlink, mode, baud). Check licence and attribute (believed CC BY-SA — verify).

> **TRAP — do not use TLE format.** 5-digit catalogue numbers ran out (July 2026); new objects have 6-digit numbers TLE cannot represent. Parse OMM JSON straight into SGP4.

> **TRAP — CelesTrak rate limits.** No more than once per hour; stop immediately on non-200; repeat offenders are IP-firewalled. Brazilian hosts on mobile data share carrier CGNAT IPs — many VibeServers fetching independently could get a whole carrier blocked.

### 6.2 Mirror on vibesdr.net
- Job on vibesdr.net fetches CelesTrak every 6–12 h, SatNOGS daily.
- Joins into **`sats.json`**: curated list (~30–50 satellites actually supported).
- VibeServers sync `sats.json` **like EiBi**: daily, random jitter, keep last good copy.
- App can supply `sats.json` to a server that has no internet.

Suggested shape:
```json
{
  "generated": "2026-10-07T12:00:00Z",
  "satellites": [
    {
      "id": "ISS",
      "name": "ISS",
      "norad": 25544,
      "omm": { "EPOCH": "...", "MEAN_MOTION": 15.5, "...": "..." },
      "transmitters": [
        { "label": "FM repeater", "downlink": 437800000, "mode": "NFM",
          "loop": "fll", "uplink": 145990000, "ctcss": 67.0 },
        { "label": "APRS", "downlink": 145825000, "mode": "AFSK1200", "loop": "fll" },
        { "label": "SSTV events", "downlink": 145800000, "mode": "NFM", "loop": "fll" }
      ]
    },
    {
      "id": "METEOR-M2-4",
      "transmitters": [
        { "label": "LRPT", "downlink": 137900000, "mode": "LRPT",
          "symrate": 72000, "mod": "QPSK", "loop": "costas" }
      ]
    }
  ]
}
```
`loop`: `fll` (FM repeater), `none` (linear transponder, model only), `costas` (digital demod does its own fine tracking; engine supplies feed-forward).

> **TRAP — Meteor modes and frequencies change on orbit** (72k QPSK ↔ 80k OQPSK, 137.100 ↔ 137.900). Always per-satellite in `sats.json`, never hard-coded.

### 6.3 Element age
| Age | Behaviour |
|---|---|
| < 3 days | Normal |
| 3–7 days | Track, show `STALE` |
| 7–14 days | Track, show age (`ELEMENTS 9 d`) prominently |
| > 14 days | Model disabled; curve-fit fallback only |

---

## 7. Location

- Server location = directory Maidenhead grid (6-char ≈ 5 km is ample). Nearest-city guidance for rural hosts is acceptable within ~50 km (TCA timing shifts ~1 s).
- **Field mode:** app may supply location, **rounded to 6-char grid on the device before sending.** Precise coordinates never leave the phone; the grid square is the most precise location VibeServer ever stores or displays.

> **TRAP — field location ≠ hosting location.** A field deck 200 km from its registered grid is noticeably wrong in AOS/LOS. Use app-supplied grid in field mode.

> **TRAP — visibility is computed locally.** `sats.json` only carries orbits; the server computes AOS/LOS/elevation for its own grid. Never offer satellites below the horizon.

---

## 8. User flow

1. User tunes near a downlink (e.g. 437.7931 — often already hand-tuned partway).
2. Tap frequency → tuning controls → new **Doppler Correction** button.
3. Server matches tuned frequency against `sats.json` downlinks within **±12 kHz**:
   - **One match, above horizon** → lock; snap main digits to nominal (437.800).
   - **Several matches** → choose highest elevation; show picker only if two are up.
   - **Match, not yet risen** → **arm**: `ISS · AOS 3:40`; auto-lock at AOS.
   - **No match** → `NO SAT` (phase 2: offer curve-fit lock).
4. Lock runs on the server until ended (§9).

---

## 9. Lock lifecycle

**Server-owned.** Once invoked, the server has everything it needs; the lock persists independently of the client that started it.

Ends only on:
- **Deliberate tune** by anyone with tune rights: tune command moving > ~15 kHz from current *actual* frequency, or band/mode change, or bookmark/deep-link recall.
- **LOS** (below horizon / min elevation 0–5°).
- **Doppler button pressed again.**

Does **not** end on signal dropout (that is `COAST`).

Small fine-tune nudges while locked adjust `trim` rather than cancel.

On end: **freeze VFO at current actual frequency** — do not snap back to nominal (a several-kHz jump would cut off passengers).

> **TRAP — reconnect re-sends.** Many clients re-send last frequency on reconnect, resume or mode refresh. Ignore frequency sets during the reconnect handshake; apply the 15 kHz / band / mode test above.

> **TRAP — orphaned armed locks.** If armed and all clients leave, cancel arms > 30 min from AOS. Active locks may run to LOS unattended.

---

## 10. Display

Reuse the airband channel-alignment sub-display slot (white text beside main digits).

```
 437800.000   ISS · ▼ 3.21 kHz
              TRACK
```

- **Main digits:** nominal frequency.
- **White text:** satellite name · direction (▲ approaching / ▼ receding) · offset, 2 dp kHz (matches `CH 128.590 · 8.33` style).
- **Small blue line** (`kHz` position): `TRACK` / `COAST` / `ARMED` / `NO TIME` / `STALE`.
- `COAST` dims the white text. **No flashing / flicker anywhere.**
- **Bottom status sub-display** between overs: `ISS · EL 34° · LOS 6:12`. Shared VFO: `SAT TRACK · <owner>`; on end `DOPPLER ENDED` briefly.

> **TRAP — waterfall marker.** Draw the VFO marker at the **actual** (shifted) frequency, optional faint tick at nominal. Otherwise it sits up to 10 kHz off the trace and looks broken.

> **TRAP — what gets stored.** Bookmarks, logging, web client and `sdr://` / `vibesdr://` deep links store **nominal + "track satellite" flag**, never the instantaneous shifted frequency.

---

## 11. Protocol & compatibility (V11 locked core)

- Per-radio capability descriptor: `doppler: 1` (**integer version**, not boolean — v2 adds linear transponders / curve-fit).
- Advertise **per radio** only if its tuning range covers at least one downlink in `sats.json`.
- **Supported but not ready** (no `sats.json`, no time): advertise with state; app **greys** the button with reason (`NO SAT DATA`, `NO TIME`) rather than hiding.
- Existing frequency field continues to report **actual** frequency. New fields: `nominal`, `offset`, `sat_id`, `lock_state`, `elevation`, `los_in`.
- Older apps never see the button. As Shared VFO passengers they simply see the VFO move at a steady rate — correct and harmless. An old app tuning away ends the lock (correct).
- Web client always matches its server.

### Shared VFO mode
Lock moves the channel for everyone. Lock owner is whoever invoked it; any deliberate tune by someone with tune rights ends it (consistent with "free to tune"). Per-user channel mode: lock affects only that user's channel.

---

## 12. Availability

| Build | Doppler lock |
|---|---|
| VibeServer (all platforms) | ✓ (cost negligible — no benchmark gate) |
| VibeServerLite APK | ✓ |

CPU cost: SGP4 at 10 Hz + NCO offset + FLL — well under 1 % on a Pi Zero.

---

## 13. Test plan

1. **Reference comparison:** run SDR Console (Windows) and VibeServer on the same pass from the same location; compare predicted Doppler second-by-second. Target agreement within ~50 Hz.
   > **TRAP — sign convention.** Confirm whether each tool reports *shift* or *correction* (opposite signs). Test away from TCA where offset is largest — an inverted sign looks plausible near zero crossing.
2. ISS 437.800 pass on Kiko's TV box: lock, COAST through gaps, re-acquisition latency at start of each over.
3. Clock skew injection: ±0.5 s, ±2 s, ±10 s — verify trim absorbs / `TIME` warnings fire.
4. Offline boot (Pi): `NO TIME` until sync; app-supplied time path.
5. Shared VFO: lock with old-app passenger connected; old app reconnects (must not cancel); old app tunes away (must cancel); VFO freezes on end.
6. Element ageing: synthetic 5 / 10 / 20-day-old elements.
7. Arm before AOS → auto lock; arm then all clients leave.

---

## 14. Phasing

1. Engine (SGP4, `sats.json` sync, time sources, AOS/LOS) + NFM Doppler lock + display. **ISS first.**
2. Curve-fit fallback; field-mode location/time from app.
3. Linear transponder (model-only) support (`doppler: 2`).

---

## Review against the codebase (2026-10-07)

**Already built — use, don't rebuild**
- **Corrected time (§5):** `android/app/src/main/cpp/vibe_clock.h` — `vibeUtcNow()`, SNTP-measured offset, never sets the
  system clock (built after the Sony TV read 2.64 s slow and killed FT8, 2026-09-19). SGP4 and every Doppler timestamp read
  this, exactly as the FT8/time decoders do. §5.1's "server NTP" = this.
- **Grid-square location (§7):** the server already publishes its grid-square centre (privacy policy, 2026-10-05). Field mode
  must round on the device the same way.
- **Airband channel sub-display (§10):** exists; the Doppler readout reuses that slot. ★ It must follow the DISPLAY FONT rule
  (main controls + VTS take the selected display font — VCR segment cells, DOT dot-matrix, Nixie) and VCR's lower-case-d rule
  for any dB. No flashing anywhere (the brief already says so).

**Fits the shared-dial contract — make it explicit**
- §4.2's "server's own updates are not tunes" is the SHARED-DIAL CONTRACT (memory `shared_dial_contract.md`): the server is king,
  clients transmit only on a user action, recovery/automatic moves are not user actions. The 10 Hz tracker must move the
  channel's NCO on an internal path that (a) never goes through `vibe_tune_pace.h` (the CPU-aware tune pacer would rate-limit
  it), (b) never counts as a user tune for the "tune-away ends the lock" test, (c) never re-arms band defaults
  (`userTuneSeq` — band defaults apply only on a person's tune).
- §11 "per-user channel mode": per-VFO (locked-range) radios already give each listener their own ClientDsp; the offset goes in
  that listener's channel only.

**New traps from this week**
- **Exact audio rate.** Until RC25 the audio resampler was built from a ROUNDED rate — up to ±117 ppm on 28/66 radio×mode
  chains — which slants SSTV/WEFAX. Fixed (exact-rate resampler, `test-resampler-rate`). Any new channel path for satellites
  must use the exact-rate constructor, and LRPT symbol timing must come from the true sample rate.
- **Radio clocks are good.** The HF+ measured −0.35 ppm against RWM (2026-10-07), so residual Doppler error will be elements/
  time, not the radio — but RTLs without TCXO can be tens of ppm; the closed-loop trim (§4.2) absorbs it; log the trim so a
  big constant trim flags a ppm problem.
- **A decoder is a consumer.** Since RC25 a running decoder keeps the audio chain alive (the idle gate counted only listeners).
  A Doppler-locked channel with nobody listening (armed locks, gallery capture) must hold the chain the same way.
- **Directory/CPU:** CelesTrak etiquette (§6.1) mirrors our own third-party etiquette — fetch via vibesdr.net only.


---

## BUILT — 2026-11-19 (branch `v12-sat`, off `v12-experimental` 3de409c9a)

| Part | Code | Test | Status |
|---|---|---|---|
| SGP4/SDP4, TLE (checksums, Alpha-5) + OMM JSON, observer, look angles, range rate, Doppler, passes | `android/app/src/main/cpp/vibe_sat_orbit.h` | `test-sat-orbit` 122/122 | the paper's verification set exact; Skyfield agreement |
| The curated birds, element ages, the lock, the FLL, the decoder hook | `vibe_sat_engine.h` | `test-sat-engine` 74/74 | |
| CelesTrak GP cache (per machine, etiquette) | `vibeserver/satcat.{h,cpp}`, `main.cpp` | `test-satcat` 23/23 | |
| Server wiring (NCO shift, tracker, messages, hwinfo, gates) | `local_sdr_shim.cpp` (search `[sat]` / `satControl`) | `scripts/test-server-sat.mjs` 33/33 | |
| Shared words / parsers for both clients | `src/services/satellites.ts` | `scripts/test_satellites_app.ts` all pass | |
| Web: SAT in the mode menu + the decoder panel's radio keys, the panel, the chan text, the needle | `web/client/src/satellites.ts`, `main.ts`, `mobile.ts`, `spectrum.ts` | `scripts/test-web-sat.mjs` 14/14 (headless Edge, muted, desktop + 390 px) | |
| App: SAT key, `SatellitePanel`, `ControlsBar satTag`, `WaterfallView needleOffsetHz` | `src/components/SatellitePanel.tsx`, `SDRScreen.tsx`, … | narrow `tsc` on the new/changed components (SDRScreen: syntax only — a whole-tree `tsc` needs > 1 GB) | **not run on a device** |
| R36S: SAT page + the readout over the waterfall | `handheld-ui/src/sat_page.h` | `-fsyntax-only -Wall` in `vibe-hh-map:trixie` (0/0) | **not run on the handheld** |

### The orbit core — own code, the published model

- Written from Spacetrack Report #3 as revised in AIAA 2006-6753 ("Revisiting Spacetrack Report #3"): WGS-72, the
  improved operations mode, SDP4's lunar–solar terms, 12 h / 24 h resonance integration (720 min steps), Lyddane.
- **Verification:** every case of the paper's `SGP4-VER.TLE` against its `tcppver.out` (both from the MIT-licensed
  Python `sgp4` 2.24 distribution — data only, `vibeserver/testdata-sat/`): 666 rows, **worst position 0.1 mm, worst
  velocity 0.5 µm/s**, every error ending (decay, eccentricity, semi-latus rectum) reproduced.
- **Geometry vs an independent implementation:** 70 looks + 14 passes for ISS, SO-50, AO-7, RS-44, AO-123 at IO92ng,
  São Paulo and Reykjavik against Skyfield 1.55 (`scripts/sat-reference.py`, its output committed as
  `reference-passes.txt`): azimuth/elevation **0.003°**, range rate **0.33 m/s = 0.5 Hz at 437.8 MHz**, AOS/LOS/TCA
  **0.27 s**, maximum elevation 0.003°. Sign: receding = positive range rate = received LOW (a *shift*, never a
  correction — brief §13 TRAP).
- **Cost (M5 Pro):** one look (SGP4 + the rotation + topocentric) **0.27 µs**; a day of ISS passes 1.4 ms. The 25 Hz
  tracker is ~0.0007 % of a core per lock. Pi 2 not measured (expect ~30× → still negligible).
- OMM JSON is the primary format (six-digit numbers: CelesTrak's amateur group already holds 101095 ERMINAZ-2V, which
  propagates). TLE is parsed too (owner paste, the verification set). TLE and OMM of the same ISS set agree to 0.9 m
  over a day (TLE's column rounding).

### The birds (checked 2026-11-19, `curatedCatalogue()`)

ISS (437.800 FM repeater, 145.800 voice/SSTV, 145.825 APRS) · SO-50 436.795 · AO-123 435.400 · PO-101 145.900 (FM) ·
RS-44 435.610–435.670 + beacon 435.605 · FO-29 435.800–435.900 + 435.795 · AO-7 mode B 145.925–145.975, mode A
29.400–29.500, beacon 29.502 · AO-73 145.950–145.970 + telemetry 145.935 · JO-97 145.855–145.875 · MO-122
435.810–435.840 + telemetry 435.800 · SONATE-2 437.025 (9k6) + 145.825 APRS · Meteor-M2-3 and M2-4 LRPT 137.900.
Sources: AMSAT's live FM and "Live Linear Satellites" (2026-02-21) pages, CelesTrak's current amateur + weather groups
(every number above has a current set), SatNOGS. **Left out:** NOAA-15/18/19 — decommissioned 6 June, 13 and 19 August
2025 (NOAA's POES archive notice; APT off); AO-91 (end of mission); SO-125 (SatNOGS says re-entered, AMSAT a 2026
designation — disputed); TEVEL-2 (downlinks not confirmed); CAS-4A / XW-2 (re-entered); QO-100 (geostationary).

### TLE supply (`satcat.cpp`)

CelesTrak `gp.php?GROUP=<g>&FORMAT=json` for `amateur` and `weather`, cached at `<data>/sat/gp-<group>.json` (the
machine's data dir: every radio process shares it). **The cache's age decides (12 h), never uptime; one attempt marker
per machine (`last-attempt`), no fetch within 2 h of ANY attempt; the first non-200 stops the round (later groups not
asked) and backs off 6 h → 12 h → 24 h; a 200 that is not GP JSON (a captive portal, "No GP data found") or over 2 MB
never replaces the cache; 30–330 s of jitter before the first fetch after start-up.** The cache is published at start
whatever its age; the engine says how old (STALE 3–7 d, ELEMENTS n d 7–14 d) and refuses past 14 days. curl, `-A
VibeServer` (no owner details).

### The Doppler lock (`local_sdr_shim.cpp`)

- **Where the shift lives:** a listener's channel NCO (`clientRetune`'s residual + `satShiftHz`); the shared dial's
  `vfoOffsetNow()` + `satSharedShift_`. The tracker (one thread, `vibe-sat-track`, 25 Hz, a 200 ms nap with no lock)
  re-points it with a **same-chain `RxPipeline::setTune`** — the NCO moves phase-continuously, nothing is rebuilt, the
  AGC is untouched, the tune pacer never sees it, `userTuneSeq` / band defaults never fire, no waterfall row is held.
- **No bin hop, ever:** a locked listener's channel is sized ONCE when the lock starts for nominal ± (2.7e-5·f + the
  2.5 kHz trim clamp) — `chanBinsFor(bw + 1.6·max)` — and stays that size until the next mode change, so the end of a
  pass rebuilds nothing either. Measured end to end: **0 channel rebuilds in 3 s of tracking** (`test-server-sat` §3).
- **Smoothness:** worst step **5.9 Hz per 25 Hz tick** over a 56° ISS pass at 437.8 MHz (max Doppler 10.0 kHz, max
  rate 148 Hz/s); the trim is slew-limited to 100 Hz/s (≤ 4 Hz a tick). Readouts twice a second; the web needle
  glides between them at the readout's rate.
- **The loop** (FM repeaters only — `loop:"fll"`): the channel IQ (`clientIqCb`) → `ResidualMeter`: Σ x·conj(x₋₁)
  over 0.4 s windows (no atan per sample; NEON / SSE2 / scalar give identical sums — §7) → the window's SNR by the
  M2M4 moments (constant-envelope FM in noise: S = √(2·M2² − M4)) gated at 3 dB → a type-2 update (target = applied +
  ½·error; a rate term for ramps) → clamp ±2.5 kHz, refuse any residual over 5 kHz outright (the steady carrier beside
  ISS). COAST after 1.2 s with no gated window: the trim FREEZES, the model carries on. Simulated: a 1.5 s clock error +
  1.6 ppm radio → **3 Hz after 20 s**, 2 Hz after a 6 s gap, at AOS and at TCA; 30 s of noise never opens the gate.
  **Linear transponders and `costas` downlinks: model only** (trim 0, never COAST).
- **Lifecycle:** match within ±12 kHz of a listed downlink (a passband ±12 kHz); several → highest elevation; none up →
  ARMED for the next AOS (auto TRACK at AOS); refused in words otherwise (NO SAT / NO SAT DATA / NO TIME / no
  location / elements too old / Doppler would leave a locked window). An FM lock snaps the dial to the nominal; a
  linear lock sets the nominal so the channel does not move at all. A nudge under 15 kHz moves the nominal and keeps
  the lock; a tune away or a mode change ends it (no config sent — the listener's own tune follows); LOS (3 s below the
  horizon) or the button ends it and **freezes the dial where the channel is** (config sent). With nobody connected an
  ARMED lock more than 30 min from AOS goes; an active one runs to LOS. A listener's lock dies with their channel.
- **Shared dial:** the lock is the room's (key `kSharedKey`), anyone with tune rights ends it; `sat` is in the
  ask-before-tuning and the owner's scan-lock lists like a tune; `sat_list` (the picker) is free.
- **Time:** `vibeUtcNow()` (the corrected clock FT8 reads); SNTP-measured = `ok`; unmeasured but within [newest epoch
  − 1 d, + 60 d] = `system` (tracked, "TIME: SYSTEM" shown); otherwise NO TIME (a Pi with no RTC booted offline).
- **Location:** the published position (`setLocationJson` → `setObserver`), the grid-square centre where that is all
  the owner gave.

### Wire

```
client → server  {"type":"sat_list"}                                  the picker's list (free)
                 {"type":"sat_list","op":"track","norad":N,"aos":A,"los":L}   one pass's sky track (≤ 120 points)
                 {"type":"sat","op":"lock","norad"?:N}                the lock at the current dial (gated like a tune)
                 {"type":"sat","op":"unlock"}
server → client  {"type":"sat_list",time,located,now,why?,sats:[{id,name,norad,note,age,ageState,tx:[{label,hz,hiHz?,
                   mode,loop,consumer?,upHz?,ctcss?,note?,reach}],az?,el?,rangeKm?,lat?,lon?,passes:[{aos,tca,los,maxEl,
                   aosAz,losAz}]}]}
                 {"type":"sat",state:"OFF|ARMED|TRACK|COAST|ENDED",why?,shared?,id,name,norad,tx,loop,consumer?,
                   nominal,offset,rate,trim,az,el,aos,los,maxEl,age,ageState,time}   twice a second while locked
                 {"type":"sat_track",norad,aos,los,pts:[[az,el]…]}
hwinfo / vibeserver.json  "doppler":1 (a version) where the radio reaches a listed downlink; "dopplerWhy" = greyed
config.vfo       the NOMINAL while locked; where the channel was after LOS / unlock
```

### Deviations from the brief (decide)

- **§11 "the existing frequency field reports the actual frequency"** — NOT done: `config.vfo` stays on the nominal and
  `offset` rides in `{"type":"sat"}`. Moving the reported dial 25×/s would pan every client's view (the view follows
  the dial — the waterfall core rules) and fight the DC-placement / recentre logic. Consequence: an OLD app on a
  shared dial hears the corrected audio but its needle sits on the nominal (up to 10 kHz off the trace).
- **§9 "a tune > 15 kHz from the current ACTUAL frequency"** — measured from the NOMINAL (the dial every current client
  shows and re-sends).
- **§6.2 the vibesdr.net mirror + SatNOGS + `sats.json`** — not built; the server asks CelesTrak itself under the rules
  above, and the transmitters are the curated list in code. `sats.json` is phase 2.
- **§5.2 the client time offset (ping/pong), §4.3 curve-fit fallback, §7 field-mode location from the app** — not built.
- **§10 bookmarks storing "nominal + track satellite flag"** — bookmarks store the nominal (the dial) already; the flag
  is not stored.

### RESUME HERE

1. **A real pass.** Nothing has been tested on air. The coordinator (or Stuart) should take an ISS 437.800 or SO-50
   pass on a VHF/UHF radio with a usable antenna, press DOPPLER (or SAT → TRACK), and check: centred through the pass,
   COAST between overs and re-centred at the next, no clicks at lock start (the one channel rebuild) and none during,
   LOS freezes the dial. Then a linear pass (RS-44) in USB. The brief's §13.1 (SDR Console side by side, ±50 Hz) is
   the reference test.
2. **Android Lite / the app's own server** get no orbit data yet (satcat is the desktop daemon's curl): the app
   should fetch CelesTrak (same rules) and hand it in with `LocalSdrShim::satLoadOmm(group, json)` — or phase 2's
   vibesdr.net `sats.json`. Until then those servers show SAT greyed "NO SAT DATA".
3. **The web page header** (`android/app/src/main/cpp/vibe_web_page.h`) was NOT committed (generated): run
   `node scripts/build-web.mjs` after merging, then rebuild the server, before `test-web-sat.mjs`.
4. Phase 2 items above (sats.json mirror, client time, curve fit, field mode, the bookmark flag); a satellite glyph for
   the mode icons (none drawn now); `doppler: 2` when linear transponders get uplink-aware handling.

### RESUME HERE — the decoder hook (for v12-lrpt and v12-ax25hs)

Header-only, `#include "vibe_sat_engine.h"`, namespace `vibe::sat`:

```cpp
DecoderView v = vibe::sat::decoderView(session);   // this listener's lock, else the shared dial's, else v.on == false
//   v.on, v.state ("TRACK"/"COAST"/"ARMED"/"ENDED"), v.norad, v.satId, v.satName, v.txLabel,
//   v.consumer ("lrpt" | "ax25-9600" | "ax25-1200" | "bpsk-telemetry" | ""), v.loop ("fll" | "none" | "costas"),
//   v.downlinkHz (the nominal = the dial), v.inChannelHz, v.modelHz, v.rateHzS, v.elDeg, v.azDeg, v.rangeKm,
//   v.aos, v.los, v.ageDays
double d, r, el;
vibe::sat::dopplerNow(norad, hz, vibe::vibeUtcNow(), d, r, &el);   // the model alone, no lock needed (gallery captures)
vibe::sat::engine();                                               // the process's engine (listJson, passes, …)
```

★★ **Do not correct twice.** `inChannelHz` is ALREADY in the listener's channel NCO: a decoder fed from that
listener's channel IQ (`clientIqCb`, "the dial at DC") sees only the residual and must not apply Doppler again (on
`costas` downlinks the engine's trim is 0, so `inChannelHz == modelHz` and your carrier loop owns the residual). A
decoder cutting its OWN slice at the nominal off the shared FFT (`feedSkimFromBins` style) must apply `modelHz` itself
and may feed `rateHzS` into its timing / carrier loop as feed-forward. `decoderView` is a map look-up under the engine's
mutex (no SGP4) — fine per block; the values are the tracker's last tick (≤ 40 ms old). Meteor's mode and frequency
change on orbit: read `consumer`/`downlinkHz` from the view, never hard-code 137.9. Tested in `test-sat-engine` §8.

### Open decisions for Stuart

- The R36S readout sits over the waterfall above the VTS (the 640 px deck has no room beside the digits) — right place?
- CelesTrak direct (with the rules above) until a vibesdr.net mirror exists — OK, or hold the feature until the mirror?
- The nominal stays in `config.vfo` (old apps' needle sits on the nominal) — accept, or report the actual as the brief says?
- An owner switch for satellites on the setup page (none now: one button, no settings, and no decoder CPU to protect)?
- Curated list entries flagged uncertain: PO-101 (scheduled), AO-7 (sunlight only), FO-29 (power-permitting), Meteor-M2-4's
  LRPT frequency/mode — keep, or show only birds confirmed in the last month?
