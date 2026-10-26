# JS8 and FT2 on the FT8 core (V12, 2026-10-26)

Two digital-spot modes built on the FT8/FT4 decoder (`decoders/ft8_decoder.*`, ft8_lib, MIT, NEON/SSE2). **Wired into
Digital spots** (`decoders/digi_mode.h`, docs/v12/DIGITAL-SPOTS.md) by `decoders/digimodes/js8.h` and `ft2.h`: they
run in Single mode on the listener's audio and in Band mode on their own dials, as "JS8" (map letter J, bluish green)
and "FT2" (2, yellow), and the gallery's summary cards count them. See "Wired in" below.

| | JS8 | FT2 |
|---|---|---|
| Files | `decoders/digimodes/js8_decoder.*`, `js8_ldpc.*`, `js8_message.*` (in `digimodes/` so every build globs them) | `decoders/ft2_decoder.h`, plus `FtxMode::FT2` in `ft8_decoder.*` |
| Mode (registry) | `decoders/digimodes/js8.h` | `decoders/digimodes/ft2.h` |
| Live engine | `Js8Decoder(rate, submodeMask)`: all four submodes at once | `Ft2Decoder(rate)`, which is `Ft8Decoder` in FT2 mode |
| One-slot engine | `Js8SlotDecoder(rate, submode)` | `Ft8SlotDecoder(rate, FtxMode::FT2)` |
| Tests | `vibeserver/test-js8.cpp` | `vibeserver/test-ft2.cpp` |
| Bench | `tools/js8-bench/js8_wav` | `tools/ft8-bench/ft8_wav --ft2` |
| Recordings | `~/VibeSDR-test-audio/js8/` | `~/VibeSDR-test-audio/ft2/` |

Both tests are registered in `scripts/run-tests.sh`. Their real-recording sections run when
`VIBE_TEST_AUDIO=~/VibeSDR-test-audio` is set.

## Sources and the rule we followed

- **JS8.** JS8Call's own source (GPL-3) is the only complete description of the protocol. We **read** it to learn
  the facts below and wrote everything ourselves. No code, structure or table was copied. Two kinds of tables were
  needed:
  - **The LDPC(174,87) parity-check matrix.** This table *is* the code and cannot be derived from anything. We took
    it from ft8_lib's own early history (Kārlis Goba, **MIT**, the same library and licence as `ft8_lib/`), commit
    `6eaf85f^`, file `ft8/arrays.h`, converted to 0-origin. Every other table (each bit's checks, the generator) is
    built from it at start-up by GF(2) elimination.
  - **Protocol words.** These are the group names, the 32 command words, the CQ wordings and the 44-character
    Huffman code. They were typed in from their meaning and are checked by tests: the Huffman code is prefix-free
    and complete, and calls, grids and frames round-trip.
- **FT2.** IU8LMC's Decodium (GPL-3) is where FT2 was defined. Only facts were taken, and they are listed below with
  their sources. FT2 turned out to be a parameterisation of FT4, so there is no new decoder code to write.

## JS8: the protocol, as built

The on-air frame is FT8's first frame (2017), kept when FT8 itself moved on:

- **Symbols.** 79 channel symbols of 8-FSK. A 7-symbol Costas array sits at symbols 0, 36 and 72. Between the first
  two arrays are 29 *parity* symbols; between the last two are 29 *information* symbols.
- **Modulation.** Three bits per symbol, MSB first, **no Gray map**. Plain continuous-phase FSK (no Gaussian
  shaping), with the tone spacing equal to the symbol rate.
- **Coding.** LDPC(174,87). There are 87 information bits: 72 message bits, then 3 flag bits (1 = first frame,
  2 = last frame, 4 = data), then a CRC-12.
- **CRC-12.** Polynomial 0xC06. The 75 bits plus 13 zero bits are divided through it, MSB first, and the remainder
  is XORed with 42. JS8 adds that XOR so that FT8 decoders never accept a JS8 frame.

| Submode | Letter | Samples/symbol @ 12 kHz | Symbol | Tone spacing | Period | Starts at | Costas arrays |
|---|---|---|---|---|---|---|---|
| Normal | A | 1920 | 160 ms | 6.25 Hz | 15 s | 0.5 s | FT8's (4,2,5,6,1,3,0) ×3 |
| Fast | B | 1200 | 100 ms | 10 Hz | 10 s | 0.2 s | (0,6,2,3,5,4,1) · (1,5,0,2,3,6,4) · (2,5,0,6,4,1,3) |
| Turbo | C | 600 | 50 ms | 20 Hz | 6 s | 0.1 s | same three |
| Slow | E | 3840 | 320 ms | 3.125 Hz | 30 s | 0.5 s | same three |

Frames (`js8_message.h` has the full layout):
- **Heartbeat / CQ:** a 50-bit call, a 15-bit grid, and a 3-bit CQ wording.
- **Compound:** a 50-bit call (for calls such as "VE3/LB9YHX") with a grid or a command.
- **Directed:** a 28-bit from-call and a 28-bit to-call (both can be groups such as @ALLCALL), a 5-bit command,
  /P flags and a 6-bit number.
- **Data:** text.

### The one thing not decoded: compressed text

JS8 sends free text mostly through **JSC**, a word dictionary of 262,144 entries. That dictionary is a large JS8Call
(GPL-3) table that cannot be derived, so we do not include it. Such a frame still decodes: its CRC checks and it is
counted and reported as `data-compressed`. Only its text is unknown (`textKnown = false`). Uncompressed (Huffman)
text frames decode in full.

Every callsign, grid, CQ, heartbeat and command does decode, and those are what a spot is made of.

## FT2: the parameters, pinned down

**FT2 is FT4 with the symbol length halved.** IU8LMC's source confirms this.

**Sources (all GPL-3; facts only):**
- Decodium 3.0 Shannon (https://github.com/iu8lmc/Decodium-3.0-Shannon):
  - `lib/ft2/ft2_params.f90`: the frame and DSP constants.
  - `genft2.f90`: the Costas arrays and the scrambling vector.
  - `gen_ft2wave.f90`: the GFSK BT and modulation index.
- Decodium 4.0 Core Shannon (https://github.com/iu8lmc/Decodium-4.0-Core-Shannon):
  - `Modulator/FtxMessageEncoder.cpp` and `FtxWaveformGenerator.cpp`: FT2 and FT4 share one tone mapper and one
    GFSK routine.
  - `widgets/mainwindow.cpp`: the T/R period, tone spacing and band dials.
  - `Detector/FtxFt2Stage7.cpp`: the decoder's DT range.
- sdroxide (Rust, GPL-3): an independent FT2 implementation whose header lists FT4 and FT2 side by side.

| Parameter | Value | Confidence |
|---|---|---|
| Modulation | 4-GFSK, BT 1.0, modulation index 1.0 | confirmed in source |
| Samples/symbol @ 12 kHz | **288** (24 ms; 41.667 baud) | confirmed in source |
| Tone spacing / bandwidth | 41.667 Hz / ≈167 Hz | confirmed in source / inferred |
| Symbols | 105: a ramp, then four 4-symbol Costas groups at 0-3, 33-36, 66-69 and 99-102 (counted from the first after the ramp), with 29 data symbols after each of the first three, then a ramp | confirmed in source |
| Costas tones | (0,1,3,2) · (1,0,2,3) · (2,3,1,0) · (3,2,0,1), exactly FT4's | confirmed in source |
| Coding | LDPC(174,91) + CRC-14, exactly FT4's; 77-bit message XORed with FT4's sequence; FT4's Gray map | confirmed in source |
| Message | standard WSJT-X 77-bit (`pack77`) | confirmed in source |
| T/R period | **3.75 s**; each transmission lasts 2.52 s | confirmed in source |
| Transmit start | 100 ms (Decodium 3); 0 ms on Linux, 300 ms elsewhere (Decodium 4) | confirmed in source |
| Decoder DT range | −0.52 … +1.52 s (Decodium 4) | confirmed in source |
| Dials (MHz) | 1.843 · 3.578 · 5.357 · 7.052 · 10.144 · 14.084 · 18.108 · 21.144 · 24.923 · 28.184 (Stuart's UberSDR list; 14.084 and 18.108 also checked in Decodium) | stated |

**Cautions.**
- **Old name clash.** Official WSJT-X carries an older, unrelated experimental "FT2" (2-tone, LDPC(128,90)). That is
  not this mode.
- **Async mode.** Decodium 4's "ASYMX / Async L2" decodes the last 3.75 s every 100 ms. Its transmissions may
  therefore drift off the slot grid. Our wide time search covers the documented DT range; a fully free-running
  transmitter is not covered.
- **Unverified.** WSJT-X Improved 3.1's FT2 (DG2YCB) has not been checked against this.

### How FT2 is built

FT2 is `FtxMode::FT2`. ft8_lib's monitor now takes an optional symbol period, slot length and time-search range.
When those are 0 the protocol's own values apply, so **FT8 and FT4 are bit-for-bit as before**: `test-ft8-fast`,
`-passes` and `-callhash` pass. For FT2:
- the window holds 5 s from the slot boundary, overlapping the next slot by 1.25 s;
- the search runs over −22 … +64 blocks (−0.53 … +1.54 s);
- DT is measured from the slot boundary.

Everything else is FT4's machinery at 288 samples per symbol: the PFFFT STFT, the NEON/SSE2 sync search and BP,
OSD, and subtraction (three passes).

## How the JS8 engine decodes

1. **Waterfall.** ft8_lib's STFT at the submode's symbol length, two time steps and two frequency steps per symbol
   (PFFFT; Turbo's 1200-point FFT is not a PFFFT size, so it uses Kiss).
2. **Sync search.** Searched over the three Costas arrays, **sixteen frequency offsets per NEON/SSE2 vector**
   (uint8 level differences summed as int16). The time range is −1.5 … +2 s around the submode's start.
3. **Likelihoods.** Max-log over the eight tone *amplitudes*, MSB first, with no Gray map.
4. **Belief propagation.** Sum-product with Padé approximations for tanh and atanh, plus early exit on
   no-progress.
5. **Coherent stage (spare CPU only).** Applies to a candidate that BP failed on. The candidate's audio is mixed to
   baseband and boxcar-decimated four lanes at a time. It is then slid ±½ symbol and ±¼ tone against the Costas
   arrays, and each data symbol is correlated with its eight tones over exactly one symbol (a matched filter). Bit
   likelihoods come from the noncoherent FSK likelihood, ln I0 with log-sum-exp. BP then runs again.
6. **OSD-1 (spare CPU).** CRC-12. Then guards: never the all-zero codeword, and at most 40 hard errors.
7. **Unpack.** The frame is unpacked; a frame that cannot be a JS8 frame is dropped.
8. **Pass 2 (spare CPU).** Each decoded signal is subtracted: fine start and frequency on a decimated baseband,
   then a phase-continuous CPFSK reference with a ±1-symbol amplitude box. Then the search runs again.

**SNR.** The estimator is the FT8 decoder's. On the synthetic band it gives true SNR in 2500 Hz to within 3-4 dB.
JS8 stations' own reports, which PSKReporter shows, read **11 dB lower**: our figure minus STUEY3D's ranged from
+10.5 to +12 dB for the six stations both heard. So a spot's SNR is reported on the network's scale
(`Js8SlotDecoder::kSnrScaleDb = −11`). After that correction, the 40 m hour agrees with STUEY3D to a mean of
+0.35 dB across nine stations.

**The live decoder** cuts every enabled submode's window from **one** int16 ring by capture time (the FT8 decoder's
anchor), copies it and queues it for **one** worker thread. If a submode's previous window is still waiting or
decoding, the new slot is **skipped and counted** (`slotsSkipped`), never waited for. When `loadProbe()` reports
load, the decode is one pass with no coherent stage and no OSD. A second pass runs only when it would end within
40 % of the time left in the period and no other submode's window is waiting.

## Results

### Synthetic band

Four stations per slot, white noise, true SNR in 2500 Hz, the live decoder's full depth (two passes, coherent stage,
OSD). The figures are decodes out of signals sent.

| Submode | −12 | −16 | −18 | −20 | −22 | −24 | Expected order |
|---|---|---|---|---|---|---|---|
| Turbo | 8/8 | 0/8 | 0/8 | | | | 3.2× Normal's symbol rate: about 5 dB worse |
| Fast | 8/8 | 8/8 | 2/8 | 0/8 | | | 1.6×: about 2 dB worse |
| Normal | 8/8 | 8/8 | 8/8 | 4/8 | 0/8 | | |
| Slow | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 0/4 | 0.5×: about 3 dB better |

- **Pure noise:** 0 decodes in all four submodes: 25 slots each (10 for Slow), 300 candidates a slot, with BP,
  the coherent stage and OSD run on every candidate.
- **FT2:** decoded 12/12 at −10 dB, 2/12 at −12 and 0 at −14, across DT −0.4 … +1.3 s. That fits IU8LMC's "about
  −12 dB". Pure noise gave 0 decodes in 60 slots.
- **Live decoders:** all four JS8 submodes decoded from one stream on their own slot boundaries; FT2 decoded 7/7
  slots fed as float audio. Nothing was skipped.

### Real signals (Stuart's UberSDR, 2026-10-26)

STUEY3D's PSKReporter reports are saved in `~/VibeSDR-test-audio/js8/pskreporter/`.

- **JS8, 40 m (7.078 MHz), 07:30-07:45 UTC.** 28 frames: heartbeats, and directed HEARTBEAT SNR replies with
  calls, grids and SNRs. **Every one of the 9 senders STUEY3D reported in the window was decoded**: DL1JBE, EA3IHV,
  9A3SWO, LA7HKA, IU2SKJ, KE2DMC (−24 against STUEY3D's −25), I2GPG, OZ9JEP and DL1AAA. We also decoded M0IQF in
  **Slow**, whom STUEY3D did not report. 20 m and 80 m had no JS8 in STUEY3D's reports, and we decoded nothing
  there.
- **FT2, 20 m (14.084 MHz), 07:34-07:44 UTC.** 15 decodes (CQ YO8BJK KN37; YO8BJK SP6FXY JO70). These are **both**
  senders STUEY3D reported in the window: YO8BJK at −14 against their −16, and SP6FXY at +2.5 against their +6.
  On 40 m (7.052 MHz) there was none, and we decoded nothing.

## CPU and RAM

Measured on the M5 Mac, one core, with the real recordings:

| Engine | Full depth | Loaded (1 pass, no extras) | vs FT8 full depth on a busy 40 m (0.93 ms/s) |
|---|---|---|---|
| JS8 Turbo (6 s) | 3.5 ms/slot | 1.2 | |
| JS8 Fast (10 s) | 3.8 | 1.2 | |
| JS8 Normal (15 s) | 6.5 | 2.3 | |
| JS8 Slow (30 s) | 7.7 | 2.6 | |
| **JS8, all four** | **1.65 ms per second of audio (0.17 % of a core)** | 0.56 ms/s | **≈ 1.8×** (≈ 0.6× loaded) |
| FT2 (3.75 s) | 3.1 ms/slot = 0.83 ms/s | ~0.6 ms/s | ≈ 0.9× |

**Pi 2: estimated, not measured.** We had no Pi 2 to profile, and the qemu instruction counter's Docker images
(`tools/ft8-bench/qemu`) were not on this Mac. Taking 20-30× an M5 core, JS8 with all four submodes would use about
3-5 % of one Pi 2 core at full depth and 1-1.5 % when loaded; FT2 about 2 %.

**Where JS8's time goes (Normal, full depth).**
- The waterfall (PFFFT) takes 0.9 ms; the vector search 0.2 ms; BP 1.4 ms; the coherent stage 3.8 ms (spare CPU
  only); subtraction 0.2 ms.
- BP was not vectorised, because at 1.4 ms per 15 s it does not pay. The coherent stage's mixer is four lanes wide.

**RAM, JS8 (all four submodes): about 4.5 MB.**
- The ring holds the longest window plus 3 s: 33 s of int16, 0.8 MB.
- One float window per submode: Normal 0.71 MB, Fast 0.48, Turbo 0.29, Slow 1.43.
- Waterfalls: 0.78 MB.

Running fewer submodes (the `submodeMask`) drops that submode's window and waterfall.

**RAM, FT2: about 1 MB.**
- The 5 s window (0.24 MB) and an 8 s ring (0.38 MB).
- The waterfall (0.07 MB) and the subtraction scratch (0.24 MB).

## Wired in (2026-10-26)

| | JS8 | FT2 |
|---|---|---|
| ModeDef | slot 15 s (its engine cuts Fast 10 s, Turbo 6 s and Slow 30 s itself), 12 kHz, audio 100-3300 Hz, costVsFt8 1.8 | slot 3.75 s, 12 kHz, audio 100-3500 Hz, costVsFt8 0.9 |
| Dials (digi_bands.h) | JS8Call's: 1.842 · 3.578 · 7.078 · 10.130 · 14.078 · 18.104 · 21.078 · 24.922 · 28.078 · 50.318 MHz | Decodium's, as Stuart's UberSDR decodes them: 1.843 · 3.578 · **5.357** · 7.052 · 10.144 · 14.084 · 18.108 · 21.144 · **24.923** · 28.184 MHz (60 m and 12 m added) |
| Spot | only when `Js8Spot::isSpot()`: a real sender's call (heartbeat, CQ, compound, directed — never a group, never a text fragment). Grid only from a heartbeat/CQ/compound frame (the sender's own). Text = the frame + its submode ("G4ABC: @HB HEARTBEAT IO91 (Turbo)") | as FT8's engine (`onSpot(to, de, grid, snr, hz)`, real calls and real grids only) |
| Server loaded | one pass, no coherent stage, no OSD | one pass |

**Single mode** is the cheap button: it runs only the modes whose dial the listener is on (docs/v12/DIGITAL-SPOTS.md,
"How the modes are chosen"). So JS8 decodes at 7.078, 14.078 and so on, and FT2 at its dials; tuned off the table,
Single runs FT8 + FT4 as it always did.

Before that rule existed, every mode ran everywhere, and the engines were checked against each other's traffic. JS8
on 21 minutes of busy real FT8 audio (40 m and 20 m) decoded 0 frames. FT2 needed one change: it runs **without OSD**
in the live decoder. With OSD on, it took real FT8 audio for about 120 CRC-14-valid codewords in 15 minutes, none of
them a message that unpacks. OSD added nothing to FT2's real decodes (15/15 either way) or to its synthetic threshold
(−10 dB 12/12, −12 dB 2/12 either way). That rule stays: a shared dial (3.578) or a wide passband still puts FT2
beside other traffic.

The web client's Single title is now "DIGITAL SPOTS" on a server with more than two modes; the About V12 note names
FT2 and JS8.

**FT4's SNR** was also moved onto the reference scale. FT4 and FT2 now share it: −9 dB, one line in `snrDb`. Against
STUEY3D's PSKReporter reports, FT4 on 20 m (four stations, 2026-10-25) read +8.6 dB before and **−0.4 dB** after. On
40 m (three stations, two recordings) it reads −1.1 dB after; individual stations scatter by ±4-8 dB, as single
reports do.

**End to end.**
- **`test-digi-spots`** (synthetic 20 m slice, 75 kHz). There are four engines: FT8 14.074, JS8 14.078, FT4 14.080
  and FT2 14.084. A JS8 Normal and a JS8 Turbo heartbeat and two FT2 CQs were each spotted with their mode at their RF,
  next to the FT8/FT4 stations. Noise gave 0 spots. Single mode was also tested at a JS8 dial (7.078) and at an FT2
  dial (14.084): the spot frame says "JS8"/"FT2", with the right band, grid and RF.
- **`test-server-spots`** (the real server, a fake rtl_tcp clip that now carries a JS8 heartbeat and an FT2 CQ):
  `spotModes` is ["FT8","FT4","FT2","JS8"]. 20 m's span is 14.0739-14.0878 MHz (13.9 kHz). Every station was spotted,
  including the JS8 and FT2 ones, on a one-pipeline radio and on a range-locked one: 32/32.
- **`test-web-spots`** (headless Edge, `--mute-audio`, desktop and 390 px): all four modes are listed in their own
  colour and letter, and the map legend shows "FT8 FT4 FT2 JS8": 26/26.
- **`test-bgdecode`**: a card from JS8 + FT2 spots is titled "JS8+FT2 last 10 min · 3 spots" and counts "JS8 2, FT2 1".

## What remains

- **JSC-compressed text** is not decodable without JS8Call's dictionary; such frames are counted, with no text.
- **Sensitivity.** Normal sits about −19/−20 dB in true 2500 Hz SNR. That is about ft8_lib's level and below the
  best WSJT-style decoders. The next step would be multi-symbol coherent detection: JS8 is phase-continuous, so
  2-3 symbols could be detected jointly.
- **Engine scope.** One JS8 engine per dial runs all four submodes. Several dials in Band mode means several
  engines, at the cost per dial shown above.
- **Background decoding** does not offer JS8 or FT2 as a VFO decoder yet (its FT8/FT4 VFO is its own engine).
- **The app** shows them through the shared tables (`src/utils/digiModes.ts` already had J and 2); its own Band mode
  is still owed (docs/v12/DIGITAL-SPOTS.md).
