# BRIEF — Radio Roles & Background Tasks

**Status:** V12 — parked 2026-10-07 (Stuart: "this is now a V12 feature when we build the decoders into the app and server"). Scoped; see "Review against the codebase" at the end before building.
**Targets:** VibeServer (full). VibeServerLite APK: Public/Private roles only — no Dedicated role, no background tasks.
**Related:** `SATELLITE-ENGINE.md`, `DECODERS-GALLERY.md`; builds on the VibeServer multi-client redesign (per-radio capability descriptors, admin baton system).

---

## 1. Purpose

Let a host assign a radio exclusively to background work — e.g. the Airspy on the Pi 500 dedicated to Meteor LRPT capture — so it is **invisible to clients and the directory** and used only by scheduled tasks.

This is the "station" side of VibeServer. Meteor is the first task; the same slot later serves APRS iGate, FT8/PSKreporter skimmer, ADS-B feeder, scanner logging, and background Es logging (TEF REF idea).

---

## 2. Roles

| Role | Directory | Clients | Purpose |
|---|---|---|---|
| **Public** | Listed | Anyone | Current behaviour |
| **Private** | Hidden | PIN | Current PIN-protected behaviour |
| **Dedicated** | Never | Never | Bound to one or more background tasks |

A Dedicated radio belongs to tasks, not people. Its **results** may still be public (e.g. gallery), while the radio itself is never exposed.

> **TRAP — enforce hiding on the server.** A Dedicated radio must be absent from: the capability descriptor, the WebSocket radio list, directory announcements, and all tune/IQ/audio command handling. Reject any command addressed to it even if a client guesses its ID. UI-only hiding is a PIN without the PIN.

> **TRAP — stable radio IDs first.** If clients address radios by list index, hiding one renumbers the rest and breaks bookmarks and `sdr://` / `vibesdr://` deep links. Introduce stable per-radio IDs before roles ship.

### Admin access
- Host/admin (via admin baton / admin password) can open a **monitor view** of a Dedicated radio: listen-only, for antenna aiming and setup. Never exposed to non-admins.

---

## 3. Task model

Each task declares:
```json
{
  "task": "meteor-lrpt",
  "kind": "windowed",          // windowed | fill
  "bands": [[137000000, 138000000]],
  "cost": { "capture": 0.03, "decode": 0.60 },   // fraction of reference CPU
  "storage_per_run_mb": 180,
  "preempt_lead_s": 120
}
```

Each Dedicated radio declares (host-configured):
- Antenna / usable band coverage (e.g. `137 MHz QFH`, `2 m–70 cm vertical`).
- Allowed tasks.

### 3.1 Scheduler
- **Windowed tasks** (Meteor passes): fixed slots from the satellite engine's pass predictor. They own the radio during the window.
- **Fill tasks** (iGate, skimmer, logging): run in gaps; pre-empted `preempt_lead_s` before a windowed slot; resume after.
- Overlapping windowed slots (two satellites up): pick the higher max-elevation pass.
- **Idle:** if no task is scheduled, **stop streaming** (radio idle / powered down where supported); restart ~2 min before next window.

### 3.2 Station profiles (examples)

| Profile | Tasks | Notes |
|---|---|---|
| ISM + Meteor | ISM 433 MHz (fill) → Meteor (windowed) → ISM | ISM results go to log/dashboard, not gallery |
| WEFAX + Meteor | HF WEFAX (windowed, broadcast schedule) → Meteor (windowed) | e.g. Airspy HF+ Discovery covers HF and 60–260 MHz |

### 3.3 Priority & pre-emption
- **Meteor is top priority** and pre-empts every other task, windowed or fill, **regardless of progress**.
- Pre-empted WEFAX: abandon immediately and **save the partial chart to the gallery** as `Partial: pre-empted by METEOR (62%)`. WEFAX decodes line by line in real time, so a partial image already exists.
- Cut-over timing: pre-empt at **AOS − (retune + settle)** (~20–30 s), not a fixed 2 min, to keep as much of the WEFAX chart as possible.
- Pre-emption threshold: only passes with predicted max elevation ≥ host setting (default 15°) pre-empt. Lower passes are skipped (`Skipped: low pass 7°`), since they rarely decode and would cost a chart for nothing.

> **TRAP — retune settle.** Task switches change frequency and often sample rate (RTL-SDR: stream restart). Allow a settle period before decode starts after every switch.

> **TRAP — ISM privacy.** 433 MHz picks up neighbours' sensors and passing cars' TPMS, and TPMS IDs can track vehicles. ISM results are **host-only by default**; publishing is per-sensor and deliberate (e.g. the host's own weather station).

> **TRAP — ISM decoder licence.** Check rtl_433's licence; GPL-2-only code is incompatible with GPLv3. Clean-room preferred, as for LRPT.

> **TRAP — antenna ports.** HF, 137 MHz and 433 MHz rarely share an antenna. Each task declares an antenna port; the scheduler only pairs compatible tasks. Future: antenna-switch hook (GPIO/relay) driven per task.

> **TRAP — antenna fit.** Only offer fill tasks whose bands match the radio's declared antenna. An HF skimmer on a 137 MHz QFH "runs" and decodes nothing.

> **TRAP — report gaps.** Fill tasks pre-empted by passes have coverage holes; log them in task status so the host doesn't assume a fault.

> **TRAP — USB bandwidth.** Pi 500 with 4+ radios shares USB controllers. Run Dedicated radios at the lowest sample rate that covers the task (decimate early), and idle between windows.

---

## 4. Benchmark gating

Reuse the existing VibeServer benchmark (which replaced the abandoned Linux Lite build).

**Headroom** = benchmark capacity − public radios at full listener load (not idle).

| Headroom vs task cost | Behaviour |
|---|---|
| **Green** | Enable freely |
| **Amber** | Allowed with warning: `May affect listeners on this server` |
| **Red** | Off by default; host override with stronger warning |

**Deferrable tasks** (e.g. Meteor decode: capture now, decode later) are treated differently: the governor's auto-pause makes them safe even on weak servers. For these, the bands set the *expected wait*, not permission. Green = decoded shortly after the pass; Amber = may wait for quiet periods; Red = allowed, with a warning `Decodes will be slow on this server`. The Red "off by default" rule applies only to **real-time** background tasks (live skimmers, iGate decoding) that can't be deferred.

Gate **decode** cost, not **capture** — capture (receive, decimate, write) is cheap and allowed almost everywhere; storage is its limit.

---

## 5. Live governor (listeners always win)

The benchmark is a snapshot; load and thermals change. A runtime governor watches Server Health:

- **Inputs:** audio underrun rate, thermal throttling state, sustained CPU. Not CPU % alone.
- **Action order:** throttle background decode → pause background decode → (never) degrade listeners.
- **Hysteresis:** pause after ~5 s of bad health; resume only after ~30 s clear.
- **Status:** `Paused: server busy` shown in task status.

> **TRAP — capture is real-time.** Capture must never be paused by the governor (dropped samples corrupt the image). Capture runs at normal priority; only decode is throttled/paused.

> **TRAP — `SCHED_IDLE` is necessary but not sufficient.** On Linux run decode threads at `SCHED_IDLE`. This doesn't prevent thermal throttling, memory-bandwidth or cache contention on small ARM cores — the governor pause is still required.

> **TRAP — flapping.** Without hysteresis the decoder toggles near threshold and causes the stutter it is meant to avoid.

---

## 6. Health & visibility

Nobody listens to a Dedicated radio, so status is the only feedback. Host-only status line per Dedicated radio / task:

```
Airspy · METEOR LRPT
Last pass 07 Oct 21:42 · EL 63° · peak SNR 9.1 dB · decoded ✓
Next pass 08 Oct 09:14 · EL 41°
Queue: 1 waiting · Decode paused: server busy
```

> **TRAP — silent failure.** A disconnected feeder means weeks of empty passes. Alert (VTS / admin notification) after N consecutive passes with no signal above threshold.

---

## 7. Storage

- Per-task storage cap; host-configurable.
- Queue cap (e.g. 5 undecoded passes); drop **oldest undecoded** first; show `3 passes waiting`.
- Delete raw IQ after successful decode (configurable retention).
- Check free space before each window; skip capture with a status reason if insufficient.

---

## 8. VibeServerLite APK

Lite is a **live receiver**. No Dedicated role, no background tasks, no scheduled capture, no IQ recording.

Rationale (not just CPU):
> **TRAP — Android kills unattended work.** Doze, app standby and aggressive vendor battery killers (notorious on cheap TV boxes) make overnight scheduled jobs unreliable even with a foreground service.

> **TRAP — storage on old Android.** Small, worn flash; inconsistent scoped-storage behaviour across the SDK range Lite targets.

---

## 9. Protocol

- Dedicated radios: never in client-facing descriptors.
- Admin API: role assignment, task assignment, antenna declaration, task status, monitor view — admin-authenticated.
- Results surfaced via gallery API (DECODERS-GALLERY.md), not via the radio.

### Admin authentication
> **TRAP — passwords on LAN.** Tunnel connections are HTTPS; direct LAN/mDNS connections are plain `ws://`. Use challenge–response (server nonce → client HMAC(nonce, password)); password never crosses the wire. Rate-limit failures.

---

## 10. Test plan

1. Dedicated radio absent from descriptor, radio list, directory; direct command by guessed ID rejected.
2. Stable IDs: hide middle radio of three; existing bookmarks/deep links still resolve correctly.
3. Scheduler: Meteor window pre-empts 2 m APRS fill task; fill resumes after LOS; gap logged.
4. Idle between windows: confirm streaming stops; restart 2 min before AOS.
5. Governor: load public radios (multiple advanced RDS sessions) during decode → decode pauses within 5 s, resumes after 30 s clear, no audio underruns on public radios.
6. Thermal: sustained decode on Pi 500 without heatsink — governor responds to throttling flag.
7. Storage: fill to cap; oldest undecoded dropped; capture skipped with reason when disk low.
8. Benchmark bands: Green/Amber/Red behaviour on Pi 500, Pi 2, Sony Android TV.

---

## Review against the codebase (2026-10-07)

**Already built**
- **Stable radio IDs (§2 trap):** done — radios are addressed as `/r/<id>/…` with serial-derived IDs (e.g. `59a1a7d0`,
  `00000002`); the directory and the app use them. Hiding a radio does not renumber anything.
- **Admin challenge-response (§9):** done — `vs_admin_nonce` + HMAC proof (`local_sdr_shim.cpp`, admin unlock; PIN proof
  `verifyPinProof`). Rate-limiting exists for PIN attempts; check it covers admin too.
- **Benchmark (§4):** exists (memory `lite_first_setup_benchmark.md`).
- **Per-radio processes:** each radio runs as `vibeserver@<serial>` behind the front door — a Dedicated radio is a natural fit
  (its process never registers routes). ★ The front door currently reports listeners 0 for itself (fixed in the directory
  page 2026-10-06; server-side summing still owed) — Dedicated radios must also be excluded from any summed counts.

**Lessons from this week that apply**
- **"Keep radio alive" (RC23)** and the USB standby fix: a Dedicated radio that idles between windows (§3.1) must not trip the
  USB give-up logic or lose its USB permission on Android — but Lite has no Dedicated role, so this is Linux/macOS only; on Linux
  idle = stop streaming is fine.
- **Removing a radio leaves a failed systemd unit** (`vibeserver@00000003` on the Pi 500, 2026-10-06) — role changes that stop a
  radio's process should `systemctl reset-failed` it.
- **Visitor stats:** the 2026-10-06 survey showed real listeners on every radio. A Dedicated radio removes a public one — say
  so in the setup UI ("this radio will disappear from the directory").
- **Governor inputs (§5):** the health chip already measures CPU/thermal/throttle (memory `throttle_snail_design.md`); audio
  underrun counters exist per listener (`chanDrops`, now sized by time since RC25).
