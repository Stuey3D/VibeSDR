# DRM (Digital Radio Mondiale) — V12

Built 2026-10-26 on branch `v12-drm` from the published standard, **ETSI ES 201 980 V4.3.1 (2023-11)**. Research
section: `DECODER-RESEARCH.md` §4.4.

## What a listener gets

- **DRM is a demodulator, like AM** — in the web client's mode picker wherever the server lists `drm` in
  `vibeserver.json`'s `modes`. DRM30 is ≤ 10 kHz wide, so it is a *listener* mode, not a whole-radio mode like DAB:
  - on a range-locked radio each listener has their own DRM receiver on their own VFO;
  - on a one-pipeline radio (the shared dial) one receiver serves everybody, and the shared-dial contract is
    unchanged (the server sends full state; nothing retunes on anyone's behalf).
- **The DRM box** (`web/client/src/drm.ts`), filled from `{"type":"drm"}` reports on the spectrum socket (≤ one per
  1.2 s super frame, sent only when something changed). It shows:
  - the station's label, its text message, and its language, country and programme type;
  - six lights in the order a receiver gains them: time → frequency → frame → FAC → SDC → MSC;
  - the SNR (the FAC cells' MER);
  - one plain line for the signal: `Robustness B · 10 kHz · 16-QAM · rate 0.62 · long interleave`;
  - the codec;
  - when the server cannot play the codec, the server's own sentence saying why.
- **The Signal pane** (2026-10-26) — the box's second view, behind **SIGNAL ▸** in its header (◂ STATION goes back),
  on the web page and in the app. See [The Signal pane](#the-signal-pane-2026-10-26) below.
- **The station bar (VTS)** carries the label and the text, with the service ID as the id, as for DAB.
- **The bandwidth row is hidden in DRM.** The receiver cuts its own channel, so the slider would do nothing.
- **Audio** replaces the AM detector's output, at the detector's own pace. It is buffered 0.8 s deep, because DRM
  delivers audio in 400 ms super frames. There is silence until there is audio, and the stream never stalls.

## Architecture (as built)

| File | What |
|---|---|
| `decoders/drm/drm_params.h` | Modes A–D at **24 kHz**: Tu/Tg = 576/64, 512/128, 352/128, 224/176 samples; every carrier is an FFT bin. Pilot tables (§8.4), FAC positions, the super-frame cell map. Its counts are checked against tables 25, 41–44 and the 65 FAC cells — all equal. |
| `decoders/drm/drm_coding.h` | Energy dispersal, CRC-8/16 (annex D), puncturing (tables 27/28), the K=7 rate-1/6 mother code, a soft max-log Viterbi, the bit and cell interleavers, QAM. **SIMD demapper** (NEON incl. ARMv7 via `neon_compat.h`, SSE2, scalar). |
| `decoders/drm/drm_mlc.h` | Multilevel coding both ways. Multistage decoding: level 0, re-encode, condition level 1 … then a second pass with every other level known. The MSC cell (de)interleaver (short, or long D=5). |
| `decoders/drm/drm_info.h` | FAC (validated beyond the CRC: reserved values are failures), SDC entities 0/1/8/9/12, text messages (§6.5), readable language and programme-type names. |
| `decoders/drm/drm_receiver.{h,cpp}` | The receiver — see below. |
| `decoders/drm/drm_audio.h` | Audio super frames → access units. xHE-AAC: header CRC, directory, frames spanning super frames (incl. 0xFFE/0xFFF), CRC-16 per AU. AAC: borders and frames. Also the DRM xHE-AAC static config → standard `UsacConfig` → AudioSpecificConfig (AOT 42), and LOAS/LATM framing. |
| `decoders/drm/drm_tx.h` | **Test transmitter** (FAC/SDC/MSC from the standard's own arithmetic, OFDM at 24 kHz) and annex B's channels 1–5 (WSSUS, Gaussian Doppler), plus frequency offset, clock ppm and AWGN. Tests and bench only. |
| `vibe_drm_audiodec.h` | The platform decoder: FFmpeg as a process (LOAS in, 48 kHz stereo out; USAC needs FFmpeg ≥ 7.1, which is probed), AudioToolbox `kAudioFormatMPEGD_USAC` on Apple, MediaCodec on Android. Plus `canDecode()`, which gives the sentence a listener sees. |
| `vibe_drm_listener.h` | One listener's DRM: IQ queue (2 s, oldest dropped and counted), a `vibe-drm` thread at decoder priority, the PCM buffer, the JSON report. |
| `local_sdr_shim.cpp` | Mode `drm` (`paramsFor`, `vsModeKnown`, the offer list). Wiring: the RxPipeline `Callbacks::iq` tap per listener (`clientIqCb`) and on the shared dial (`iqCb`), with `setIqMinRate(24000)`; the audio replaced in `onClientAudio` / `onAudio`; the health card's "too heavy" line. |

### The receiver chain

0. **Resample to 24 kHz.** One windowed-sinc FIR, then cubic interpolation. A pipeline channel already at 24 kHz
   passes straight through.
   - **Pulse blanker:** a run-based detector (an 8-sample mean 6 dB over the running mean, or one sample 13 dB over
     it). It was added because the first real recording carried a 40 Hz pulsed interferer.
1. **Mode and timing** come from guard-interval correlation, folded modulo Ts over 0.8 s.
   - The peak has to stand over the fold's 10th percentile. That is the first false-lock gate: an AM carrier gives a
     flat fold.
   - The peak's phase gives the fractional frequency offset.
2. **Integer frequency** comes from the three reference tones' coherence over one frame, scored per tone, searched
   ±1.5 kHz. Each hypothesis carries its own per-symbol rotation (2π·d·Tg/Tu).
3. **Frame sync** combines two scores:
   - the gain references compared with themselves y symbols earlier — channel-free, and it names s mod y robustly;
   - the time references compared carrier to neighbour, which pins the exact symbol.
4. **Tracking:**
   - frequency from the reference tones' phase step;
   - timing from the channel's averaged delay profile. The centre of its extent is held mid-guard, using integer
     window steps; their rate is reported as the sample-clock offset.
5. **Channel estimation:**
   - gain references interpolated in time — cubic through four pilots (2y−1 symbols of look-ahead);
   - then in frequency by a Wiener filter (sinc correlation over Tg, 12 taps, solved once per plan), centred on the
     delay profile;
   - SIMD equaliser; |H|² is each cell's CSI weight.
6. **Decoding:**
   - FAC every frame. It needs CRC-8 plus valid fields, and two agreeing FACs before "locked" — the second and third
     false-lock gates.
   - SDC every super frame (CRC-16).
   - MSC through the cell deinterleaver and the multistage MLC decoder → logical frames → text message → audio
     framer.

## The Signal pane (2026-10-26)

DAB+'s advanced pane, for DRM (Stuart: "yes that is what we want"). The compact box stays the default; SIGNAL swaps it
for the pane, and the key names where it goes (DAB's rule).

**Where:**
- Web: `web/client/src/drm.ts` `drmSignalRows` (the rows, as data) and `drmSignalRender`, with `main.ts` `drmSetPane`.
  It uses `#dabSignal`'s CSS rules, so the amber label, white value, headings and traffic lights are DAB's. Rows are
  patched in place, so a tooltip being read survives a report.
- App: `src/services/drm.ts` `drmSignalRows` (the web's rows, held equal by `scripts/test_drm_app.ts`), drawn by
  `DrmPanel` in DabPanel's row style.
- Pad and keyboard:
  - web: the box is a `[data-navbox]`. Before this it was a plain `.open` panel, which `gamepad.ts` treated as a scope of
    its own with nothing in it, so the pad could reach nothing in DRM;
  - app: the screen highlight lands on the box (`drmBox`); Enter takes the keys and walks SIGNAL · MIN and every row;
    Back hands back. Unlike DAB's box it does not take the keys by itself, because in DRM the dial is still the radio's.
- An older server sends none of the new fields, and rows it cannot fill are not drawn.

**What it shows** (each row has a plain-English tooltip; in the app it is the accessibility hint):

| Group | Rows |
|---|---|
| LOCK | lock state · seconds of signal to each of the six lights (time, frequency, frame, FAC, SDC, MSC) since this lock began · re-acquisitions, and which test lost the last one |
| SIGNAL | robustness mode (with what it is for) · bandwidth and occupancy · guard interval · main channel QAM, code rate (and part A's) · service channel QAM, rate · interleaving |
| QUALITY | SNR (FAC MER) · MSC MER and SDC MER, each judged by its QAM (about 8 dB decodes 16-QAM cleanly, 14 dB 64-QAM — RRI played without a bad frame at 7–9 dB) · frequency offset · sample clock · **delay spread** against the guard · **Doppler spread** · pulse blanker (last 5–10 s) |
| ERRORS | FAC, SDC and audio frames as *errors*, not passes · concealed frames · audio gaps (buffer ran dry) · IQ gaps bridged · IQ dropped |
| AUDIO | codec · SBR · parametric stereo · audio mode · sample rate · bit rate · decoder |
| STATION | label · service id · programme type · language · country · station clock · text |

**New report fields** (`vibe_drm_report.h`, new and optional; every old field is unchanged): `so`, `codeRateA`,
`sdcRate`, `sdcMer`, `delayMs`, `dopplerHz`, `sbr`, `ps`, `underruns`, `sdcOk`, `sdcBad`, `superFrames`, `lockS[6]`,
`reacq`, `lostWhy`, `lostAtS`, `blankedPct`, `iqGaps`. A value not measured yet is absent, never a placeholder.

### Delay spread and Doppler — the method

Both come from the channel estimate the equaliser already has (`drm_receiver.cpp`, `delayAccumulate` and
`dopplerAccumulate`), on the `vibe-drm` thread.

**Delay spread** — the RSCI's measure (TS 102 349 `rdel`): the shortest window holding 95 % of the power-delay
profile's energy.
- Every y-th symbol, the time-interpolated gain-reference columns (one every x carriers) are Hann-weighted and put
  through one P-point FFT, P = Tu/x samples. That is the columns' alias-free span, ≥ 2·Tg in every mode.
- |·|² is averaged over ~25 profiles (~2 s in mode B).
- The noise floor (the profile's median) is subtracted, and bins under −30 dB of the peak count as nothing.
- **The receiver's own resolution is removed:** the same chain fed an ideal one-path channel gives its 95 % width
  (~1/bandwidth, widened by the window), and that width is subtracted. A clean channel reads 0, not 0.2 ms.
- The window is not circular. It is unwrapped half a span either side of the timing loop's own centre, so the number
  describes the paths where the receiver has put them.
- **Limit:** the profile repeats every Tu/x samples. A spread over half of that (4.67 ms in mode D, 3.67 ms in C) reads
  the other way round: in mode D, 5 ms apart is 4.33 ms the other way. To the pilots that is the same channel, and both
  readings fit the guard.

**Doppler spread** — 2σ of a Gaussian Doppler spectrum, annex B's definition.
- A carrier's gain reference recurs every y symbols, so the ring holds its raw estimate at lags τ = y·Ts and 2τ.
- Per symbol, R(τ) = Σ H(t)·H\*(t−τ) and R(2τ) over that symbol's references are normalised by their power and
  averaged over ~3 s.
- For a Gaussian spectrum, |R(τ)| ∝ exp(−2π²σ²τ²). White noise adds only to R(0), so the ratio
  |R(2τ)|/|R(τ)| = exp(−6π²σ²τ²) is unbiased by noise. σ follows from it.
- Ratios under 0.02 (fading too fast for these lags) read as the most the lags can tell: ~5.6 Hz in mode B.

**Accuracy** (`vibeserver/test-drm-signal.cpp`):

| Channel | Reads |
|---|---|
| One path, every mode, 12–30 dB | 0.00 ms |
| Two fixed paths 0.5 / 1 / 2 / 3 / 4 ms apart (B), 1 / 2 ms (A), 2 / 4.5 ms (D), 12–30 dB | 0.46 / 0.96 / 1.96 / 2.96 / 3.96, 0.96 / 1.96, 1.96 / 4.46 ms (−0.04 ms bias, SNR-independent) |
| Annex B channel 4 (2 ms, 1 Hz), channel 5 (4 ms, 2 Hz) | 1.96 ms, 3.96 ms |
| Doppler 0.1 / 0.25 / 0.5 / 1 / 2 Hz (two Rayleigh paths, 3 seeds, 15 and 25 dB) | 0.14 / 0.20 / 0.48 / 0.95–0.96 / 2.10–2.15 Hz |
| A static channel | 0.02 Hz |
| **Real:** RRI 9570 kHz (150 s, Stuart's UberSDR) | delay 0.28–0.54 ms, Doppler 0.13–0.35 Hz, every 10 s |

Below ~0.25 Hz the frequency-tracking loop (bandwidth ~0.3 Hz) takes some of the common phase, so 0.25 Hz reads 0.20
and 0.1 reads 0.14. The pane shows "under 0.1 Hz" below that.

**CPU:** not a hot loop, so no SIMD.
- Per symbol: ~35 complex products in mode B. Every y-th symbol: one FFT of ≤ 256 points.
- A/B with `VIBE_DRM_NOSTATS` on the Mac: the difference is under 0.01 % of a core (below the measurement's noise).

### Two timing faults, found by the delay-spread test (2026-10-26)

1. **A timing step moved the FFT window, but not the averaged delay profile that steers it.**
   - The profile kept reporting the old position, so the loop stepped again on every symbol.
   - The result was ~30 samples of overshoot on a static two-path channel, then a walk back.
   - The wandering window smeared the profile it steered from. Both profiles now move with each step
     (`applyStepToRing`).
2. **In mode D the guard is most of the profile's span** (176 of 224), so two paths read either way round with equal
   block energy.
   - A noise-decided flip threw the window 108 samples.
   - Fix: among blocks within 5 % of the strongest, the one nearest the window's centre wins.
   - The block energies are now a sliding sum, O(P), and the extent centres come from next/previous-tap arrays.
   - That made the receiver **cheaper** than before: mode B 0.46 → 0.38 %, mode D 0.40 → 0.30 % of a core.

Effect on decoding (annex B channels, 8 seeds × 32 super frames at 22 dB, frames correct):

| Channel | Before | After |
|---|---|---|
| D 16-QAM ch5 | 410 | 419 |
| B 16-QAM ch5 | 117 | 124 |
| B 16-QAM ch4 | 472 | 467 |
| B 16-QAM ch3 | 675 | 673 |
| A 64-QAM ch2 | 681 | 682 |

RRI 9570 kHz reaches the MSC 1.2 s sooner (4.0 s).

Also fixed:
- `loseSync` now clears the timing profile. A re-acquisition used to be steered by the old window's profile.
- The pulse blanker's mean now warms up over 10 ms. It began from one sample's power and blanked the first 100 ms of
  every lock (10 % of the first second); the first lights now come 0.4 s sooner.

### The flaky `test-server-drm` — found and fixed

**Symptom:** ~3 runs in 10 heard silence in the 8 s audio window. FAC failures climbed; no queue drops were logged.

**How it was found:**
- `VIBE_DRM_DUMP=<dir>` (a diagnostic; off unless set) writes the IQ blocks a DRM listener is fed.
- Replayed offline, a failing run failed the same way, deterministically. So the fault was in the IQ, not the thread.
- Aligning a failing range-locked dump against a shared-dial one showed the lag jumping by exactly **384 samples**
  (one whole block, 10.7 ms at 36 kHz) four or five times a run.

**Cause:**
- A range-locked listener's DSP thread is fed through ~40 ms of slack (`handBlockToListeners`). When it is
  descheduled, its oldest blocks are dropped. On this Mac several agents' builds were running beside the test.
- Audio shrugs a 10 ms hole off. OFDM loses its frame alignment: 16 FACs fail and the lock goes.
- **Every** range-locked run lost the lock at ~11–13 s. A run *failed* only when the audio window fell on the
  re-acquisition. The shared dial has no such queue, which is why only that scenario failed.

**Fix:**
- The listener thread already knew (a jump in the channelizer's block index). It now calls `DrmListener::noteGap`,
  which fills the hole with as many zero samples.
- The timeline keeps its alignment, and the missing cells equalise to zero weight: erasures, which the interleaver and
  the Viterbi absorb.
- Synthetic, 10 holes: un-bridged, 4 re-acquisitions and 17/120 frames; bridged, 0 and 110/120.
- Server: 0 losses of sync in 12 runs since (it was every range-locked run). The test now asserts the lock held.
- The Signal pane counts the holes ("IQ gaps bridged").
- **Not bridged:** holes the radio itself reports (`blk->gap`, from the USB side). Their length is unknown, so the
  receiver re-acquires as before.

The harness had a second, unrelated race: a free port taken before the server bound it ("port already in use"). It now
starts again on fresh ports.

## Codecs — the honest position

> **Update, 2026-10-26:** Stuart chose to build **FAAD2's DRM build into the server** (VIBE_DRM_FAAD2), so DRM's AAC now plays — RRI end to end. It is never in the App Store app. See [DRM-AUDIO-ROUTES.md](DRM-AUDIO-ROUTES.md). The table below is the position before that.


| Station codec | Who can decode it | Status |
|---|---|---|
| **xHE-AAC (USAC)** | FFmpeg ≥ 7.1 (Debian 13 yes; Ubuntu 24.04 = 6.1 no; Pi OS bookworm = 5.1 no), macOS AudioToolbox, Android 10+ where listed | **Built, NOT verified on air.** No xHE-AAC station was receivable during this work, and building a USAC encoder for test content was not permitted. The config rebuild is unit-tested against the standard's syntax. |
| **DRM AAC / HE-AAC** (ER AAC Scalable, object type 20, with HCR + VCB11 and 960-sample frames; SBR stored backwards) | **None of the platform decoders.** FFmpeg 9.0.1 says "AAC data resilience (flags 5) is not implemented". AudioToolbox refuses the format. | Frames are found and counted. The box says why there is no sound. |

**Radio Romania International (all its DRM tested) uses DRM AAC, 12 kHz, mono, ~11.6 kbit/s.** Most of the older
DRM stations will too. So the claim in the brief that "HE-AAC v1/v2 has always been supported" is not true for
*DRM's* AAC.

The fix that keeps "we ship no codec" is a **lossless repack** of DRM's ER-AAC into a plain AAC-LC (960) bitstream,
which FFmpeg decodes:

- undo HCR;
- map VCB11's virtual books 16–31 to book 11;
- move the reversed SBR bits into a fill element.

It needs ISO/IEC 14496-3's HCR clause, which is a paid standard. Stuart's call.

## Test results (`vibeserver/test-drm.cpp`, 134 checks, 0 failed; `scripts/run-tests.sh`)

**The standard's tables** — cell counts, SDC lengths and the PRBS — all match.

**Coding layer (cell domain, AWGN):**
- 64-QAM R=0.6 decodes cleanly from 18 dB. The standard's ideal-receiver figure is 14.9 dB at BER 1e-4.
- 16-QAM decodes cleanly from 14 dB.

**End to end** (frames correct, as a % of the 96 sent over 40 s; the ~4 % shortfall is acquisition plus the 2 s
interleaver):

| Mode, QAM, channel | 10 dB | 14 dB | 18 dB | 22 dB | 26 dB | 30 dB |
|---|---|---|---|---|---|---|
| B 64-QAM, AWGN | 0 | 8 | 96 | 96 | 96 | 96 |
| B 16-QAM, AWGN | 89 | 96 | 96 | 96 | 96 | 96 |
| A 64-QAM, ch 2 (Rice) | 1 | 89 | 96 | 96 | 96 | 96 |
| B 64-QAM, ch 3 (US Consortium) | 11 | 34 | 53 | 64 | 65 | 66 |
| B 16-QAM, ch 3 | 50 | 86 | 91 | 92 | 93 | 92 |
| B 16-QAM, ch 4 (CCIR Poor: 2 ms, 1 Hz) | 31 | 52 | 57 | 58 | 60 | 60 |
| B 16-QAM, ch 5 (4 ms, 2 Hz) | 3 | 11 | 14 | 15 | 12 | 14 |
| D 16-QAM, ch 5 | 48 | 54 | 54 | 50 | 47 | 47 |

- **The fast-fading channels (4, 5) have an error floor.** The diagnostics point at estimation under Doppler:
  - channel 4 frozen (0.05 Hz) decodes 100 %; at 0.3 Hz 97 %; at 1 Hz 68 %;
  - the tracking loops are not the cause (`VIBE_DRM_NOFT` / `VIBE_DRM_NOTT` change nothing).
  - This is the first thing to improve, with a Doppler-aware Wiener in time and a decision-directed refinement.
- **Tuning error and clock:** ±1.2 kHz × ±30 ppm, all clean. The offset reads within 2 Hz; the clock within 6 ppm.
- **False locks:**
  - 10 minutes of noise: 0 FAC passes, 0 locks, 0 frames.
  - 2 minutes of an AM broadcast: 0, 0, 0.
- **Real recordings** (Stuart's UberSDR, 2026-10-26; `~/VibeSDR-test-audio/drm/`):
  - **RRI 9570 kHz, 18:00 UTC:** everything decoded.
    - Mode B, SO 3, 16-QAM MSC, 16-QAM SDC, long interleave.
    - FAC 361/362, SDC 119/120, SNR ≈ 18 dB.
    - Label "SNR Tiganesti E1"; codec AAC mono 12 kHz, 11.6 kbit/s.
    - No text message was being sent (the bytes are 0x00).
  - **RRI 9720 kHz, 17:27 UTC:** mode B / SO 3 / 16-QAM read and FAC 89 % good, but the SDC never decodes.
    - A 40 Hz pulsed interferer at +17 dB (a chirp sweeping the lower half of the channel) forces 18 % blanking,
      which caps MER at ~4 dB.
    - The pulses are 0.93-coherent shot to shot, so per-pulse template subtraction is the next step.
  - **RRI 5910 and 13750 kHz:** nothing receivable at the time (noise).
- **Through the real server** (`scripts/test-server-drm.mjs`, 46 checks since the Signal pane): a fake rtl_tcp replays RRI 9570 kHz, made
  by `scripts/drm-clip.py`.
  - Tested on a one-pipeline radio and on a range-locked one.
  - Checked: the offer, the lock, the label, mode/QAM, the codec, the sentence for AAC, all six lights, audio never
    stalling, and the reports stopping on leaving DRM.
- **Web** (`scripts/test-web-drm.mjs`, headless Edge `--mute-audio`, 35 checks since the Signal pane):
  - the picker offers DRM, and the box fills with the label, lights, signal line, SNR, codec and why;
  - the VTS label appears, and the width row hides and returns;
  - at 390 px nothing scrolls sideways;
  - no exceptions.

## CPU per DRM listener

- **Since 2026-10-26** (the Signal pane's statistics included, the timing loop's block scan made O(P)): mode A 0.37 %,
  B 0.38 %, D 0.30 % of one Mac core, synthetic 64-QAM. The statistics add < 0.01 %. Pi 2 estimate by the same
  25–40× ratio as below: ~8–15 % of one core.
- **Mac (measured, before):** 0.4 % of one core, synthetic 64-QAM, every mode, full decode. 0.53 % for the real RRI file at
  48 kHz input, including the 48 → 24 kHz resampler. The scalar build costs the same; the SIMD kernels are a small
  share.
- **Pi 2 (estimated, not measured):** Cortex-A7 at 900 MHz is ~25–40× slower per core, so **~10–20 % of one of its four
  cores per DRM listener**. The Pi 2 can do it, comfortably for one or two listeners.
  - At 2.4 MS/s the pipeline's channel is exactly 24 kHz, so the resampler is skipped.
  - No benchmark gate is needed.
- **RAM:** ~1 MB per listener (the 2 s IQ queue at 24 kHz is 384 KB; the PCM buffer up to 3 s; the long interleaver
  5 × 2959 cells).

## What remains

1. **The AAC repack.** Without it, the older stations — RRI among them — have no sound.
2. **xHE-AAC verified on a real station**, on FFmpeg 7.1+ and AudioToolbox, including whether the SDC's rate is
   `usacSamplingFrequency` (assumed). The xHE frames-per-super-frame count will settle it.
3. **Multipath under fast fading** (channels 4/5) — see above.
4. **Pulse interference** — subtraction instead of blanking (9720 kHz).
5. **The app** (React Native) — the mode, the box and (2026-10-26) the Signal pane BUILT (APP-PARITY-2026-10-26.md: DrmPanel, drm.ts held
   to drmRender by scripts/test_drm_app.ts); the audio is the server's, so it plays as any mode's. Still owed: the station
   bar (VTS) showing the DRM label. The original note:
   AudioToolbox USAC on iOS, MediaCodec on Android. `vibe_drm_audiodec.h` compiles for Android (NDK armv7/arm64) but has
   not run on a device.
6. **Not built:** spectrum occupancies 4/5 (18/20 kHz, unused on HF); multiple services (the first audio service is
   played); journaline and slideshow (packet mode); AFS.
7. **Robustness mode E (DRM+)** — **not cheap.** It needs a 100 kHz channel and its own sample rate, a 244-cell FAC
   with two service sets, a different pilot algebra (R/Z/Q matrices), 4 frames per super frame, and 4-/16-QAM rate
   sets. The coding layer (`drm_coding.h`, `drm_mlc.h`) carries over; the OFDM front end and tables are about a day's
   work, plus a VHF test signal.
