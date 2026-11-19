# The VARA HF identification tag (V12, 2026-11-19)

Branch `v12-varatag` (worktree `.claude/worktrees/v12-varatag`), off `v12-experimental`. A signal label,
**"VARA HF (proprietary — not decoded)"**, over an HF signal whose OUTWARD shape is VARA HF's. **Identify only.**

## ★★★ The legal note (Stuart, 2026-11-19) — read before touching anything here

VARA must **never** be decoded, in any form. Its author sells the modem and enforces that with cease-and-desist letters;
an open-source VARA receiver had to shut down. So:

- The tag uses **only outward, publicly documented measurements**: occupied bandwidth (VARA HF's published 2750 / 2300 /
  500 Hz modes), the regular comb of carriers in the time-averaged **power spectrum** and its spacing (measured, not
  assumed), and how the block comes and goes (ARQ bursts). Facts from sigidwiki's "VARA HF" page only (widths, carrier
  counts 59 / 49 / 11, symbol rates 23 / 42 / 47 / 94 Bd).
- Nothing in the tree demodulates a VARA carrier, reads a phase or a symbol, or knows anything of VARA's framing,
  scrambling, coding or protocol. No XARA code, documents or descriptions were opened or used, and none may be.
- The words are fixed in ONE place per reader: `src/utils/knownSignals.ts` `VARA_TAG_LABEL` / `VARA_TAG_HINT` (web and
  app). The server only sends the kind `"vara"` and where. Never say more than "proprietary — not decoded".
- The synthetic "VARA-shaped" signals in `test-varatag` are **generic CP-OFDM** (random QPSK per carrier) built from the
  published width / carrier count / baud — no framing, no coding, nothing of VARA's own.
- If a future change needs anything beyond the power spectrum's shape, the answer is no.

## RESUME HERE

Built and committed on this branch; **not merged**. What is done, and what is owed:

1. **Server** (`android/app/src/main/cpp/vibe_varatag.h`, wired in `local_sdr_shim.cpp`): stage 1 on the shared wide row,
   stage 2 on a slice of the same forward FFT, `sigtags` on the spectrum socket (`Out::Tags`), the owner switch
   `varatag`. Compiled on this Mac only through `test-varatag` (header + channeliser); **the whole vibeserver build had
   not run when this was written** — the build lock was held by another agent's build for the whole session. See
   "Not done" below.
2. **Web + app**: one more label kind (`src: 'tag'`), parsed by `parseSigTags`, drawn by the existing label layer
   (`labelLayout`, the bar for the occupied span), tap = tune in USB with no decoder. TS tests pass
   (`scripts/test_known_signals.ts`: 132/0, 9 new).
3. **Synthetic**: `vibeserver/test-varatag.cpp` — every positive tagged, every look-alike 0 labels (numbers below).
4. **Real signals**: ~40 minutes of Stuart's UberSDR (80 / 40 / 30 / 20 m Winlink segments, evening 2026-11-19):
   **0 false labels — and no real VARA was found on air to confirm the positive.** The spacing window
   (`kSpacingLo/Hi` 45.3–48.5 Hz) is therefore still the published-figure value (46.9 Hz), not a measured one. **The
   coordinator should capture a real VARA ARQ session** (20 m 14.105–14.112 in daytime is the best bet from the UK;
   `tools/ubersdr-rec/ubersdr-rec.mjs 192.168.86.11:8080 14108000 iq48 600 ~/VibeSDR-test-audio/vara/…`) and run
   `VIBE_VARA_REAL=1 test-varatag`; the harness prints each tag, its centre and the measured spacing.
5. **R36S**: not wired. The handheld's engine is a one-pipeline radio (no shared channeliser), where the tag does not
   run (see "Where it runs"), so it would never receive a tag. Its label layer (`handheld-ui/src/known_labels.h`) could
   take one in a few lines if that changes.

### Not done
- The vibeserver/web build (`cmake --build … --target vibeserver`, `node scripts/build-web.mjs` → `vibe_web_page.h`) —
  the lock was busy. The shim edits are small and mirror the lightning badge's; build them first.
- No live-server test (rules: none of Stuart's servers).
- `scripts/run-tests.sh` not run (rules). `test-varatag` is registered there (`deps_for`, `cobjs_for` cobj-ndb, `-O2`).

## How it works

### Stage 1 — the shape, from the row the waterfall already has
`LocalSdrShim::onSpectrum` hands the averaged wide row (FFT_AVG engine frames, `fftSizeForRate` bins ≈ 47–75 Hz, the
channeliser's **unwindowed** transform) to `Tagger::row` at most 4 times a second. No new transform.
- A local noise floor: medians of ~700 Hz blocks, then the 30th percentile of ±12 blocks.
- Runs of bins ≥ floor + 7 dB, dips of up to 4 bins bridged (a fade's notch must not cut a block in two).
- Each run's width at 10 dB under its median top (or the floor threshold on a weak block), bridged the same way, must
  fall in a VARA class: 380–660 Hz (500), 2050–2520 (2300), 2520–3050 (2750).
- The top flat (std dev ≤ 4.5 dB over the inner 80 %, ≥ 70 % of it within 6 dB of the top) and the sides steep (3 and 4
  bins out back at the floor + 4.5 dB, or 12 dB under the top — the unwindowed transform leaks ~−12 dB three bins out).
- A passing run joins a **track** (same class, centre within ±12 % of the width). A track's on/off history over 90 s is
  the burst pattern.

### Stage 2 — the comb, from one narrow slice (K = 1)
The wide bins are too coarse to see carriers ~47 Hz apart, so one candidate at a time (on for ≥ 3 s in the last 90 s,
no comb result in the last 4 minutes) gets a slice cut by `Channelizer::extract` from **the same forward FFT** every
listener's channel comes from (`sliceBins`: twice the class width plus a guard; ~9–12 kHz). That is one small inverse
transform per block, and only while an evaluation is open.
- Hann-windowed segments at ~1.5–2.9 Hz resolution, ½ overlap; a segment counts only while the block is **up** (its band
  8 dB over the slice's guard bins), so ARQ gaps and the other station's silence never dilute it.
- After 40 such segments (or 80 s; fewer than 18 = no verdict) the averaged power spectrum's flat top has its slow shape
  removed (a 150 Hz moving average) and is clipped to ±6 dB (a narrow carrier or a notch must not be the ripple).
- **The periodogram of that ripple** over periods 20–200 Hz (`measureComb`) names the carrier spacing. It passes when
  the strongest period of all lies in 45.3–48.5 Hz and its value is ≥ 0.10 (0.5 = a pure ripple; noise ≈ 0.005).

### Tag and hysteresis
`conf = 0.15 × shape + 0.85 × comb score` (comb score 1 at a periodogram value ≥ 0.15). Tagged at conf ≥ 0.85 **and** a
pattern (≥ 2 bursts in 90 s, or 10 s on); held through pauses for 60 s; dropped below 0.60 or 60 s unseen; a track is
forgotten after 120 s. At most 24 tracks; the comb re-checks a tagged track every 4 minutes.

### Where it runs (the reach gate)
- HF only: the whole capture below 30.5 MHz.
- A shared / locked radio only (`perClientDsp()`): the slice needs the channeliser. A one-pipeline radio (a personal
  receiver, the R36S) has no shared forward FFT; giving it one is the WideTap's cost (10–20 % of a Pi 2 core), far too
  much for a label. **Open decision for Stuart** below.
- Someone connected (`s_listenersCached > 0`), and the owner's switch on (`varatag` in Setup → Modes and decoders, on by
  default; both BLOCKABLE lists: `vibe_setup_page.h` and `ServerModeScreen.tsx`).

### The wire
`{"type":"sigtags","v":1,"tags":[{"id":3,"hz":7049400,"bw":2300,"k":"vara","c":0.93}]}` on the spectrum socket, its own
newest-wins class `Out::Tags`, per listener on change and every 10 s while any are held (`teleDue` slot 2); an empty set
once on the change. Clients drop tags 30 s after the last message (`SIGTAG_STALE_MS`) and on a new server. An older
client ignores the message (its own `case`).

### The clients
`parseSigTags` (kinds it knows only, HF, 300–3200 Hz, ≤ 16) → `sigTagSignals` (a curated label, an EiBi row or a bookmark
within half the width wins) → `withSigTags` into the same list the labels already use. Drawn in the labels' own look
with the occupied span as the digital modes' bar; the screen-reader text is `VARA_TAG_HINT`. A tap tunes USB 1500 Hz below
the centre and opens nothing. Web: `spectrum.ts` `case 'sigtags'` → `main.ts` `sigTagsReceived`. App:
`VibeServerWsClient` → `SDRScreen` `onSigTags` → `knownSignals` memo.

## Tests and what they showed

### Synthetic — `vibeserver/test-varatag.cpp` (the server's path: Channelizer → unwindowed rows → Tagger; slice by `extract`)
47 Hz bins (192 kS/s, 4096 points) and 75 Hz bins (307.2 kS/s): see "Results" below for the run's figures.

| Case | Result |
|---|---|
| VARA-shaped 2300 / 2750 / 500, ARQ, 47 Hz bins | tagged, centre within 10 Hz, spacing read 46.7–46.9 Hz, after 20–28 s |
| 2300 continuous, 75 Hz bins | tagged (spacing 46.2–46.3) |
| 2300 ARQ + two-path fade, 75 Hz bins; 2750 ARQ + fade, 47 Hz bins | tagged (comb P 0.17–0.19) |
| two sessions on one capture | 2 tags |
| noise (47 and 75 Hz bins) | 0 |
| PACTOR III stand-in (18 tones, 120 Hz, 100 Bd, ARQ / continuous) | 0 — comb names 118–119 Hz |
| PACTOR IV stand-in (1800 Bd one carrier, ARQ), STANAG 4285 / MIL-110 stand-in (2400 Bd) | 0 — no comb (P ≤ 0.02) |
| FreeDV 700D / 700E stand-ins, RADE V1 stand-in (30 × 50 Hz) | 0 — no width class |
| 2.3 kHz-wide 50 Hz comb, 2.3 kHz-wide 31.25 Hz comb (the spacing guard alone) | 0 — comb names 50.0 / 31.2 Hz, P in the VARA window ≤ 0.01 |
| DRM mode B 10 kHz (our transmitter, `drm_tx.h`; its carriers are ALSO 46.875 Hz apart) | 0 — the width |
| Olivia 8/500, 16/500, 32/1000, 64/2000, MFSK16 (our transmitter, `textmodes.h`) | 0 |
| MT63-500 / 1000 / 2000 (our transmitter, `mt63.h`) | 0 |
| REAL RADE V1 + SSB speech (Stuart's UberSDR, 40 m LSB, 2026-11-16, CT1DHR's over) | 0 — comb names 50.1 Hz |

The margin: every look-alike that reached stage 2 scored ≤ 0.01 inside the VARA window; every positive ≥ 0.14.

### Real recordings — `VIBE_VARA_REAL=1 test-varatag` (all of `~/VibeSDR-test-audio/vara/*.wav`, kept out of git)
Recorded from Stuart's UberSDR (RX888) with `tools/ubersdr-rec`, 2026-11-19 21:52–22:27 UTC, 1 other client on it:

| File | What is in it | Tags |
|---|---|---|
| `uber_80m_3590000_iq192_…2152Z` (120 s), `uber_80m_3595000_iq48_…2201Z` (600 s) | 80 m 3.494–3.686: LSB QSOs, FT8/FT4 at 3.573/3.575, CW, a continuous ~2.7 kHz block at 3.5805–3.583 MHz (comb ~150 Hz, not VARA), narrow data | 0 |
| `uber_40m_7060000_iq192_…2152Z` (120 s), `uber_40m_7050000_iq48_…2201Z` (600 s), `uber_40m_7100000_iq192_…2156Z` (240 s) | 40 m 6.96–7.20: a ~2.2 kHz burst every 15 s at 7.0483–7.0505 MHz (discrete wavering tones, a carrier through it — one station, no reply; not identified), SSB, CW, FT8 | 0 |
| `uber_30m_10140000_iq192_…2156Z` (240 s) | 30 m: FT8 10.136, narrow data | 0 |
| 20 m / 30 m iq48 (420 s, 22:20) | see "Results" | |

**Hits on real VARA: none observed** (no VARA ARQ session was on air in these windows). **False labels: 0.**

### CPU (this M5 Pro, `-O2`, the test's own timers)
Stage 1: ~38 µs a row at 4096 bins, ~153 µs at 16384 bins; at ≤ 4 rows a second that is **0.06 % of one core** at the
server's big sizes. Stage 2: ~154 µs per second of slice while an evaluation is open (one at a time, ≤ 80 s, then
nothing for 4 minutes per track) — ~0.015 %. A Pi 2 is roughly 10–15× slower per core: **≈ 1 % at most while a candidate
is being checked, ≈ 0.6–0.9 % for stage 1** (an estimate from the ratio, not measured on a Pi 2). Below the benchmark-row
threshold, so there is no benchmark row (the owner switch is the control). The extract on the DSP thread is one
`sliceBins`-point inverse FFT per block during an evaluation (the same cost as one CW-skimmer slice).
★ SIMD: the hot loops are a 4096–8192-point `vibedsp::ComplexFFT` (pffft, NEON/SSE) a third of a second, and stage 1's
small `nth_element`s over ≤ 32768 floats four times a second; nothing here is a per-sample loop at the capture's rate.

## Open decisions for Stuart
- One-pipeline radios (personal receivers, the R36S) get no tag: the slice would need a wide tap (10–20 % of a Pi 2 core)
  — leave them out, or cut the comb's slice from the listener's own channel when they are tuned on the signal?
- The tag is on by default for every shared HF VibeServer — or off by default?
- The look: the labels' own lilac (as now), or a colour of its own like the DX catches?
- Only VARA: PACTOR III's 120 Hz comb and the single-carrier ARQ shapes are measured already — tag them too
  ("PACTOR (proprietary — not decoded)")? Not built: the brief was VARA only.

## Files
- `android/app/src/main/cpp/vibe_varatag.h` — the identifier (header only).
- `android/app/src/main/cpp/local_sdr_shim.cpp` — `g_varaTag`, stage 1 in `onSpectrum`, `varaTagSlice` in the channeliser
  callback, `sigtags` beside `lx`, `Out::Tags`, `teleDue` slot 2.
- `android/app/src/main/cpp/vibe_setup_page.h`, `src/screens/ServerModeScreen.tsx` — the `varatag` switch.
- `src/utils/knownSignals.ts` — `VARA_TAG_LABEL`, `parseSigTags`, `sigTagSignals`, `withSigTags`, `isTagLabel`.
- `web/client/src/spectrum.ts`, `web/client/src/main.ts`, `web/client/index.html` (hint) — web.
- `src/services/VibeServerWsClient.ts`, `src/services/sdrProtocol.ts`, `src/screens/SDRScreen.tsx`,
  `src/components/WaterfallView.tsx`, `src/components/MenuSheet.tsx` (hint) — app.
- `src/components/AboutOverlay.tsx` — the release note.
- `vibeserver/test-varatag.cpp`, `scripts/test_known_signals.ts`, `scripts/run-tests.sh` — tests.
