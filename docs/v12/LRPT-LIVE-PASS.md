# METEOR LRPT on a real pass (beta91, 2026-11-22): why it lost the satellite, and what changed

Branch `b91-lrptfix` (off `v12-experimental` 6991154d2). The first real METEOR-M2-4 pass captured through the server's
own LRPT run decoded 10 frames in 5 seconds, stopped, never relocked, ended "the pass" with eight minutes of it left and
then showed "picture complete" in SATELLITE while the receiver hunted again. Replayed, measured, fixed: the same
recording now gives **60 picture frames and one picture** across the pass.

## The recording (local only — NOT in git)

`~/VibeSDR-test-audio/lrpt-captures/lrpt-1795357190-300000.cf32` (1.57 GB, complex float32, 300 000 S/s, 655.4 s from
14:19:50 UTC, the 1536 MB capture cap) and its per-second log `…-300000.txt` (`t_s doppler_Hz rate lock carrier sync
frames bad`; the Doppler is the satellite engine's feed-forward). Written by the developer capture `Run::Dump`
(`VIBE_LRPT_IQ_DUMP`, `vibe_lrpt_run.h`) on Stuart's server — exactly what the receiver was fed.

## What the recording says (measured, not guessed)

Tools: `vibeserver/tool-lrpt-replay.cpp` (the server's own `Run`, the logged Doppler); a scratch Welch PSD / x⁴ scanner
(2048-point Hann PSD a second against the pre-pass floor t = 20–250 s; a 65 536-point x⁴ FFT a second).

| | |
|---|---|
| **In-band S/N** | (signal power in ±45 kHz of its centre) ÷ (floor in the same band): ≥ 0 dB only **467–503 s**; peak **+6.0 dB at 485 s** (Es/N0 ≈ S/N + 1–1.4 dB ≈ 7 dB). 330–466 s: −1 to −5 dB (Es/N0 ≈ −4 … 0: undecodable by anyone). |
| **The receiver's own reading** | the demodulator's M2M4 Es/N0 agrees with the PSD to ~1 dB (e.g. 482 s: PSD → 7.0, M2M4 5.8). |
| **Carrier** | the x⁴ line sits at **feed-forward − 3340 Hz**, constant to ±3 Hz over 470–490 s (e.g. 480 s: line −2706.5, feed-forward +634.1). The Doppler sign, units and the single application are right; the −3.34 kHz is a fixed radio / LO error (≈ −24 ppm at 137.9 MHz). `--no-doppler` decodes the same frames. |
| **Slope** | −24 Hz/s at the top of the pass (the log and the x⁴ line agree) — well inside the loop. |
| **A gain step** | the whole slice's level drops **5.6 dB within 50 ms at 510.47 s** (−20.1 → −25.7 dB) and the satellite sinks to the floor with it (S/N −10 … −1 dB after). Something changed the radio's gain at **14:28:20.5 UTC** — **needs a look at that server's log** (not touched from here). |
| **The 137.86 MHz carrier** | a narrow line at −41.7 kHz in the slice, +19 dB per bin over the floor, ≈ −8 dB of the noise in the signal's band: ≤ 0.7 dB of harm, and its x⁴ line is ~48 dB under the signal's. Not a factor. |
| **Threshold on this signal** | with the old decoder, CADUs passed 89 % at Es/N0 4.1, 11 % at 3.4, 0 % at 3.0–3.8 (488–490 s). |

## Root causes (in order of what they cost the pass)

1. **Soft differential decoding before the Viterbi** (`DiffDecoder`: each soft value × the last). Multiplying two noisy
   values costs ~1.5 dB at Es/N0 3–5 dB: synthetic OQPSK + differential needed **Es/N0 ≈ 5 dB** (0 % of CADUs at 4 dB)
   where plain QPSK decodes at 3. Min-sum instead of the geometric mean: 1 → 7 % at 4 dB — not the fix (tried, reverted).
2. **"Decoding" never ended once the ASM was lost.** `FrameSync::decoding()` counted *failed decodes*; after the sync
   dropped there were none, so it read "decoding" for ever, the demodulator held its acquisition off for the rest of the
   pass, and the loop random-walked 2–3 kHz off in the fade. **This is the "never relocked".**
3. **Rate AUTO walked 72k ↔ 80k** (a client key attaches without a rate; the pick's entry names 72k but the client key
   did not carry it): the 80k group's 24 s dwell sat on **470–483 s, the best 13 s of the pass**. Replay: AUTO 8 frames,
   72k pinned 20.
4. **Acquisition on the raw 300 kHz slice** (6 dB more noise than the signal's band, raised to the 4th power) and only
   "while unlocked" — which on OQPSK (lock metric 0.72–0.78 while decoding, bar 0.80) meant always, as an absolute reset
   of the NCO. With −3.34 kHz to find it never fired below Es/N0 ≈ 4.5 dB.
5. **"Pass over (no frame for 30 s)"** at 517 s while the engine was still tracking — a fade or gain step is a gap in the
   picture, not the end of the pass. And the SATELLITE window then showed the stale "picture complete".

## What changed

| File | Change |
|---|---|
| `vibe_lrpt.h` `FrameSync` | **The differential code decoded WITH the convolutional code**: the per-stream accumulator commutes with the K=7 code (both linear: g∗(u/(1+D)) = (g∗u)/(1+D)), so the ordinary 64-state Viterbi decodes the accumulated data u′ from the RAW soft values and u = u′ ⊕ u′ delayed. The sign pattern (which stream alternates) follows from the sync's ambiguity (`softJoint`); the streams' relative sign is tried both ways when RS fails and kept. **Same cost.** The ASM search still runs on the differentially decoded stream. `decoding()` counted in soft values arriving (8 CADUs' worth). |
| `vibe_lrpt.h` `Receiver` | the 80k group leaves after **4 s without a 0x27 marker** (the marker is found before the 16 s deinterleaver); `preferRate()` takes the satellite engine's rate. |
| `vibe_lrpt_demod.h` | **x⁴ acquisition on the matched filter's points** (144 k/s, after the NCO), six 8192-point looks, a line 6× the mean bin, applied as a **correction** when > 60 Hz, whenever frames are not decoding. **The leash**: once frames have decoded with a feed-forward on, the loop stays within 150 Hz of where it decoded (a found line moves the leash). **M2M4 Es/N0** (signed, so noise averages to ≈ 0). |
| `vibe_lrpt_run.h` | **the pass ends when the satellite engine says it has SET** (5 s after the last frame), never while it is in range; with no pick, after 30 s with neither a frame nor a signal. `lrpt_status` gains `snrDb` ((S+N)/N from the M2M4 Es/N0 — what the waterfall shows over its floor at the top of the signal), `needDb` (5), `saved`, `inRange`. |
| `vibe_sat_engine.h`, `vibe_decoder_host.h` | `lrptSymbolRateOf` / `inRangeOf` from the listener's pick (`decoderView`); `Env::satInRange` override for tests. |
| `src/services/lrpt.ts` | "signal +X dB over the noise (needs about +5)" instead of "lock N % (80 % holds it)"; "between frames, still listening"; "last pass saved to the gallery · …" — the live state. |
| `web/client/src/satellites.ts`, `src/components/SatellitePanel.tsx` | LRPT's own line wins over the stale "picture complete" (SSTV unchanged). |
| `src/components/AboutOverlay.tsx` | beta91 note. `sdrTour` / `pickerTour` / website / watch say nothing about LRPT's status line (grepped: no change). |
| `tool-lrpt-replay.cpp` | `--rate`, `--engine`, `--no-doppler`, `--from/--to`, `--png`, `--cadu`, per-second Es/N0 / acquisitions / leash, lines with picture per channel. |
| `scripts/run-tests.sh` | `test-lrpt` built `-O2` (ctest already `-O3`): 42 s → 10 s. |

## Results

**The recorded pass** (`tool-lrpt-replay`, M5, `-O3`):

| | frames (picture VCDUs) | picture | pass |
|---|---|---|---|
| live, on the server (old) | 10 (482.7–487.7 s) | 32 lines | cut at 517 s, a new receiver |
| replay, old code | 8 | 32 lines | cut at 517 s |
| replay, AUTO (a client key, no rate) | **56** | one 1568 × 888, 240 lines with data | one pass |
| replay, `--engine` (the pick's 72k, in range while tracked) | **60** | one 1568 × 888: ch 1 240, ch 2 200, ch 4 240 lines with data | one pass |

Frames now come **471–490 s** (8–9 of 9 CADUs a second, i.e. continuously wherever Es/N0 ≥ ~3 dB), **501–508 s**, and
after the gain step **564–568 s, 583–585 s, 594–608 s**. 491–500 s (S/N 0–1 dB) and 330–466 s (below 0 dB) are below
what any decoder can do. The picture: `final--engine-0.png` in the scratch dir (cloud and sea; channels 1, 2, 4).

**Synthetic** — `test-lrpt` **58 / 0** (was 52; new: the live pass rebuilt — OQPSK + differential, −3.34 kHz, −25 Hz/s
fed forward: **Es/N0 3.0 dB 48 %, 3.5 dB 84 %, 5.0 dB 90 %** of picture blocks, was 0 % at 3.5 and 4 dB; a **35 s fade
through `Run`**: in range → one picture and frames after it (with the old `decoding()` and no leash: **0 frames after** —
checked by building the test against the old logic), no pick → two pictures). Noise: 0 pictures as before; **0 false
acquisitions on the recording's 300 s of pre-pass noise**; `snrDb` there averages **+0.14 dB** (max 0.7). 
`test-lrpt-host` 33 / 0, `scripts/test_lrpt_app.ts` 22 / 0 (was 16), `tool-lrpt-replay --selftest` ok.

**CPU** (M5, `-O3`): one OQPSK chain on 60 s of noise at 300 kS/s 0.61 % → **0.65 %** of a core (the joint decode is
the same Viterbi; the point-domain acquisition replaces the raw one). Whole recording: 4.4 s → 5.9 s (AUTO) / 7.3 s
(`--engine`) for 655 s — not a per-chain cost but **more time in the 72k group's two-demodulator contest** (QPSK and
OQPSK side by side until 12 frames) instead of the one-chain 80k group. On a Pi 2 that is the existing ~30–40 % a chain
while the contest runs (LRPT.md §Cost), for longer.

## Open decisions for Stuart

- The METEOR entries could also name **OQPSK** (both N2-3 and N2-4 are), so a pick runs one demodulator instead of the
  contest (−45 % CPU during it). Not done: the entry has no modulation field today.
- **The gain step at 14:28:20.5 UTC** (5.6 dB, the satellite lost under the floor after it): what changed the gain?
  An AGC, another listener, a band default? Worth a look in that server's log.

## Needs a live check

- A real pass with the merged server: frames from about Es/N0 3 dB ("signal about +5 dB over the noise"), the picture
  held open across gaps while SATELLITE tracks, saved at LOS, and the SATELLITE line showing the live state afterwards.
- The web page's embedded copy (`vibe_web_page.h`) is **not rebuilt** on this branch — the coordinator's
  `build-web.mjs` at merge.

## RESUME HERE

1. Merge `b91-lrptfix`; rebuild the web page; one live pass with the capture on (`VIBE_LRPT_IQ_DUMP`) — replay it with
   `tool-lrpt-replay <cf32> --engine --png <prefix>` and compare frames with this table.
2. If passes still drop out mid-pass, look first at the server's gain changes during the pass (the 510 s step here).
3. Optional: a modulation field in the satellite entries (above); an 80k pass on air (still synthetic-only).
