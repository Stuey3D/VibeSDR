# BRIEF — Satellite Decoders, Gallery & Client-Assisted Decode

**Status:** V12 — parked 2026-10-07 (Stuart: "this is now a V12 feature when we build the decoders into the app and server"). Scoped; see "Review against the codebase" at the end before building.
**Targets:** VibeServer (full), VibeSDR app (iOS first, then Android), web client (later via WASM)
**Related:** `SATELLITE-ENGINE.md` (Doppler, `sats.json`, pass prediction), `RADIO-ROLES.md` (Dedicated radios, scheduler, governor)

---

## 1. Purpose

Decode satellite imagery and data on top of the satellite engine, store results in a **Gallery**, and let capable client devices (e.g. iPhone 17 Pro Max) take over heavy decodes from weak servers (e.g. Sony Android TV).

Positioning: **listener-first background decoding** — decoders step aside when people are listening. (Market positively; don't knock other projects.)

---

## 2. Decoders & phasing

| Phase | Decoder | Notes |
|---|---|---|
| 1 | — | Satellite engine + Doppler lock (separate brief) |
| 2 | **SSTV** | ISS SSTV events on 145.800; reuses Doppler lock; light |
| 3 | **APRS** (AFSK1200) | ISS 145.825 and others; light |
| 4 | **Meteor-M LRPT** | Heavy; capture + deferred, resumable decode; gallery; client-assisted decode |

> **TRAP — NOAA APT is dead.** NOAA-18 retired 6 Jun 2025, NOAA-15 12 Aug 2025, NOAA-19 19 Aug 2025. Build Meteor-M LRPT. APT only ever as an archive-file decoder, if at all.

> **TRAP — licence.** SatDump is the reference decoder and GPLv3, but VibeSDR's App Store exception cannot be granted over third-party code. **Clean-room VibeDSP implementation** from the published LRPT/CCSDS specifications. Use SatDump only as a black-box reference for output comparison.

> **TRAP — modes change on orbit.** Meteor has switched between 72k QPSK and 80k OQPSK, and between 137.100 and 137.900 MHz. Mode, symbol rate and frequency come from `sats.json` per satellite; never hard-coded. Support both modulations.

> **TRAP — antenna.** LRPT needs a 137 MHz QFH or V-dipole. Show predicted max elevation in pass lists so users don't blame the decoder for a 7° pass.

---

## 3. Meteor LRPT pipeline

```
raw IQ → QPSK/OQPSK demod → Viterbi → frame sync / derandomise → Reed–Solomon → CCSDS packets → JPEG MCUs → composite image
         └──────────────────────── each stage checkpoints to disk ────────────────────────┘
```

- Demod Costas loop handles fine carrier tracking; satellite engine supplies Doppler **feed-forward** for fast acquisition (±3.1 kHz at 137 MHz).

### 3.1 Capture (cheap, real-time)
- Narrowband IQ (~150 kS/s), written to disk during the pass. ~180 MB per 10-min pass at 8-bit.
- Runs at **normal priority**; never paused by the governor.
- Optional storage-saving mode: live demod, store **soft symbols** (~86 MB/pass) instead of raw IQ. Default on full VibeServer: **raw IQ** (allows re-demod with improved decoders).

> **TRAP — dropped samples.** A USB overrun mid-pass corrupts that stretch of image. Monitor overruns; record them in pass metadata.

### 3.2 Decode (heavy, deferred, resumable)
- Runs at `SCHED_IDLE` (Linux), under the governor (RADIO-ROLES.md §5).
- Processes in **100–500 ms slices** so pause takes effect promptly.
- **Checkpoints persisted to disk** at stage boundaries and periodically within stages — survives reboot and solar-host low-battery sleep.
- Progress reported as overall % plus current stage.

> **TRAP — starvation.** Permanently busy server never finishes. Queue cap, drop oldest undecoded, show queue length (RADIO-ROLES.md §7).

### 3.3 How weak servers decode Meteor (the key idea)

Meteor imagery is **not time-critical**. That separates the work into a cheap, time-critical part and a heavy part that can wait:

1. **Capture during the pass**: cheap enough for almost any host, including old TV boxes, a Pi 2 or e-waste phones running full VibeServer. Only storage limits it.
2. **Decode afterwards, whenever there's spare capacity**: the heavy stages run in small slices at idle priority. The governor watches Server Health; when listeners load the server (e.g. several advanced RDS sessions across radios), the decode **pauses automatically** and **resumes once the server is clear**. Checkpoints mean nothing is lost or redone.
3. **Result:** a weak server still decodes every pass. It just takes longer (minutes to hours instead of seconds), and **listeners never notice**.
4. **Optional speed-up:** a capable client (e.g. iPhone 17 Pro Max) can take over the remaining stages through client-assisted decode (§5), and the result returns to the server's gallery.

This is the main difference from background decoders that run regardless of listener load: here background work always gives way to people listening.

On weak hosts the benchmark (RADIO-ROLES.md §4) decides only the *expected wait*, not whether decoding is possible. A Red-band server is told "decodes will be slow and may wait for quiet periods", not "unsupported". Exception: VibeServerLite APK (no capture/storage, see radio-roles brief §8).

---

## 4. Gallery

Built as part of these decoders. Server-side store of decoded products; viewable in app and web client.

Entry:
```
METEOR-M2 4 · 07 Oct 21:42 · max EL 63°
Captured ✓ · Decoding 85% (server, paused: busy)
[ Decode on this device ]
```

States: `Captured` → `Decoding n% (server|<device>)` → `Decoded ✓ (server|iPhone)` / `Failed: <reason>` / `Rejected: client mismatch`.

Each item stores:
- Satellite, pass start/end, max elevation, peak SNR, overruns.
- **Decoder version** and **where decoded** (`server` / `client: <device class>`).
- Products (channels / composites), thumbnails.

> **TRAP — decoder version.** Stamp every item; when VibeDSP's decoder improves, passes with retained raw IQ can be re-decoded selectively.

Location privacy: gallery metadata shows grid square at most; never coordinates.

---

## 5. Client-assisted decode

Server holds captured data; a client with spare power takes over the remaining decode and returns results.

### 5.1 Flow
1. Gallery shows `[ Decode on this device ]` on a captured / partially decoded pass.
2. Client **claims a lease** on the pass.
3. Server **pauses its own decode** and sends the **smallest available stage** from its latest checkpoint.
4. Client decodes the remaining stages (VibeDSP, same engine).
5. Client uploads **verified packets** (not an image).
6. Server **spot-check verifies** (§5.4), builds the image itself, adds to gallery.
7. VTS message on client: `METEOR DECODE COMPLETE · uploading to <server>…` → `✓ Added to gallery`.

> **TRAP — ship the smallest stage.** If Viterbi is done, send decoded frames (a few MB), not 180 MB of IQ. Usually the client only needs the remaining stages.

### 5.2 Lease
- One active lease per pass. Others see `Being decoded on another device · 40%` instead of the button.
- Lease ~10 min, renewed while the client is working.
- Lease expiry → server resumes from its own checkpoint. No work lost.
- **First to finish wins**; the other result is discarded. No duplicate gallery entries.
- Client disconnected mid-decode: finish locally, queue upload for reconnect.

### 5.3 Client behaviour (iOS first)
- Decode **only while VibeSDR is on screen** (screen does not sleep while app is foreground). On background: pause + checkpoint locally; resume on foreground. No `BGProcessingTask` in v1.
- Run at low QoS (`.utility` / `.background`) — **audio and UI rendering always win**.
- Guards (mainly for slower clients — older Android, low-end tablets, later web/WASM): watch `ProcessInfo.thermalState` (slow at `.serious`, pause at `.critical`); respect Low Power Mode (pause with VTS note).
- Expectation: on A19 Pro a full pass decode likely takes seconds to ~a minute — **time it**; don't design around the slow case.

### 5.4 Verification (anti-corruption, anti-forgery)
> **TRAP — Reed–Solomon is error correction, not authentication.** A modified client can construct perfectly valid packets carrying any image.

Server-side spot-check:
- After receiving the result, server picks **N random short segments** using a CSPRNG — **chosen only after upload**, never predictable by the client.
- Server re-decodes those segments from its own data (cheap even on the TV box) and compares packets byte-for-byte.
- Validate **packet counter and timestamp continuity** across the whole pass (no dropped/duplicated stretches between checked segments).
- **Server renders the image from verified packets**; client-supplied images are never accepted.
- Mismatch → reject, log `Client result rejected · mismatch` in host task status, server decode resumes from checkpoint.

### 5.5 Permissions (host setting)
```
On-device decode:  Anyone  |  Anyone (small transfers only)  |  Admin only
```
- **Default: Anyone.**
- **Small transfers only:** anyone may take over once past the raw-IQ stage; raw IQ download admin-only. For hosts concerned about bandwidth.
- **Admin only:** uses admin password / baton (challenge–response — see radio-roles brief §9).
- Upload is always subject to verification regardless of setting.

> **TRAP — offload over mobile tunnels.** Raw IQ over Cloudflare tunnel on mobile data is costly for hosts (e.g. Brazil). Prefer LAN for raw-stage handover; compress soft symbols; never push raw IQ over the tunnel without admin rights.

---

## 6. Capability descriptor

- `decoders: { sstv: 1, aprs: 1, lrpt: 1 }` — advertised per server/radio, versioned integers.
- `lrpt` gated by benchmark (RADIO-ROLES.md §4). If below threshold, advertise as `unavailable: benchmark` so the app greys with reason.
- `gallery: 1`, `client_decode: 1` with permission level.
- VibeServerLite APK: SSTV/APRS live decoders only; no LRPT, no gallery capture (gallery viewing of nothing is pointless — omit).

---

## 7. Test plan

1. Capture a real Meteor M2-4 pass on the Sony Android TV; compare decoded output with SatDump on the same IQ.
2. Server decode on Pi 500 under listener load (multiple advanced RDS sessions): pauses/resumes with hysteresis, no public audio underruns.
3. Reboot mid-decode: resumes from persisted checkpoint.
4. Client-assisted: Sony TV captures, iPhone 17 Pro Max decodes from (a) raw stage over LAN, (b) post-Viterbi stage over tunnel. Measure transfer sizes and decode times.
5. Lease: second client sees "being decoded"; kill first client → lease expires → server resumes; first client reconnects later with result → discarded if server finished first.
6. Forgery: modified client returns RS-valid packets with altered content → rejected by spot-check. Dropped/duplicated packet stretch → rejected by continuity check.
7. Background app on iOS mid-decode → pause/checkpoint → foreground resumes.
8. Mode change: switch `sats.json` entry to 80k OQPSK; decoder follows without app release.
9. SSTV during an ISS SSTV event with Doppler lock active.

---

## Review against the codebase (2026-10-07)

**The gallery should start with the decoders we already have.** V12's "decoders built into the app and server" means the
gallery's first content is today's server-side decoders, not only satellites:
- **WEFAX** charts — the app already keeps PREV/LIVE and SAVE; the server could archive charts like the UberSDR addon does.
  ★ A server-scheduled WEFAX capture (from the broadcast schedule) starts at the chart start, so it always gets phasing and the
  header bar — exactly the case our aligner handles best. Lessons that must carry over: `docs/WEFAX-WORLD-STATIONS.md`,
  `scripts/wefax-world/harness.ts`, the conservative "recognised formats only" aligner (RC26), and that UberSDR merged two charts
  into one file and failed late joins entirely (2026-10-07) — chart boundaries must come from start/stop tones + phasing.
- **SSTV** pictures, **NAVTEX** messages (the message box already does one-at-a-time + SAVE), **FT8** spots.

**Existing pieces to reuse**
- **Server Health** already reports CPU/RAM/thermals per server (the health chip); the governor (RADIO-ROLES §5) reads it.
- **Benchmark** (memory `lite_first_setup_benchmark.md`) — the Green/Amber/Red bands.
- **Crash reporter** (`vibeserver/crash_report.h`) — long decode jobs that die must leave a record; Android Diagnostics
  (`VibeExitInfo`) likewise. Export-logs button now lives on the server/directory/About screens (2026-10-07).
- **Thread priority rule** (memory `feedback_thread_priority_order.md`): Network > Audio > Spectrum > Decoders — §3.2's
  `SCHED_IDLE` decode is this rule; capture is not a decoder for this purpose (real-time, normal priority).

**Corrections / additions**
- §3.1 storage: ~180 MB raw IQ per pass on a Pi's SD card is real wear. Default to **soft symbols on SD-card hosts**, raw IQ only
  on SSD/HDD or when the host opts in.
- §5.4 verification: good. Note the existing admin auth is already HMAC(secret, nonce) challenge-response
  (`vs_admin_nonce` / `vs_admin_auth`) — §5.5 "Admin only" reuses it.
- §6 VibeServerLite: Lite *does* run the server-side decoders today (WEFAX/SSTV/NAVTEX live) — keep that; only capture/gallery
  is excluded, as the brief says.
- Watch/Jr: no gallery; at most a "new picture" notification.
