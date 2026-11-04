# The idle DX scanner and the DX Log (V12)

**Status:** built 2026-11-04 on `v12-dx-scanner` (off `v12-experimental`), from Stuart's brief (`BRIEF-dx-scanner.md`).
Not merged, not released. **Related:** `BACKGROUND-DECODING.md` (the idle/yield framework this is "just another task"
of), `ANDROID-SETTINGS-PARITY.md` (the phone's server screen), `RADIO-ROLES.md` (the dedicated role, not built yet).

When a radio is idle, VibeServer slowly sweeps Broadcast FM (87.5–108 MHz) and DAB Band III (all 41 blocks, 5A–13F with
the N blocks) and keeps what it hears in a **DX Log**, apart from the bookmarks. Its job is to catch **transient**
stations — Sporadic-E, tropo, meteor scatter. Strong locals are logged once, then stay out of the way.

## What the owner sees

Setup → Radio, under Background Decoding (and the same block on the Android server screen — Server mode and Lite):

> **DX Scanner** — catch Sporadic-E, tropo and meteor scatter while the radio is idle. When nobody has been listening for
> [5 minutes] the radio slowly sweeps [Broadcast FM and DAB / FM only / DAB only] and keeps every station it identifies …

- On/off per radio, **default off**. Under the switch (the decoder rule — options sit under their toggle): the wait
  (default 5 min), the bands, the **FM channel raster** (100 kHz, or 50 kHz for regions that use it — drawn only while FM
  is swept) and **Only the owner may view the DX Log** (default off: open).
- **Branches on the driver** (AGENTS.md): not drawn on an Icom (no IQ); an Airspy HF+ is offered FM only (no band menu).
  The page says how many FM hops this radio makes at its rate ("At 2.400 MS/s FM takes about 12 hops of ~4 s").
- **The benchmark sets the default, never hides**: below green on the new `dec_dxscan` row the block says "Not
  recommended on this box: … It will still run (in smaller steps …)". It is still allowed.
- Stored as ONE JSON blob, `RadioConfig::dxScan` — `{"on","bands","waitMin","raster","viewAdmin"}` (+ `proof`, `gapMin`
  kept as saved, not drawn) — exactly as `bgDecode` is. Applied live on a save, and from the radio process's config watch.
- Admin status: `dxScan` — `state` (off / waiting / scanning / listener / blocked + `why`), what it is doing now, the last
  visit's line, the last hop's wall/CPU/peaks, the mean DAB block time, the last whole-sweep times, entries and sweeps.

## What a listener sees

**DX LOG** beside **GALLERY** under VIEWS — the web client's DECODERS panel and the app's mode sheet (Stuart: "The DX
Log should live next to the gallery"). Drawn only where hwinfo carries `dxLog:true`: the scanner is on (or a log
exists) and, if the owner locked viewing, only for the owner. An older server sends no `dxLog`; an older client never
looks for it.

- **DX only by default**; SHOW LOCALS and SHOW SUSPECTS widen it (a Suspect is greyed with the server's reason);
  ALL / FM / DAB; sort by LAST HEARD / FIRST HEARD / PEAK SIGNAL / TIMES HEARD; **LIVE NOW** on entries heard in the
  current or the last completed sweep of their band; 25 a page.
- A row opens its **openings** (start, end, detections, peak). **TUNE** goes there through the client's ordinary tune
  path — FM: the frequency in WFM; DAB: the block's first service (the server sends the block's centre, so neither
  client keeps a Band III table) — and the server's own shared-dial rules decide, as for any tune.
- **BOOKMARK** (the owner: no password set, the host itself, or the owner's proof — POST /bookmarks's rule) copies it
  into the normal bookmarks: FM as a manual WFM bookmark named by the PS (else "PI xxxx"); DAB as the band scan's bookmark
  rows (every service the multiplex named). The entry is then never pruned.
- **Alerts, per user and opt-in**: "Tell me about new DX here" (this browser / this device). Polled once a minute while
  connected; shown in the VTS like a server notice; the first poll on a server only records where its alerts are, so
  nothing older than the visit is announced. The server alerts only on a **new DX entry** or a **new opening** of a known
  DX entry — never on Local, Suspect or the bootstrap's Provisional entries.
- `src/utils/dxLog.ts` is the one rule for both clients.

## How it works

```
 housekeeping tick (2 s): bgTick → dxTick ── nobody here ≥ wait, nothing else owns the idle radio ──► dxStart
                                                                                                        │
 connect path: resumeCaptureIdle → bgStopForListener, dxStopForListener ◄── (stop, discard, give the dial back)
                                                                                                        ▼
 vibe-dxscan (sequencing only) ── nextJob(cursor) ──► FM hop  |  DAB sweep from the cursor to 13F
   FM hop:  bgPlace(hop centre) → DSP thread copies IQ → FmHopEngine (vibe-dxfm + up to 4 workers, decoder priority)
            → hits → Log::commitFm → advance(cursor)            (cut short or timed out: nothing, the cursor stays)
   DAB:     dabBandScanRun in DX MODE → each COMPLETED block → Log::commitDab → advance(cursor)
```

### The arbitration — background decoding's, unchanged (brief §3)

- Runs only with **nobody here**, after the owner's wait; holds the idle park off meanwhile (`dxWanted_`, as `bgWanted_`).
- A listener arriving stops it **on the connect path**, before the newcomer is told anything: the hop in progress is
  **thrown away** (never logged) and the radio goes back to exactly the dial it was left on (`bgPlace` of the saved
  centre/view/dial; a DAB sweep hands DAB back itself through the band scan's own `dabRestore`).
- **Priority**: a due background-decoding profile comes first (its start stops the scanner); ADS-B kept running owns the
  idle radio; a **locked** radio is never moved; never on an Icom; never while another whole-radio mode is still in its
  leave grace; never **on battery** below the threshold (not charging and under the resume level, or paused). There is
  no satellite/Meteor scheduler in the server yet — when there is, it outranks this the same way a profile does.
- **Resume point**: a cursor per band, persisted in the log — FM as a *frequency* (so a radio whose rate or raster changed
  resumes in place), DAB as a block index. The next idle period carries on from the next unvisited hop/block. With both
  bands on, whole sweeps alternate (FM, DAB, FM …).
- **Shared VFO** (§12 q1, resolved): allowed — it only runs with nobody here; nobody sees the scan, only the DX LOG key.
- **The dedicated role** (§3) does not exist in the server yet (`RADIO-ROLES.md`); when it does, its "nobody is ever here"
  makes this scan full time with no other change.

### The FM hop (brief §4) — `vibe_dxscan_fm.h`

- **Hops as wide as the device allows**: a channel's centre must sit inside ±(0.4·rate − 100 kHz) of the capture centre;
  every raster channel is in exactly one hop, and the capture centre sits **half a raster step off the raster** so no
  channel is on the dongle's DC (`planFm`; `test-dxscan` proves it for 0.768–10 MS/s at both rasters). RTL 2.4 MS/s: 12
  hops; 2.048 MS/s: 15; RSP/Airspy at 10 MS/s: 3; HF+ at 768 kS/s: 52.
- **Settle** 50 ms after the retune, then a **Hann-windowed** spectrum for 0.3 s. Peaks on the raster: a channel's power
  (±80 kHz) at least 8 dB over the floor, and the highest within ±100 kHz. ★ The floor is the quietest tenth of the
  **bins**, not a percentile of channels: on a busy band every channel holds a local's skirt, and the first version read
  −39 dBFS against −63 of noise and lost a weak station between locals.
- **Per peak, one ~300 kHz channel cut from ONE shared forward FFT** (vibedsp `Channelizer`, each worker its own
  `ExtractCtx`) → `FmDemod` (NEON / SSE2) → `StereoPLL` (57 kHz reference + bit clock) → `RdsDemod` — the listener's own
  parts. The peaks are dealt to up to 4 worker threads at decoder priority (thread order Network > Audio > Spectrum >
  Decoders holds); the DSP thread only copies into a bounded queue (≤ 0.5 s and ≤ 512 k samples; the oldest dropped and
  told to RDS as a hole).
- **Dwell** 4 s per round (brief: 3–5 s), ending early (not before 1.5 s) once every peak has a PI in ≥ 3 clean groups and
  a whole PS. **More peaks than the benchmark's budget** are decoded in ROUNDS within the same hop: a weak box sweeps more
  slowly, it never falls behind.
- **RDS hook** (the only change inside vibedsp): `RdsDecoder::Callbacks::group` — every group whose block A passed its
  checkword, before any trust rule, routed for the winning hypothesis only, null everywhere else — and `psComplete()`.
  The DX log counts only **clean** block-A groups.

### The DAB sweep (brief §5) — the owner's band scan in DX mode

Stuart: *"DAB scanning is already built too, we just need to tweak it to be a DX scanner so for DAB it can take a little
longer to adjust the gain etc to try and pull a multiplex in as best as it can."* So there is no second DAB sweep:
`dabBandScanRun` has a DX mode (`dabDx_.on`), and **the owner's own scan runs exactly as before** when it is off.

- Starts at the DX cursor; the same per-block gain entry (a remembered gain, else the scan's noise gain — high, the fast
  side to start from) and the same ordinary DAB pipeline, so the same weak-signal lock. **No DAB internals were touched**:
  EId/ECC/TII come from the receiver's own `json()` report and the FIC rows.
- **No bookmarks**: none added or reconciled, and the learn-on-air path is held off while it runs (`g_dxDabScan`).
- **The dwell** — weak-multiplex pull-in beats sweep speed:

  | | owner's scan | DX mode |
  |---|---|---|
  | no lock, no sign of a signal | 2.0 s | **2.5 s** |
  | no lock but a null symbol out of the noise (≥ 5 dB) or the AGC had to cut | up to 8 s | **up to 15 s** |
  | locked, no FIB yet (a false lock?) | 3 s | **6 s** |
  | longest on one block | 16 s | **30 s** |
  | done when | the list confirmed (bookmark rule) | the label and the whole service list are in, or 4 s quiet after 6 s |

- **Measured** on the weak 12D reference (`~/VibeSDR-test-audio/dab/bench/12D-v4l-2048.vbu8`, 2.048 MS/s, played only on
  12D, noise elsewhere — `scripts/test-dx-scan.mjs` E and `scripts/probe-dx-dab-pullin.mjs`): see *Measurements* below.
  ★ A replay does not follow the gain the server writes, so the AGC's part cannot be measured on it; the gain-following
  model (`fake-rtl-tcp --dab-mux bench-clip/mux.cf32`) needs a clip that is not on this machine.

### Confirmation (brief §6)

- **FM**: the PI in **≥ 3 CRC-valid (clean block A) groups within one dwell, or the PS fully assembled** — a single valid
  group is never enough (`fmConfirmed`; `test-dxscan`: "a single CRC-valid RDS group never creates an entry").
- **DAB**: the first CRC-valid FIB carrying an EId. With TII the key is **EId + block + TII main/sub** (two SFN sites,
  two entries); without, EId + block. TII is the receiver's strongest transmitter when it has one.
- **Images and intermod (TRAP)**: a new FM detection on the **mirror of a known Local about the capture's DC**, or on
  **2·f1 − f2** of two Locals inside the capture, is **Suspect** — kept, greyed, never alerted. ★ Plus one rule the first
  e2e run asked for: the **same PI mirrored about the DC in the same visit, ≥ 10 dB weaker**, is Suspect at once — even in
  the bootstrap (the fake dongle's images of its own locals decoded cleanly, and a real RTL's do too).

### Classification (brief §7)

- Keys: FM **PI + frequency** (the same PI elsewhere is another entry), DAB **EId + block [+ TII]**.
- **Presence** is counted per **visit** of the hop/block that covers an entry — not per whole sweep — in 4-hour buckets
  over 7 days, so a scanner interrupted all day still measures every station fairly.
- **Local** = present in ≥ 90 % of its visits across a **7-day** window and on every day of it that had visits (a duct
  that came and went is not a local), first heard ≥ 7 days ago. ★ Judged on the **PI, never the PS** — a scrolling PS is
  kept as variants for display (`test-dxscan`: a local with six PS frames is still Local).
- **Bootstrap**: the log's first 24 h — everything **Provisional**, no alerts. At its end the baseline is judged on what
  the bootstrap saw (≥ 90 % of ≥ 3 visits → Local, the rest DX — and a baseline detection on a new Local's image →
  Suspect), with no alert for having been found then; their next opening alerts. So strong locals land **once** and never
  alert.
- A Local unheard for over 7 days drops back (unclassified → DX).
- **Openings**: a contiguous run of detections; over 30 min unheard (`gapMin`) starts the next.

### Data model, persistence and bounds (brief §8)

- `vibe_dxscan.h`: `Entry` (id, band, key, class, suspectWhy, frequency/block, PI, PS + variants, EId, ECC, ensemble,
  services, TII, first/last heard, times heard, peak level (dBFS, uncalibrated — not dBm) and SNR (dB over the hop's floor;
  MER for DAB), presence, promoted, openings, buckets) and `Opening` (start, end, detections, peaks, the DAB services that
  decoded — §12 q3 — and a proof ref, unused).
- **In memory, written in batches**: every 5 minutes when it changed, and at shutdown — never per detection (SD-card
  wear). One file per radio beside the gallery: `<data dir>/dxlog[-<serial>].json` (Android: `files/dxlog.json`), written
  to a temp file and renamed.
- **Bounded** (the 256 MB floor): 1,000 entries; 32 openings each (newest); 8 PS variants; 32 services per entry, 16 per
  opening; 100 alerts. At the cap the oldest **Suspect** go first, then Provisional, then DX, then Local — a **Promoted**
  entry is never pruned. Every API page is ≤ 100 entries. CLEAR keeps promoted entries.

### API (brief §9) — additive only

| | |
|---|---|
| `GET /vibeserver/dxlog.json?band=&local=1&suspect=1&sort=last\|first\|peak\|times&offset=&limit=` | the page: `total`, `counts`, `bootstrap`, `sweeps`, `alertSeq`, `entries`, `scanner` (its settings), `canPromote` |
| `GET /vibeserver/dxlog/entry?id=` | one entry with its openings, newest first |
| `GET /vibeserver/dxlog/alerts?since=` | alerts newer than a sequence number |
| `POST /vibeserver/dxlog/promote?id=` | owner: into the bookmarks |
| `POST /vibeserver/dxlog/clear` | owner: empty the log (promoted kept) |
| hwinfo `dxLog` | true = draw DX LOG |

Reads are PIN-gated like the gallery and honour the owner's view lock; writes are POST /bookmarks's owner rule. **V11
core lock**: no WebSocket message was added or changed; an old client never sees any of it
(`test-dx-scan` C: tune, spectrum, nothing `dx` pushed down the socket).

## Decisions for the brief's open questions (easy to change)

- **§12 q2 — proof clips: opt-in, default OFF.** The model and the file carry `proof` and an opening's `proof` ref, but
  nothing records a clip yet and **no control is drawn** (no dead controls): the scan does no audio decoding (brief §2),
  so a clip needs its own short listener-style capture after a confirmation — left for a later step.
- **§12 q3 — DAB openings record the services that decoded** (≤ 16 an opening).

## Measurements

(M5 Pro, `build-dx`, Release.)

- **FM hop engine** (`test-dxscan-fm`, five stations at 2.4 MS/s, 4 workers): ~0.18 s of CPU per hop of ~4.3 s of capture
  — **~4 % of one core for the hop, under 1 % per station**. In the server, the whole process (spectrum, the idle
  pipeline and the hop) spent 0.05–0.28 s CPU per 2.9 s hop with 1–3 stations (`test-dx-scan` A, 2.5 s test dwell).
- **Pi 2 (estimate, not measured)**: `PI2-REAL-CPU.md` puts RDS over WFM at +23 points of a Pi 2 core per station at
  2.4 MS/s and the wide FFT at ~15–20 %; a hop's station is the discriminator + pilot loop + RDS front end (no audio), so
  ~25–35 % a station plus ~20 % for the hop's FFT — about 2–3 cores for eight stations at once. The `dec_dxscan` row
  measures exactly that and turns it into the round budget (8 × 150 / %): a Pi 2 at ~250 % would decode ~4 stations a
  round, two rounds on a busy hop, ~8 s a hop, **~2 minutes a sweep** at 2.048 MS/s. It runs only with nobody listening;
  a listener's arrival stops it before their audio starts.
- **Sweep time**: FM at 2.4 MS/s, 12 hops × ~4.5 s ≈ **55 s** a sweep (test: 12 × 2.9 s with the 2.5 s test dwell). DAB,
  41 blocks: **116 s** with one multiplex (12D in 6.1 s; the 40 empty blocks 2.5–2.7 s each, mean 2.6).
- **RAM bound**: the log at its caps — FM entries ~1.5 KB each, DAB entries up to ~4 KB — under ~3 MB, typically tens of
  KB; the FM hop: its queue ≤ 4 MB, a batch of 8 bins blocks (512 KB at 2.4 MS/s), ~60 KB a station; the file is the log
  as JSON (a few KB per entry at the worst, rewritten at most every 5 minutes).
- **DAB dwell vs pull-in on 12D**: see the table below (`scripts/probe-dx-dab-pullin.mjs`).

DABPULLIN

## Tests

- `vibeserver/test-dxscan.cpp` (66): the config, the hop plan at five rates and both rasters, confirmation (incl. "a single
  CRC-valid RDS group never creates an entry"), the scheduler and cursor (interrupted half the time, every hop and block
  still covered; resume from the next hop; FM/DAB only; a changed rate resumes in place), bootstrap/locals/alerts
  (locals appear once, silent; a scrolling-PS local is Local; a Local gone 7 days drops back), openings (Es for 20 min →
  one entry, one opening; back 2 h later → a second opening, not a second entry), image/intermod Suspect (incl. the
  same-visit mirror in the bootstrap), DAB keys with and without TII, the caps and prune order (promoted never pruned),
  persistence (byte-identical round trip, the cursor survives), the page.
- `vibeserver/test-dxscan-fm.cpp` (11): five RDS stations from one 2.4 MS/s capture (one 20 dB under its neighbours, one
  with no RDS) — every PI and PS, no stray PI, abort returns nothing, rounds, noise only, the 50 kHz raster. Clean under
  ASan and TSan.
- `scripts/test-dx-scan.mjs` (32, ~3 min, in `run-tests.sh`): the server end to end — §13's server-side acceptance tests
  (scan starts after the wait; a listener mid-hop gets the dial back and the hop is not logged; the cursor resumes; an old
  client works normally; promote/clear; the 12D multiplex found in DX mode with no bookmarks).
- `scripts/shot-web-dxlog.mjs`: pictures of the web DX LOG at 390 and 900 px on the default, silver-solid and black-glass
  looks, with an overflow check.
- `scripts/check-android-settings-parity.mjs`, `scripts/check-setup-page.mjs`, `scripts/test_nav_coverage.ts` pass; `npx
  tsc --noEmit` has only the three known errors.

## Not done / next

- **Proof clips** (above). **Distance/country from PI/ECC** and the **shared propagation map** — later phases (brief §11).
- **TII on weak DX**: attached when the receiver has it at the end of the block's dwell; the 12D replay had none in its
  6 s. A DX-mode wait for TII on a strong-enough block is a possible refinement.
- **The dedicated role** and a **satellite scheduler** do not exist in the server yet; both slot into `dxWhyNot` / the
  start path the way background decoding's profiles do.
- **A real Pi 2 run** of `dec_dxscan` and of a sweep; field tuning of the FM dwell (4 s) and the 8 dB peak threshold.
- **Not built for this branch**: an Android gradle build (the shim and JNI were syntax-checked for arm64-v8a and
  armeabi-v7a with NDK 27, and the FM engine's NEON path compiled for both).
- `RdsDecoder::errorTable()` builds a static table lazily; with several decoders on several threads that is a benign race
  only if the burst setting never changes while they run (it is the Advanced-RDS lever). Pre-existing; noted, not changed.
