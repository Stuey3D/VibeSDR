# VOR bearing and ILS deviation (V12)

Branch `v12-vorsar` (off `v12-experimental` 3de409c9a), 2026-11-19. A listener tunes a VOR in AM and presses **VOR / ILS**:
VibeServer reads the radial they are on FROM the station, the station's Morse ident, and names it from the published list; on
an ILS localiser or glideslope it reads how far off the course / glide path they are. Two VORs' radials cross where the
receiver is.

**Nothing has been decoded off air by us.** Every figure below is from our own transmitter (`decoders/nav_tx.h`), unit tests
and a fake radio end to end. ★ The bearing CONVENTION (which way round the two 30 Hz phases are) is the published one, but
only a real VOR can prove we read it the right way round — see "Live captures wanted".

## RESUME HERE

**★ ON AIR 2026-11-20 ~02:30 (Pi 500 :48004, the VHF V4L on its ADS-B antenna; tools/vsrec + tools/vor-replay):**
- **DTY Daventry 116.40, 60 s** (`~/VibeSDR-test-audio/vor/dty-60s-wide.cu8`, 48 kHz cu8, ±12.5 kHz passband): the ident read
  **"DTY"** (9.4 wpm); var30 27 %, subcarrier 28 % (both as specified); only 2 seconds passed the VOR gate (refCoh 0.42–0.53
  against ≥ 0.5 — a weak signal on a 1090 MHz aerial), their bearings **54–57°**. Expected: 47.8° true from DTY to the IO92nh
  square's centre (21 km; the square is good to a few km, i.e. several degrees). **Same side, not BEHIND — the phase sense
  looks right.** No 3-second believed bearing yet: a better aerial (or a longer run) is wanted before calling the radial exact.
- **The AM passband (±6 kHz) cuts the 9960 Hz subcarrier** — the first captures had sub 0.8 %. The live decoder widens the
  channel to 25 kHz itself (navWideSync); a capture must ask (BW=12500).
- **CFD 116.50, 120 s**: a carrier and a 9960 Hz tone but NO 30 Hz on either (var30 0.6 %, dev 24 Hz) and no ident — not a
  working VOR as we expected; check the station / frequency before using it.


1. **Live capture (the coordinator — agents may not tune Stuart's radios):** 60 s of IQ on two or three UK VORs the Pi 500's
   VHF RTL (`:48004`, serial 00000001) hears, AM, ≥ 2.4 MS/s or a 48 kHz+ channel. Candidates within range of a Humber-area
   server: **OTR Ottringham 113.90**, **GAM Gamston 112.80**, **POL Pole Hill 112.10**, **TNT Trent 115.70**, **DTY Daventry
   116.40** (further). Feed each through `test-vor` (add a `wav`/`cu8` replay mode — not built) or attach the decoder live.
   The decoder's OWN check is in the owner's line: **"the radial passes N km from this server"** — on a real VOR that should
   be a few km at most (station distance × sin(error)); if it says BEHIND the station, the phase sense is inverted
   (`nav_rx.h second_`: swap the subtraction) — one capture settles it.
2. An ILS: a localiser is weak on the ground away from the approach; Humberside (EGNJ) ILS is not in the open data. Record one
   if heard; check the side words against where the server is relative to the extended centreline.
3. Not built (below): the radial line on a MAP, the app's compass card, the R36S view, an ILS runway table for the UK.

## Where it is offered

- **A listener's decoder** (`audio_extension_attach`, `extension_name: "vor"`) — no parameters (AUTO: the 50 kHz channel says
  VOR / localiser / glideslope, the signal confirms; the receiver runs both detectors).
- **Reach** (`vibe_decoder_reach.h`): 108–117.975 MHz and 328.6–335.4 MHz, any part. Greyed with the reason elsewhere; on an
  **Icom greyed** ("VOR and ILS need the radio's IQ … an Icom gives demodulated audio" — `vibe_icom_decode.h`). Not on VFO B
  (refused in words).
- **Owner switch:** "VOR bearing and ILS deviation" (setup page and the app's ServerModeScreen). **Benchmark row `dec_vor`**
  (a VOR with its ident, 20 s at 37.5 kHz): 0.04 % of an M5 Pro core, decodes; sets the switch's default, never hides it.
- **Clients:** key **VOR / ILS** under **Aviation on the VHF tab** (VOR, localiser) **and the UHF tab** (glideslope); the hint
  under it. Web: a compass card (true north up, the radial drawn from the station) or the ILS needle on five dots, the
  readout, the station, and a **FIX** line when two different VORs were heard in the last 30 min (`radialFix`). App: the
  status line and the log (no card yet).
- Copy: AboutOverlay V12 note; web tutorial's DECODERS card. `sdrTour`'s mode card names no decoder — re-read: still true.

## How it is fed (★ the important plumbing)

The demodulator's channel IQ is **cut to the listener's width** (AM 10 kHz) — the VOR's 9960 Hz subcarrier would be gone (the
first end-to-end run read `sub 0`). So:

- **Per-listener radios** (`perClientDsp`): `feedOneClient` hands the decoder the listener's **slice** — the channeliser's
  extract, ≥ 24 kHz, flat to ±0.375 of its rate, the dial within half a bin of DC — **before** their pipeline. Their audio and
  width are untouched. (`DecoderManager::feedNavIqSession`.)
- **The one pipeline** (a single-listener radio / shared dial): its channel is held **≥ 25 kHz wide while the shared host runs
  VOR** (`navWideSync` at every attach, detach and decoder-socket close; the listener's own width put back after), as M17 holds
  its 12.5 kHz. ★ While it is held the AM audio is 25 kHz wide: **the 9960 Hz subcarrier is audible as a high whine** — a
  decision for Stuart (below).
- Both through paging's channel-IQ queue on the decoder thread (`qc_`, bounded at 2 s, oldest dropped and counted);
  `DecoderHost::navIqRunning()` is the DSP thread's one relaxed load when nothing runs.

## The receiver (`decoders/nav_rx.h`)

From ICAO Annex 10 Vol. I §3.1 (ILS) and §3.3 (VOR) as described in the public literature (the Annex itself was not opened).

- At the channel rate: `e = |z|` (the TRUE envelope — `|z|²` would cross speech into the subcarrier band); five products of it
  — `e`, `(e − dc)·LO(−9960)`, `(e − dc)·LO(−1020)` (I and Q) — through **one** Hamming low-pass (flat to ±650 Hz) that
  decimates to ≈ 3.1 kHz. The 1 Hz `dc` high-pass is only on the subcarrier / ident products (it moves 9960 Hz by 0.006°; the
  30 Hz variable never passes through it).
- At ≈ 3.1 kHz: the subcarrier's FM discriminator (its half-sample delay put back analytically), the ident's 17 ms moving
  average and its power every 10 ms; a second low-pass, **identical for the discriminator and the envelope**, to ≈ 780 Hz.
- Every second, Hann-windowed correlations at 30 / 90 / 150 Hz against ONE time base: variable depth, reference deviation,
  subcarrier depth, the coherences, the ILS depths. **Bearing = phase(reference) − phase(variable)** (the variable lags by the
  azimuth). A Doppler VOR reaches the receiver as the same signal.
- **A VOR second:** the discriminator's 30 Hz ≥ 50 % of its variance, deviation 300–650 Hz (480 ± 16 nominal), subcarrier
  10–50 %, variable 10–50 %. **A bearing** after 3 such seconds running that agree (circular σ ≤ 4°), their vector mean (5 s).
- **An ILS second:** 90 + 150 Hz ≥ 60 % of the 20–390 Hz envelope variance, each ≥ 2 %, no VOR subcarrier; held after 3
  seconds whose DDM agrees within 0.02. DDM = m90 − m150. Localiser: 90 Hz dominant = left of the course seen from an
  approaching aircraft = **fly right**; glideslope: 90 Hz dominant = above = **fly down**. Full scale 0.155 / 0.175 DDM.
- **The ident** (`IdentKeyer`): keyed when marks stand ≥ 8 dB (power) over the spaces (20th / 95th percentiles of 30 s),
  threshold half-way in dB, 30 ms debounce, dots by 2-means over each word's marks (ratio 2–4.5), else the last dot (7 WPM to
  start); a word ends at ≥ max(7 dots, 1.2 s). Letters from the ITU table. **Confirmed** by the NDB skimmer's consensus
  (`ndb::IdentVote`: three clean identical readings …) **or** a reading that IS a station published on this channel, read
  twice (the list is the second witness).
- **SIMD:** the envelope (four `|z|` a lane — SSE2 / AArch64 `sqrt`, ARMv7 lane-wise `sqrtf`: every path the correctly
  rounded IEEE root) and the decimating dot products (`q65_simd.h`) have scalar twins; `test-vor` checks the decimated
  streams' checksum and every report bit for bit; `VIBE_FORCE_SCALAR` builds pass too.
- **Memory:** fixed — FIR histories, one second at 780 Hz, 30 s of 100 Hz keying, a ≤ 200-element run. A few tens of KB.

## The station list (`vibe_vor_navaids.h`, `scripts/gen-vor-navaids.mjs`)

**OurAirports, public domain** (navaids.csv, airport-frequencies.csv, airports.csv — retrieved 2026-11-19 into
`.mapdata-cache/`): **3651 VOR / VOR-DME / VORTAC** worldwide (48 in the UK) with ident, frequency, position, DME channel, and
the station's **SLAVED variation** (the declination its radials are aligned to; 2918 have it, else the site's magnetic
variation) — a radial's TRUE bearing = magnetic radial + variation. **21 ILS localisers** only (those OurAirports'
frequencies list as ILS/LOC on the localiser raster, positioned at their airport). ★ National AIPs (the UK's is Crown
copyright) are NOT bundled, so most localisers show "runway not in the open data". ≈ 270 KB of `.rodata`, paged in only
when looked up. `--check` fails when the table drifts from the cache.

⚖ **What is sent to whom** (`vibe_nav_run.h`): a navaid is a broadcast for anyone — radial, deviation, ident, station go to
every listener on the decoder. The distance from the station to 1 km and how far the radial passes from the server go to the
**owner's signed-in sockets only** (`textFramePriv`); everyone else gets the distance to 10 km (the NDB list's precision).

## Tests

| Suite | What | Result |
|---|---|---|
| `test-vor` (vibeserver/, `-O2`, header-only) | the band plan + 40 ILS pairs; 40 radials at 25 / 31.25 / 37.5 / 48 kHz within 0.5° (worst 0.02°); 24 radials at 20 dB (worst 0.11°); ±2 kHz off the dial; the 30 Hz 0.1 % fast; 30 % voice on the VOR; 5 dB only-if-right; localiser DDM ±0.155 in 17 steps (worst 0.0008); side; glideslope 35/45 %; 15 dB; idents LON BIG OTR MAY TNT MOT at 6–10 WPM (≥ 3 of 4 repeats), a localiser's ILL; **5 min of noise and 5 min of AM speech: nothing**; a plain carrier, 9960 Hz without FM, no variable: nothing; a VOR never an ILS; SIMD = scalar; cost (0.04 % of an M5 core) | **35 / 0** |
| `test-server-vorsar.mjs` | real server, fake rtl_tcp, `test-vor clip` (LON on 247.5° at +25 kHz): one pipeline — radial 247.5 (σ 0.01°), ident LON, LON London GB, true 244.5 (var −3), the line, state 1, the owner's "radial passes" only signed in; per-listener — 247.5 at 36 kHz; switches off → refused (shared with 406) | **25 / 0** |
| `test_navaids_app.ts` | the shared client half: sanitise, words, readout, the line, the FIX (exact on a constructed case; behind a station refused), the web card, placement, glyphs, both clients' wiring, both setup screens, the copy | **42 / 0** |
| `test-bench-decoders` | `dec_vor` 0.04 %, decoded 49, has a rule | pass |

Also re-run and passing: test_decoder_bands, test_decoder_reach_clients (82), test-radio-caps (248), test-decoder-reach (76),
test-setup-mode-tree (64), check-setup-page, test_tones_app, test_balloon_app, test_ncdxf_app. Web: esbuild bundles clean;
the full web `tsc` cannot run inside the agents' 1 GB cap (OOM at 960 MB) — the coordinator's run checks it.

## Not built

- **The radial on a MAP** — ★ beta93: the VOR / ILS MAP mode draws every station's radial and the FIX (docs/v12/VOR-MAP.md). For this decoder (the brief's "line on the map"): `radialLine()` is ready (great circle from the station along the
  true bearing) and the web shows the FIX in words; drawing it on the web's TrackView / the app's MapOverlay is not done.
- **The app's card** (compass / needle) — the app shows the status line and the log. **R36S** view — not built.
- A `wav`/`cu8` replay mode in `test-vor` for real recordings.
- ILS runways for the UK (no open source); DME distance (we name the channel only — DME is a separate 1 GHz system).
- Two-frequency (capture-effect) localisers: both carriers sum in our envelope; untested.

## Decisions for Stuart

1. **One-pipeline radio: the AM channel is held at 25 kHz while VOR runs** — the subcarrier whine is audible. Alternative: an
   audio low-pass in the pipeline for that case (a vibedsp change) — worth it?
2. **Station distance to 10 km for listeners, exact for the owner** — a radial plus a distance is a position fix of the
   server. Or the distance for the owner only?
3. **The FIX** in the web box (two VORs heard this session) shows a latitude/longitude — of the receiver, by construction.
   Keep, or owner only?
4. **Placement:** VHF Aviation + UHF Aviation (glideslope). OK?

## Sources (the house rule)

ICAO Annex 10 Vol. I signal definitions as publicly described (VOR 30 Hz / 9960 Hz ± 480 Hz / 1020 Hz ident; ILS 90/150 Hz,
DDM sides, 0.155 / 0.175 full scale); the LOC↔GS pairing table checked against the FAA AIM table as reproduced on the
RadioReference wiki (read 2026-11-19, all 40 pairs). No other receiver's source was read. Station data: OurAirports (public
domain).
