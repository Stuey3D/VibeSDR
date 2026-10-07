# BRIEF — Satellite Engine & Doppler Lock

**Status:** V12 — parked 2026-10-07 (Stuart: "this is now a V12 feature when we build the decoders into the app and server"). Scoped; see "Review against the codebase" at the end before building.
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
