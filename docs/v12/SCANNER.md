# SCANNER — Band search, Bookmark scan, Manual QSO scan (the SSB hopper) — V12

**Source:** Stuart's `BRIEF-scanner.md` (July 2026, written for v10 as an app-only feature). This file is that brief
brought up to date for V12. Stuart's design is kept as he wrote it; **only what the platform change requires is
changed, and every change is marked ▶ PROPOSAL (for Stuart) with the reasoning**, or ✓ STUART (decided 2026-10-27, via
the coordinator) where he has already answered.

**Branch:** `v12-scanner` (from `v12-experimental`), worktree `~/VibeSDR-scanner`.
**Status:** M1 (engine core, probe coverage) BUILT in the app — see §15. M2 / M3 and the server coverage class wait for
VFO A/B's watch channels (`v12-dual-vfo`) and for Stuart's answers to §0.

---

## 0. Proposals and questions for Stuart (read this first)

| # | Question / proposal | Recommendation |
|---|---|---|
| **A** | A 4th coverage class, `server` — the VibeServer computes each tag's channel power and floor on the server, from the FFT every listener already shares, and pushes it per tag every frame (§5.6). | **Yes, as M2-S** — after dual VFO lands. Exact, every frame, works on the web too, ~nothing on a Pi 2. |
| **B1** | VibeServer's **shared-dial contract** is added alongside the OWRX rule — ONE clamp / refuse / PAUSED mechanism serves both (§4.4). On a range-locked VibeServer the scanner hops only inside the owner's window; on a single-user radio it may retune the hardware. | Built that way in M1. |
| **B2** | On a VibeServer with **one shared dial** the scanner is **absent** (your DAB-scan rule: "antisocial"). Once watch channels exist, a scan could run on the listener's own WATCH channel instead, leaving the shared dial alone (scanning *inside* the capture window). Offer it then? | **Yes, with M2-S** — it is exactly what the watch channel was made "scanner-ready" for. Until then, absent. |
| **B3** | An **Icom** on a VibeServer: not offered in M1 (a CI-V retune per hop is untested; the rig may be in its owner's hands; listeners follow the radio by default). | Leave out until tested on the IC-7300. |
| **C** | The brief excluded **VibeServer's web client**. With the server doing the detection (A), the web client gets the scanner nearly for free — the web client is the moat. Include it? | **Yes, after M2-S**: the web scanner should be server-backed like the decoders, not a second JS engine. Open question. |
| **D1** | **Priority watch on a watch channel** — a starred tag *inside the capture window* is held on VFO B, whose squelch-open event (already on the wire) IS the priority interrupt: zero probing, zero audio gaps (§6.4). | Yes, M2-S. |
| **D2** | **"Listen to one, watch another"** — HOLD on a stop while VFO B keeps scanning the rest; the found list fills from both. | Yes, M2-S; needs `kMaxPerListener` > 1 for more than one watcher. |
| **D3** | **Binaural / stereo**: a priority interrupt or a found signal plays in the ear its frequency says (low left, high right), using VFO A/B's pan mixer — the ears tell you where the new voice is before you look. | Yes, after dual VFO (it is the mixer's own rule). |
| **D4** | **Bookmarks**: a found signal → bookmark goes to the user's bookmarks (built). Proposal: a VibeServer **owner's** found list → "Add to server bookmarks" (the 365-day *scanned* expiry class), and a listener stop on an unnamed frequency that later decodes a station name (RDS/DAB/learned) feeds the 30-day *learned* list as today. | Owner-only, M2. |
| **E1** | The brief's `BackendCapabilities.scan` was replaced by two pure functions, `scanPolicy()` / `scanTiming()` (testable without a socket; one rule the key and the engine share). | Keep. |
| **E2** | The brief's `onViewSpanChanged` event was replaced by **reading the reach on every tick** (`tuneBounds ∩ panSpan` + `activeProfileId()`), so every backend is covered with one tiny adapter addition. | Keep. |
| **E3** | `Auto` needs a floor in level units. The waterfall's auto-range floor now lives in a GPU worklet (WaterfallView's SignalProcessor) and measures the *view*, not the channel. M1 uses the auto squelch's own reference (`channelExcessDb`, the empty neighbours), then the tracked floor, then — only on a backend whose view cannot answer (a zoomed-out Kiwi) — the quiet quartile of what the sweep itself read. | Keep; it is the existing estimator wherever one exists. |
| **E4** | Default **"Time limit per signal" is OFF** (your rule). So in a band scan a continuous carrier holds the stop until SKIP / IGNORE — the "Ignore?" hint flags it. Should BAND mode default the limit ON (e.g. 30 s)? | Your call — the brief's acceptance item 1 ("max-dwell escapes a continuous carrier") assumes the tick is on. |
| **E5** | Skip is a **soft ignore for 3 minutes** in every mode (your band-scan rule). In MANUAL that means a skipped QSO is not revisited for 3 min. OK, or should MANUAL's Skip only move on? | Keep one meaning everywhere unless you say otherwise. |
| **E6** | Band picker steps: the band plan's *tuning* step (40 m = 500 Hz) is not a *scan* step. Scans use one passband apart for SSB (2.5 kHz), 500 Hz for CW, 5 kHz for SW broadcast; channelised bands keep theirs (CB 10 kHz, airband 25 / 8.33 kHz). | Keep. |

---

## 1. Goal & design principles (Stuart's — unchanged)

Three modes:

1. **Band scan** — sweep a frequency range, stop on signals above threshold, resume when they drop, log everything found.
   ✓ STUART 2026-10-27: *"should behave like a traditional scanner, enter frequency range to scan, let it do its thing,
   give it the ability to ignore signals such as noise spikes or VOLMET so it doesn't get stuck."* So **Lo / Hi (+ step,
   mode) is the primary input**; the band picker is a one-tap shortcut that fills those fields; on a bounded span (an
   OWRX profile, a locked VibeServer window) they pre-fill from it. Start, and leave it.
   ✓ STUART: *"it builds a list of found frequencies with a simple ignore button, so that permanent noise spikes or
   VOLMET cannot trap it."*
2. **Bookmark scan** — hop across a bookmark group, stopping on active channels.
3. **Manual scan (QSO hopper)** — **the headline feature.** Tag an SSB conversation, tag another — across bands and
   demodulators — and the scanner hops between them, dwelling on whichever is active. A tag is a full VFO snapshot,
   not a channel number.

### 1.1 Prime directive: complexity lives in the engine, never in the settings

- The user is never asked to choose a detection technique. The engine selects per tag, per backend, per view.
- Stop threshold defaults to **Auto** (noise floor + margin).
- Threshold units are **whatever the user's meter already shows**.
- Exactly **three live controls** while scanning: **Hold, Skip, Ignore** (✓ STUART: "Ignore", not "Lockout", one word
  everywhere).
- Power features are each one visible control: priority = a **star**, discovery = the **found list**, recording = one tick.

### 1.2 The differentiator

Passive monitoring of tagged passbands from data the client already has (M2), and exact per-tag channel power where
the samples are ours (M3 — local IQ; and, new in V12, the **server** — §5.6). The probe path (M1) remains the permanent
fallback and has to be right, not provisional.

---

## 2. Data model

`src/services/scanner.ts` (pure TS — engine, types, presets), `src/components/ScannerPanel.tsx` (the body of the
decoder box), `src/hooks/useScanner.ts` (the host: tune, the native gate, the recorder, VoiceOver, the clock),
`src/services/scanPresets.ts` (AsyncStorage).

`ScanTag`, `ScanPreset`, `FoundSignal` are the brief's, with two changes:

- ✓ STUART — **`ScannerConfig` has two independent triggers, not a mode picker** (*"the scanners need triggers for moving
  to the next point, so the dwell on the signal until it stops, and/or a time limit per signal"*):
  `untilStops` + `hangMs` (default ON, 5 s, 1–15 s) and `timeLimit` + `timeLimitMs` (default OFF). Alone = classic
  timed dwell; both = whichever comes first (the brief's activity capped by maxDwell). At least one is always on:
  unticking the last turns the other on (`setTrigger`). The brief's `advance / maxDwellMs / timedDwellMs` are migrated
  on read (`normaliseConfig`).
- The manual threshold is stored **per meter family** (`{ snr, dbfs }`) as §5.2 required.
- `FoundSignal` gains `dwellMs` and `capped` (stops that ended on the time limit) — what drives the "Ignore?" hint.

Do **not** change the `UserBookmark` schema (`src/services/userBookmarks.ts:18` — verified, still there). A bookmark
without stored bandwidth gets the mode's default passband (`bookmarkTag`).

---

## 3. Scope

### In scope (unchanged in spirit)
State machine on the backend's `tune()`; activity metric in the user's meter units, Auto default; the coverage classes
with automatic selection; three candidate sources; the panel; hard audio gating; OWRX safety **and the VibeServer
shared-dial contract** (§4.4); presets; media keys; VoiceOver from day one.

### ✓ STUART — placement: **"scanner is going to be another decoder box item."**
SCANNER sits in **Mode → CLIENT DECODERS** beside RTTY / NAVTEX / CW…, and lives in the **same decoder box**
(DecoderPanel): same open / minimise / BIG, same pad and keyboard rules, title SCANNER, with **BAND · BOOKMARKS · MANUAL**
as keys in the box's header. No second entry point. One box at a time: opening SCANNER **replaces** a running decoder
(and opening a decoder stops the scan), exactly as switching decoders always has. On OpenWebRX — whose own decoders are
demodulators in the Mode sheet — the CLIENT DECODERS row appears with SCANNER alone.

### Out of scope (Stuart's — unchanged; these are wrong behaviours, not deferred features)
- ❌ AFC / drift tracking on tagged SSB QSOs.
- ❌ Auto-zoom or any scanner-initiated view movement. Scanner tunes always pass `recenter: false`.
- ❌ OWRX profile switching by the scanner, under any circumstance. The scanner **must be incapable** of emitting a
  tune outside the reach (§4.4). ✓ STUART: *"we still support OpenWebRX so that does apply, this scanner is for all back
  ends we support."*
- ❌ FM-DX — permanently (one physical tuner shared by every listener).
- ❌ ▶ PROPOSAL **B1/B2** — likewise **a VibeServer radio with one shared dial** (its dial is every listener's).
- ❌ Scan-list import/export; bookmark schema changes; persisting the session tag list or found list.
- ~~Excluded: VibeServer's web client~~ → ▶ PROPOSAL **C** (open question).

---

## 4. ScannerController state machine

```
IDLE → (gate shut, tune) → SETTLING → SAMPLING → STOPPED (gate open) → hang / time limit / Skip / Ignore / priority → hop
                                         ↓ nothing sustained → hop
HOLD   = STOPPED with every timer frozen (Hold, or play/pause; or the user turning the dial during a stop)
PAUSED = candidates out of reach — keeps its place, resumes by itself
```
The brief's ADVANCING is the instant between a decision and the next tune, so it is a transition, not a state.

- **SAMPLING** ✓ STUART (noise spikes never stop it): a stop needs **sustained** activity — ≥ 3 consecutive samples
  above threshold spanning ≥ 200 ms (`SUSTAIN_N`, `SUSTAIN_MS`, tuned in the tests, not settings). A run still building
  when the window ends gets 450 ms more (`SAMPLE_EXTEND_MS`) so a 5 fps Pi 2 or OpenWebRX is not missed. A sample
  flagged as a **broadband impulse** (a static crash or ignition noise lights the whole span: the row's median jumps ≥ 6 dB
  over its baseline — `ImpulseDetector`) is ignored entirely: it is evidence about the band, not this channel.
- **STOPPED** ★★ unchanged and critical: in "until the signal stops", the signal falling below threshold (2 dB of
  hysteresis) starts the hang; **the signal coming back cancels it** — that is what keeps SSB over-changes from dumping
  the QSO. The time limit (if ticked) runs from the stop. A priority interrupt exits STOPPED immediately; HOLD is exempt.
- **HOLD** freezes both timers; Hold again resumes the stop with its timers where they were (or re-samples, if held
  mid-sample); Skip moves on. A person turning the dial ≥ 2 kHz off a stop for 3 frames → HOLD ("you tuned away"); a
  tap on a list row holds before it tunes.

### 4.1 Candidate iteration
Band: `lo + n·step`, wrapping. Lists: list order, wrapping. Ignored candidates are passed over; Skip's soft ignore lasts
3 min (`SOFT_SKIP_MS`) and gives way only when nothing else is left.

### 4.2 Adapter surface
▶ PROPOSAL **E1/E2** (built): instead of `BackendCapabilities.scan` and a new `onViewSpanChanged` event —
- `scanPolicy({kind, isLocal, sharedDial, lockedCentreHz, icom})` → `{offered, why, movesHardware}` — the SCANNER key is
  absent where `offered` is false (no dead control);
- `scanTiming(kind, isLocal)` → `{settleMs, sampleMs}` (§5.3);
- ONE optional method on `SDRBackend`: `activeProfileId?()` (OWRX: `sdr_id|profile_id` from every `config`, whoever
  switched it). `OwrxAdapter.onConfig` is at `OwrxAdapter.ts:583` (the brief said :502); `BackendCapabilities` is at
  `SDRBackend.ts:127` (the brief said :106).

### 4.3 Range validation — one function, three call sites
`validateCandidates(candidates, reach)` at start, on every reach change, and on preset load (`checkReachable()`).
Anything out of reach at start/load → **refuse**, list it, offer **START WITH N REACHABLE** as an explicit action — never
silently scan a subset. A band partly outside the reach offers the part inside.

### 4.4 ONE clamp for OWRX and VibeServer — the reach
▶ PROPOSAL **B1** — the shared-dial contract is added *beside* the OWRX rule, through the same mechanism:

The host answers `reach()` = **where the scanner may tune right now**, read afresh on every tick and again immediately
before every tune (`sendTune` — the only place a tune leaves the scanner):

| Backend | reach | key (a change re-validates) |
|---|---|---|
| OpenWebRX | active profile span (`panSpan()`) ∩ `caps.freqRange` | span + `activeProfileId()` |
| VibeServer, range-locked | the owner's locked window (`utils/tuneWindow.ts` `tuneBounds`) | the window |
| VibeServer, single-user | the radio's range (hardware may move) | the range |
| VibeServer, shared dial | — `allowed: false` → not offered / PAUSED | — |
| UberSDR / KiwiSDR | `caps.freqRange` | the range |
| Local USB / rtl_tcp / SpyServer (movable) | the whole tunable range (the dongle follows) | the range, never the moving view |
| Whole-band modes running (DAB / ADS-B / ISM) | `allowed: false` → PAUSED | — |

- A reach change (another listener switching the OWRX profile; a VibeServer owner moving the locked window; the radio
  becoming a shared dial) that leaves any candidate out → **PAUSED** on the next tick, the audio opened, the panel lists
  what is out of reach, VoiceOver announces it; when everything is reachable again → **auto-resume from the same place**.
- The user's own in-app profile switch (`noteLocalSwitch`) re-latches: one that still covers the list keeps scanning;
  one that does not behaves like start-time validation (refused).
- OWRX's adapter **no longer** auto-switches profile on an out-of-window tune at all (it clamps to the edge —
  `OwrxAdapter.ts:1237`; the brief's `:8` reference described code that has gone). The scanner never relies on that.

---

## 5. Activity engine

### 5.1 Threshold tied to the user's signal meter
`signalMode` is at `SDRScreen.tsx:3692` (brief: :1075); OWRX and Kiwi force `snr → smeter` at `:4001` (brief: :1273).
The scanner is fed **the same numbers the meter shows**, from the same frame (`levelDbm` with the visual trim, `snrDb`).

### 5.2 Comparator — two families
`snr` → SNR dB (UberSDR/VibeServer: the server's channel SNR — the `VibeSignal` listener, `audioSnrRef = e.snr − 30`,
`SDRScreen.tsx:5661`; the brief's :1060). `smeter | dbfs` → the absolute level (dBFS; a Kiwi's or OWRX's S-meter dBm).
The S-meter mapping is no longer `dbfsToSMeter` in ControlsBar (gone): it is `constants/meters.ts`
(`S9_DB = −73`, 6 dB per S-unit, `sMeterText` / `formatReading`). The S-meter stop level steps in S-unit detents
(S1…S9, S9+10/20/30/40 — `sMeterDetents`).

### 5.3 Settle / sample (initial values)
| Backend | metric on tune | settle | sample |
|---|---|---|---|
| UberSDR | radiod channel SNR / power push | 250 | 300 |
| **VibeServer** (new) | the listener channel's `sig.chan` / SNR every frame | 250 | 400 |
| KiwiSDR | S-meter (SND header) | 400 | 500 |
| OpenWebRX | S-meter (JSON) | 500 | 500 |
| Local USB / rtl_tcp / SpyServer | derived | 100 | 500 |

### 5.4 Auto threshold
Level units: floor + margin (6 dB), the floor being — ▶ PROPOSAL **E3** — the tag's own `noiseRefDb` (captured at tag
time), else this frame's neighbour reference (`channelExcessDb`, `services/squelchNeighbours.ts` — the auto squelch's
"reference the signal cannot be in"), else the tracked floor (`floorEmaRef`, UberSDR/VibeServer), else the last known,
else the 25th percentile of the sweep's own first readings (needs 8). SNR units: Auto = the margin. The panel shows the
resolved threshold live ("S5 auto", or "measuring the noise floor…").

### 5.5 Coverage classes — automatic selection
| Class | Technique | When |
|---|---|---|
| `server` ▶ **NEW (A)** | the VibeServer computes each tag's channel power + floor from the shared FFT, every frame | VibeServer && tag inside the capture window |
| `local` | VibeDSP channel power from raw IQ (§9) | on-device paths && tag inside Fs |
| `passive` | passband energy from the rows the client draws (§8) | tag visible && bin ≤ passband/2 |
| `probe` | tune, settle, sample, return — universal fallback | everything else |

`server` ⊃ `local` ⊃ `passive` ⊃ `probe`. M1 is probe only: the coverage dot is amber everywhere ("checked periodically").

### 5.6 ▶ PROPOSAL A — `server` coverage on VibeServer

**Why:** on a VibeServer the samples are ours, on the server. Every listener already shares one forward FFT; the CW/PSK
skimmers and Band spots slice it per listener (`clientCentreBin` / `extract`, `local_sdr_shim.cpp:8736`, :25119), and
the server already sends each listener `{"type":"sig","chan":…,"floor":…}` for its own channel (:12536). VFO A/B's watch
channels compute "levels + squelch off the wide row" for each watch channel (`watchOnFrame`, branch `v12-dual-vfo`).
The scanner needs exactly that for N tags.

**Wire (proposal):**
```
client → {"type":"scanwatch","tags":[{"id":"t1","hz":14205000,"lo":50,"hi":2700}, …]}      // ≤ 32, inside the window
client → {"type":"scanwatch","tags":[]}                                                     // off
server → {"type":"scanpow","r":[["t1",-87.2,-112.0],["t2",-111.8,-112.3], …]}               // id, chan dB, floor dB
server → {"type":"scanwatch","refused":"outside the capture window","ids":["t3"]}            // the reach, server-side
```
`scanpow` goes on **the listener's own frame clock** (like `sig` — never the engine's 20/s to a 5 fps listener). The
floor is the server's own per-tag neighbour floor (the same rule as `squelchNeighbours.ts`), so the client compares in
exactly the numbers its meter shows. Tags outside the window are refused by the SERVER (the shared-dial contract: the
server states the truth, the client adopts it) and fall back to `probe` — only on a radio that is the listener's alone.

**Cost (Pi 2, estimated, to be measured):** per tag per frame a band-sum over its bins in the already-averaged row
(SSB at ~200 Hz bins ≈ 12 bins; + 4 neighbour windows) ≈ 60 adds → 32 tags ≈ 2 k adds per frame per listener — under
0.1 % of one Cortex-A7 core at 10 fps. No extra FFT, no extra thread. Wire: ~16 bytes per tag per frame →
8 tags at 10 fps ≈ 1.3 KB/s. RAM: a few hundred bytes per listener. Nothing runs for a listener with no tags.

**Then, via watch channels (D1/D2):** the priority star inside the window rides on VFO B (its squelch-open event is the
interrupt — no probe, no gap); on a shared dial the scan itself can run on the listener's watch channel without ever
touching the shared dial (B2).

---

## 6. Candidate sources

### 6.1 Band scan + found list
✓ STUART: Lo / Hi / step / mode are the primary fields; **BANDS** fills them in one tap with the regionally correct values
(`bandChoices`, from `BAND_PLAN` `constants/bandPlan.ts:26`; 11m CB is `:67–68` — NFM 10 kHz in Regions 1/3, AM in
Region 2; `bandTuneDefaults` is `:235`, `getBandsAtRegion` `:219`). Bands the receiver cannot reach are absent; a band
partly in reach is offered clipped. On a bounded span (≤ 5 MHz — an OWRX profile, a locked window) Lo / Hi are seeded
from it, so an airband profile is START alone. On a wideband receiver they stay empty and required.

**Found list** — ✓ STUART: every stop logs a row, newest first:
`128.590 MHz – London South Information Broadcast   [IGNORE]`. **IGNORE is the row's one obvious key**; tune, → Tag and
→ Bookmark are secondary (tap the row). Ignored rows move to an **IGNORED** section, greyed, with UN-IGNORE. A frequency
that keeps trapping the scan — ≥ 4 stops, ≥ 2 min dwell, or ≥ 2 time-limit escapes — shows **IGNORE?** (a hint; nothing
is ignored without the user's tap). Spikes never create a row (they never stop the scan).

✓ STUART — **names beside frequencies**: wherever the scanner shows a frequency (found rows, the status line, tag rows,
the VoiceOver announcement — "Stopped on 128.590 megahertz, London South Information"; unnamed, the mode instead: "Stopped on 14.205 megahertz USB"), the best name within the
channel (|f − bookmark| ≤ half the scan step, or the tag's passband half-width): the user's own bookmarks first, then
the server's (incl. learned / scanned), then EiBi's schedule; nearest within a source wins. The name truncates with an
ellipsis on a phone, never pushing IGNORE off-screen. *Built-in airband names: `briefs/BRIEF-airband-geo-frequencies-v2.md`
is not implemented yet — the lookup takes it as a third source (rank 2) the day it lands.*

### 6.2 Bookmark scan
Group picker over this receiver's bookmarks (`bookmarksForInstance` scope rules). Stars and ignores work as in manual.

### 6.3 Manual scan (QSO hopper)
TAG snapshots the VFO (frequency, mode, passband) and the floor there (`noiseRefDb`, from the neighbour reference — so
the threshold is relative to the tag's own floor, never to the strong station's level at tag time). Hang 5 s default,
1–15 s. Rows: label, freq / mode, coverage dot, ★, IGNORE, ✕.

### 6.4 Priority — one star
Probe coverage (M1): a starred tag gets a priority probe every 5 s while the scanner sits on something else (skipped
while recording and in HOLD); while scanning, a due star is visited out of turn. Found active (sustained) → **priority
interrupt**: the stop is abandoned for the star (its recording ends), announced "Priority — …". ▶ D1: under `server`
coverage the star costs nothing and interrupts within a frame of the first syllable.

---

## 7. UI

### 7.1 The decoder box
✓ STUART placement (§3). Body: status line (state, frequency – name, threshold) · PAUSED / refusal banner (with START
WITH N REACHABLE) · Undo line · **START/STOP · HOLD · SKIP · IGNORE** (+ TAG in Manual) as 44 pt keys · the source
section · FOUND / IGNORED · SETTINGS (☑ Until the signal stops + HANG · ☐ Time limit per signal + LIMIT · Auto
threshold or STOP AT in the meter's units · ☐ Record on stop) · SCANS (SAVE AS SCAN…, saved, starters with CLONE).
The box's list zone walks every control (pad / keyboard: arrows move, Enter presses, Back leaves); Lo / Hi / the name
open the controller keyboard on a pad (`usePadField`).

### 7.2 Coverage indicator & waterfall
M1: every tag's dot is amber (probe). The waterfall tag markers / sweep indicator are **not built in M1** (they come
with M2's coverage, where they mean something). The main meter stays live throughout, so the user watches what the
scanner sees.

### 7.3 Record-on-stop
Each stop starts the existing recorder (`startRecording(freq, mode)`), stopped on leaving the stop; the file is renamed
`VibeSDR_<time>_<freq>_<MODE>_scan.m4a` (the Recordings list reads the first four fields, so it lists it as usual) and
indexed under the server. Probes are suspended while recording. A manual REC press first ends the scanner's file and
waits for it — one recorder, never two; record-on-stop stands aside while a manual recording runs.

### 7.4 Audio gating
The persistent native gate (`VibePowerModule.setSquelchOpen`, the effect at `SDRScreen.tsx:4293`; brief: :1472) is shut
while hopping and open on STOPPED / HOLD / PAUSED / IDLE. It is **shared with the Kiwi's client squelch** — open only
when neither wants it shut (`applyNativeGate`). Stop / unmount / disconnect always open it.

### 7.5 VoiceOver — from day one
Every key has a label and hint; steppers are `adjustable`; rows read as sentences (frequency, name, mode, priority,
ignored, "checked periodically"); every state change is announced (`AccessibilityInfo.announceForAccessibility`):
"Scanning", "Stopped on … ", "Priority — …", "Held on …", "Ignoring …", "Scan paused — receiver profile changed",
"Scan resumed", "Scan stopped".

---

## 8. Passive spectrum monitoring — M2 (unchanged from the brief)
Per row, per passive tag: mean power over `[f+bwLow, f+bwHigh]` minus its floor, ~750 ms sustained debounce. Gate per
tag: inside the visible span and bin ≤ passband / 2. Never auto-zoom. On a VibeServer, `server` (§5.6) supersedes it.

## 9. Local-IQ channel power — M3 (unchanged)
VibeDSP band-sums per tag per frame (NEON), window-clustered sweep for out-of-window tags, ≥ 2 s window dwell. iOS:
`modules/vibe-local-sdr/build_ios.sh` before archiving (verified present). ▶ Note: the same C++ (`vibe_watch.h` level
off the wide row) can serve M3 and `server` — one implementation, two hosts.

## 10. Persistence
Presets: AsyncStorage `lsv_scan_presets` (versioned JSON), global across receivers, validated at load. Ignores and stars
persist inside presets. Three bundled starters (discrete sets only): **VOLMET (Europe)** (Shannon 3413/5505/8957/13264,
RAF 5450/11253 kHz USB), **Time Signals** (WWV/WWVH, CHU, RWM), **Marine HF distress & safety** (2182, 4125, 6215, 8291,
12290, 16420 kHz USB — ITU RR Appendix 15). Read-only; CLONE to edit. Session tag list and found list: in the screen's
controller ref only (survive backgrounding, die with the app; precedent: session-scoped NR/notch, `SDRScreen.tsx:1366`).

## 11. Media controls
While engaged: **next-track → Skip** (from HOLD: resume; from PAUSED: try to resume); **play/pause → Hold toggle** (the
native side's mute is undone at once — Hold means listen here — and an OWRX pause does not drop the socket mid-scan);
previous-track does nothing. Done without native changes: while engaged the native skip mode is set to `bookmark`, which
hands ⏭ to JS on every backend. Idle: your skip mode is restored.

## 12. Per-backend summary
| Backend | Scan? | M1 coverage | Later | Notes |
|---|---|---|---|---|
| UberSDR | ✅ | probe (fast: pushed SNR) | passive | full 0–30 MHz |
| KiwiSDR / Web-888 | ✅ | probe | passive (zoom-gated) | S-meter dBm |
| OpenWebRX / OWRX+ | ✅ | probe, **inside the active profile only** | passive | PAUSED on someone else's profile change; never switches |
| VibeServer, range-locked | ✅ | probe, **inside the owner's window** | **server** (A), watch channels | never moves the radio |
| VibeServer, single-user | ✅ | probe, may retune the radio | server | it is yours |
| VibeServer, shared dial | ❌ (absent) | — | ▶ B2: on the listener's watch channel | antisocial otherwise |
| VibeServer, Icom | ❌ (M1) | — | ▶ B3 | untested CI-V hop rate |
| Local USB / rtl_tcp / SpyServer | ✅ | probe, retunes the dongle | local (M3) | |
| FM-DX | ❌ never | — | — | one tuner for every listener |

## 13. Acceptance criteria (Stuart's, with V12 additions)
1. Zero-config airband on an OWRX profile (bounds/step/mode seeded; stops; hang; found list; one-tap Ignore sticks;
   the time limit escapes a carrier — with the tick on, see E4).
2. QSO hopper: per-tag mode + passband, no audio on retune, a 3 s over-change does not advance (hang 5 s),
   noiseRef-relative hold.
3. Instant jump under passive (M2).  4. Local IQ (M3).
5. Priority interrupts a stop under passive (M2) and **probe** (M1); HOLD suppresses it.
6. Record-on-stop: correctly named clean recordings.
7. OWRX safety — **and VibeServer's window / shared dial**: PAUSED within one config message, banner, auto-resume, no
   out-of-reach tune ever.
8. Threshold UX on every backend family; per-family manual thresholds; S-unit detents.
9. Band picker regional values, unreachable bands absent.
10. VoiceOver end to end.  11. Hygiene (gate never left shut; media keys; session vs presets).
12. ✓ STUART: spikes never stop it or fill the found list; a carrier is escaped and offered for Ignore; ignores persist;
    an intermittent signal still stops reliably; names beside frequencies; the name truncates, Ignore stays.

## 14. Implementation order
**M1 — engine core** (built, §15). **M2-S** — `server` coverage + watch-channel priority (after dual VFO + A/B2/D1).
**M2** — passive coverage, coverage dots, smart ADVANCING, spectrum-peak band sweep, waterfall markers. **M3** — local IQ.
**Web** — the scanner in the web client's decoder box, server-backed (C).

## 15. M1 as built (2026-10-27)

| Acceptance | Status | How it is proven |
|---|---|---|
| 1 zero-config airband | ✅ engine; ⏳ device | `test_scanner`: OWRX airband band scan stops untouched; found list; Ignore persists; time limit escapes a carrier, "Ignore?" offered |
| 2 QSO hopper | ✅ engine; ⏳ device | 3 tags / 2 bands / 3 modes; mode + passband on every tune; gate shut on every hop; 3 s gap held; noiseRef −120 holds a +8 dB weak station after a +40 dB strong one |
| 5 priority (probe) | ✅ | star probed while stopped, every probe returns; interrupt when live; also under time-limit-only; HOLD suppresses |
| 6 record-on-stop | ✅ engine; ⏳ device | start with the target, stop on leaving, no probes while recording, manual REC left alone |
| 7 OWRX safety (+ VibeServer) | ✅ | hijack → PAUSED in one tick, banner lists all, nothing tuned while paused, auto-resume; own switch → refusal; fuzz (400 random reach changes, skips, holds, restarts): **0 tunes outside the reach**; VibeServer window moved → PAUSED/resume; shared dial → PAUSED with the reason |
| 8 threshold UX | ✅ | Auto untouched stops on UberSDR, Kiwi, VibeServer, OWRX; per-family manual; S-meter detents |
| 9 band picker | ✅ | CB R1 NFM / R2 AM 10 kHz, exact bounds; no 2 m on 0–30 MHz; airband offered on VHF |
| Stuart's band rules | ✅ | broadband crashes every 700 ms: 0 stops, 0 found; one-frame spikes: 0 found; an ATC-like 5 s over every 15 s with crashes: caught ≥ 4 of 5, nothing else found |
| triggers | ✅ | until-stops only / time-limit only / both (each way) / Hold freezes both |
| names | ✅ | name shown; nearest wins; user's over server's; none → frequency; VoiceOver sentence |

`scripts/test_scanner.ts` — 148 checks, registered in `scripts/run-tests.sh`. Typecheck clean.

**Known M1 limits (said plainly):** probe coverage visits a band channel only once a pass, so a very short call on a
long band can be missed — M2/`server` fix that. On local hardware in **SNR** meter mode with the view *unlocked*, the
local SNR (like the meter's own) is measured at the view centre, not the VFO — dBFS mode, or the view following the VFO
(the default), is correct; M3 replaces it. The 390 pt layout rule is pinned in the source by the test; it still needs a
look on a real phone with a long bookmark name. Nothing has been run on a device yet.
