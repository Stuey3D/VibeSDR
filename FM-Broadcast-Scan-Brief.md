# FM Broadcast Scan — Claude Code Handoff Brief

**Status:** design locked, ready to implement
**Part of:** the v10 scanner rollout (alongside Band SNR sweep, Bookmark scan, Manual SSB QSO hopper).
**Related:** VibeServer Multi-Client brief (per-radio model — makes whole-band scanning socially safe on a shared server).

---

## 0. What this is, in one line

One button. It sweeps the entire FM broadcast band, moving the RTL-SDR's RF centre along as it goes, finds every real station, dwells on each just long enough to capture RDS, and saves them all as bookmarks with their RDS data.

## 1. Design principle — it's a composition, not a new engine

This is **not** a fourth scanner. It is the existing scanner pieces wired together with a fixed config:

- **Band SNR sweep** does the *finding*.
- A **dwell** does the *RDS capture*.
- **Auto-bookmark + `mergeBookmarks`** does the *saving*.

The "one button" simply encodes the configuration — FM band edges, channel raster, WFM mode, RDS on, station-validity gate — so the user never sees any of it. **Complexity stays in the engine; the settings stay empty.** The detection engine already selects between probe-dwell / passive-spectrum / local-IQ channel-power automatically; this feature is a preset that drives it, plus the LO-hopping survey layer described below.

---

## 2. The one hard problem: bandwidth vs. band width

An RTL-SDR sees only ~2.4 MHz cleanly. The FM broadcast band is ~20.5 MHz (Region 1: 87.5–108 MHz; Region 2: 88–108 MHz). So the band **cannot** be surveyed in a single capture — the LO must hop across it in tiles. This forces a two-phase design.

Backend applicability:
- **Local IQ (USB RTL-SDR / RTL-TCP)** — we own the LO, so the full tile-survey works.
- **VibeServer** — currently single-client and owns its LO, so it behaves exactly like local USB. **Full survey works here too.**
- **Remote shared receivers (OWRX / Kiwi / FM-DX)** — we can't move their hardware centre, so the fast survey is impossible. A pure **tune-and-dwell** RDS scan across the raster still works, just slower and without the survey pre-filter. This is precisely what the detection-engine abstraction is for: local/VibeServer → tile-survey + dwell; remote → probe-dwell across the raster. **Same button, engine picks the path.** (Remote path can ship as a follow-up if it's simpler to land local/VibeServer first — decide at implementation time; the local/VibeServer path is the headline.)

---

## 3. Phase 1 — Survey (fast, find candidates)

Step the LO across the band in tiles and detect carriers.

- **Tile width:** target ~1.8–2.0 MHz *effective* per tile (sample rate ~2.4 Msps, but usable flat bandwidth is less after filter roll-off). Region 1's 20.5 MHz → roughly 11–12 tiles.
- **Overlap tiles** at their edges. Usable width < sample rate, so without overlap you miss stations sitting near a tile boundary.
- **DC-spike handling (critical).** The RTL-SDR (R820T/R860 tuners) has a DC/LO-leakage spike at *exactly tile centre*, and these tuners have **no offset-tuning** capability. A station detected at tile centre is a phantom. Mitigate by **offsetting each tile so its centre falls between raster channels**, and/or **discarding centre ±~100 kHz** and letting the neighbouring (overlapping) tile cover that slice. Do not report a "station" at DC.
- **Detect carriers:** FFT each tile's IQ, find power maxima above a noise-relative threshold.
- **Snap to raster:** snap each candidate to the broadcast channel raster — **100 kHz (Region 1)** or **200 kHz (Region 2, odd tenths)** — using the existing `ituRegion` + FM-raster logic. Pick the **local maximum within ±raster/2** so adjacent-channel splatter doesn't create duplicate candidates either side of the true channel.

Output of Phase 1: a shortlist of candidate channels (typically ~15–40 real slots, **not** all 205 raster channels). Whole-band survey should complete in a couple of seconds.

---

## 4. Phase 2 — Dwell + RDS capture (the slow part, per candidate only)

For **each candidate only**, tune WFM and capture RDS. Timing is the entire budget — do not dwell on channels the survey already rejected.

RDS timing facts to design against:
- **PI code** (Programme Identification) arrives almost instantly — it's in block A of *every* group. Use it as the fastest "this is a real, locked station" signal and as the stable ID.
- **PS name** (Programme Service, 8 chars) takes ~1–3 s: 2 chars per group-0, interleaved with other group types; longer when the signal is weak.
- **RT** (RadioText) is now-playing text and changes — **do not** store it (see §5).

Dwell state machine, per candidate:
1. Tune WFM to the raster-snapped frequency.
2. Wait for RDS PI lock (fast) → mark as real.
3. Continue capturing until **PS is stable and unchanged for a few consecutive groups** → **early-exit** (don't burn the full ceiling on a strong station).
4. Enforce a **dwell ceiling (~3–4 s)**. On ceiling, save what we have (PI at minimum) and move on.
5. Advance to the next candidate.

Early-exit on stable PS is what keeps strong stations snappy; the ceiling bounds the worst case on weak ones.

---

## 5. Station-validity gate — save *stations*, not noise

Before saving a candidate, require **at least one** of:

- a **valid RDS PI lock**, **or**
- a **19 kHz stereo pilot** present, **or**
- a **sustained local-maximum carrier** clearly above the noise floor over the dwell.

This OR-gate rejects the three things that would otherwise pollute the bookmark list:
- **Front-end images** of strong local transmitters (no PI/pilot, or not a sustained true local max).
- **Adjacent-channel splatter** (handled by taking the local max within ±raster/2).
- **Dead spectral bumps** (no PI, no pilot, not a sustained carrier).

Note: the pilot must be an **OR**, not a hard requirement — genuine **mono community stations** have no stereo pilot but are real. PI-or-pilot-or-strong-carrier covers them.

---

## 6. Bookmark payload

For each saved station:

| Field | Source | Notes |
|---|---|---|
| frequency | raster-snapped candidate | the channel, not the raw FFT peak |
| name | RDS **PS** | fall back to `FM <freq>` if PS never locked |
| PI (hex) | RDS **PI** | **stable ID + dedupe key** |
| PTY | RDS **PTY** | store as a tag |
| SNR | detection SNR | useful for sorting/quality |

- **Skip RT** — it's rolling now-playing text, not station identity.
- **Dedupe on PI:** a strong station bleeding onto two adjacent frequencies must not double-save. PI is the dedupe key; keep the stronger-SNR instance.
- **Batch tagging:** tag the whole run with a source/group (e.g. `FM Scan`) so the user can **review or bulk-clear** the batch. Merge into existing bookmarks via the existing **`mergeBookmarks`** path so a re-scan updates rather than duplicates.

---

## 7. UX — one button, stays one button

- **Single action** starts the scan. No band pickers, no mode switches, no thresholds exposed (band edges/raster come from `ituRegion`).
- **Live-populating found-list:** stations appear as they're confirmed, so the user sees progress and results building in real time.
- **The sweep itself is the progress indicator** — show the band position moving across the FM band as the LO hops; no separate progress bar needed.
- **Cancel keeps what's been found** — cancelling mid-scan retains everything confirmed so far (partial scans are still useful).
- Colourblind-safe / accessibility per the app's existing conventions (no state by red-vs-green; position + label + dB numbers).

---

## 8. Implementation order (suggested)

1. **Survey layer** (Phase 1) on local IQ: tile plan with overlap + DC-offset, FFT carrier detection, raster snap, candidate shortlist. Verify against a known local FM band plan — every strong local station found, no DC phantoms, no splatter duplicates.
2. **Dwell + RDS** (Phase 2): WFM tune, PI lock, PS capture with early-exit + ceiling.
3. **Validity gate** (§5) + **bookmark payload/dedupe** (§6), wired through `mergeBookmarks` with the `FM Scan` batch tag.
4. **UX:** one-button entry, live found-list, sweep-as-progress, cancel-keeps-found.
5. **VibeServer path:** confirm it behaves as local IQ (it owns its LO); no separate work expected beyond the backend abstraction already selecting the local path.
6. **(Follow-up, optional)** remote OWRX/Kiwi tune-and-dwell path (no survey), if not landed in the same version.

---

## 9. Gotcha checklist (things that will bite in testing if skipped)

- DC-centre phantom "station" every tile → offset tiles / discard centre ±100 kHz.
- Stations near tile edges missed → overlap tiles.
- Duplicate candidates either side of a real channel → local-max within ±raster/2.
- Same station saved on two frequencies → dedupe on PI.
- Mono community station rejected → pilot is OR, not required.
- Weak station burning full dwell → PS-stable early-exit + hard ceiling.
- RT saved and looking like a changing "station name" → don't store RT.
- Region 2 stations on even tenths → raster is 200 kHz, odd tenths only.
- On a shared VibeServer: scan must only hop the **scanning user's own dongle** — this is guaranteed by the per-radio model (see VibeServer Multi-Client brief §10); no action needed here beyond not assuming a shared centre.
