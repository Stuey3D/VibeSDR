# METEOR-M LRPT (V12): weather-satellite pictures, our own decoder from the published standards

Branch `v12-lrpt` (off `v12-experimental`), 2026-11-19. The Russian METEOR-M weather satellites broadcast their MSU-MR
imager's pictures continuously on 137.100 / 137.900 MHz as LRPT. This decodes them into per-channel greyscale pictures
and a colour composite, live during the pass (a preview growing in the decoder box), and files the full-size pictures
in the gallery as standard PNGs when the pass ends.

**Headline:** a public 2018 METEOR-M N2 recording decodes to a recognisable picture — the Netherlands, the IJsselmeer,
the Wadden Sea and Denmark under bands of cloud — **918 of 922 frames** passing Reed–Solomon, three channels (1 visible,
2 near-IR, 5 thermal). The modes the satellites in orbit use **now** (OQPSK, differential coding, the 80k interleaved
mode) are tested on our own transmitter only: no public recording of N2-3 / N2-4 was found (§Test sources).

## Sources and the house rule

- **CCSDS 131.0-B** (TM synchronisation and channel coding): the attached sync marker 0x1ACFFC1D, the rate-½ K=7
  convolutional code (171 / 133 octal), the pseudo-randomiser h(x) = x⁸+x⁷+x⁵+x³+1 seeded all ones, RS(255,223) E=16 over
  x⁸+x⁷+x²+x+1 with first root 112 and root spacing 11, interleave depth 4. **CCSDS 132.0-B** (TM space data link): the
  transfer frame (VCDU) header, the M_PDU first-header pointer, space packets.
- **ITU-T T.81 (JPEG) Annex K**: the luminance Huffman tables, the quantisation table, zig-zag; the IJG quality scaling.
- **METEOR-specific facts** were READ from dbdexter's `meteor_decode` and `meteor_demod`, which are **MIT-licensed**:
  the CADU layout (1024 bytes, VCDU 892 + 128 parity), the insert zone (2 bytes) and M_PDU header, **no dual-basis RS
  transform**, VCID 5 for imagery, APIDs 64–69 for MSU-MR channels 1–6 (70 = telemetry), 14 MCUs a packet and 196 a line,
  the MCU header (number, 2-byte scan header, 3-byte segment header ending in the quality factor), DC prediction restarting
  each packet, no byte stuffing, 43 packets a scan period, the differential code, and the 80k mode's 0x27 marker every 80
  bits with a 36-branch × 2048 convolutional interleaver. **All code is ours; nothing was copied.** No GPL decoder was read
  (SatDump was not opened).
- Learnt from the real recording, not from any source: the packet TIME is shared by every channel's packets of one scan
  period and steps 1232 ms (sometimes 1228: whole-ms stamps of a ≈ 1231.1 ms period) — the strips are numbered from it.

## Architecture

`android/app/src/main/cpp/` — header-only, shared by VibeServer and the app's local engine.

| File | What |
|---|---|
| `vibe_lrpt_demod.h` | AGC → phasor NCO (Doppler feed-forward + carrier loop) → **64-phase polyphase RRC (α 0.6) evaluated only at the timing points** (2 a symbol) → Gardner → Costas (QPSK, or OQPSK with Q half a symbol late) → int8 soft pairs. Acquisition: FFT of x⁴, four 8192-point looks averaged, a line 8× the mean bin; held while frames decode. |
| `vibe_lrpt_fec.h` | the coded ASM and its **eight ambiguities** (4 rotations × I/Q swap — so G2's inversion and the bit order need not be known), the rate-½ soft **Viterbi** (DAB's destination-side ACS reduced to rate ½; NEON / SSE2 / scalar bit-identical), PN, **RS(255,223)×4** (BM + Chien + Forney, table syndromes), the differential decoder, the 80k deinterleaver. |
| `vibe_lrpt.h` | `FrameSync` (ASM search at every bit offset → Viterbi per CADU with 64-step lead and tail → derandomise → RS: only frames whose four words ALL pass go on), `InterleaveFront` (80k: marker phase/ambiguity search, strip, deinterleave), `PacketAssembler` (M_PDU → packets, gap = drop the partial), `Receiver` (AUTO, packets → strips → channels), `renderPictures` (PNG per live channel + RGB 221). |
| `vibe_lrpt_image.h` | Huffman / dequantise / IDCT of the 14-MCU segments, `Channel` (8-row strips, missing blocks stay black, never invented), auto-level (WEFAX's 2nd–98th percentile, `auto_level.h`), thermal channels inverted (cloud white). |
| `vibe_lrpt_run.h` | the DecoderHost's `lrpt` extension: IQ blocks → Receiver, the preview frames, `lrpt_status`, the pass's PNGs to the gallery. |
| `vibe_lrpt_tx.h` | **our own LRPT transmitter** (tests and the benchmark row): pictures → forward DCT/quantise/Huffman-encode → packets → VCDUs → RS encode → PN → convolutional code → [diff] → [80k interleave + markers] → RRC-shaped QPSK/OQPSK with carrier, Doppler slope, clock error, noise and any of the eight ambiguities. |

**AUTO.** Plain vs differential coding: both searched on the same soft stream; the first to pass RS wins. QPSK vs OQPSK:
**two demodulators side by side** until one has 12 good frames; the one with more stays (an OQPSK demodulator on the 2018
QPSK recording still passes RS on 55 % of frames — "first to decode wins" kept it and lost half the picture). With no
symbol rate named (what the clients send until the satellite engine names it), groups are walked while nothing decodes:
72k OQPSK (N2-3 / N2-4's usual) → 80k OQPSK interleaved (24 s dwell: its deinterleaver alone takes 16 s) → 72k QPSK (N2,
silent since 2022). VCDUs are delivered once, whichever chain got them first (counter dedupe). Each can be pinned
(`"symbol_rate"`, `"modulation"`, `"diff"`, `"interleaved"`) — the manual fallback; no client draws those keys yet.

**0 false images.** The ASM search is eager on purpose, the Viterbi always outputs something, but nothing reaches the
picture unless all four RS(255,223) words of the frame pass (a random frame: ≈ 10⁻⁴⁰), then the packet checks (version,
length, APID 64–69, MCU a multiple of 14 below 196, quality 1–100, the time on the 1232 ms grid).

**Memory** (256 MB boards): a channel ≤ 800 strips (6400 rows, ≈ 16 min) = 10 MB; ≤ 6 channels (METEOR sends 3 → ≈ 30 MB
for a long pass); the 80k deinterleaver 2.6 MB only while that group runs; the IQ queue is the host's (2 s, drop-oldest).
Real 105 s decode: 38 MB maximum RSS including the test's WAV buffers.

## Interface (for the satellite engine, branch `v12-sat`)

- **Input:** complex baseband (interleaved float I/Q) at **any rate from 1.8× to 8× the symbol rate** (144–600 kS/s;
  150–300 kS/s is what the host's slice gives), centred on the published downlink. On a server it is the DecoderHost's IQ
  slice of the listener's dial: `iqWidthHz()` asks for 140 kHz (115 kHz signal + Doppler), the shim's per-listener
  extract (`feedSkimFromBins`) cuts it (150 kS/s at 1.2 MS/s, 300 kS/s at 2.4 MS/s).
- **Doppler — either:** (a) the engine moves the listener's dial with the pass (the slice follows; nothing else needed),
  **or** (b) it leaves the dial on the published frequency and fills `DecoderHost::Env::satDopplerHz` — *the shift to
  remove now, Hz, positive = received above nominal* — read once per IQ block (≈ 20 a second) and fed forward to the
  carrier loop (`Receiver::setDopplerHz`). **With neither**, acquisition + the loop follow ±3.1 kHz / 65 Hz/s unaided
  (test: −2.8 kHz with a +65 Hz/s slope and 80 ppm clock decodes every block); the engine only makes lock faster.
- **Wired to the engine (2026-11-19, after merging `v12-sat`):** with no `Env::satDopplerHz` override, the host reads
  `vibe::sat::decoderView(session)` once per IQ block and feeds `modelHz` (received − nominal) forward when a lock is
  on (TRACK / COAST). The slice is cut at the DIAL (the nominal) off the shared FFT, not from the channel the lock's NCO
  moves, so this is not a double correction — `inChannelHz` is never added. No lock = 0 (self-acquisition as before).
- **Per-satellite parameters** (sats.json): `"symbol_rate"` 72000 / 80000 in the attach message; absent = AUTO (above).
  The 80k mode implies interleaved. Modulation and coding are found by themselves.
- **Not built here:** pass prediction, scheduling, the capture-and-deferred decode / client-assisted stage
  (DECODERS-GALLERY.md §3, §5). `Receiver::feedSoft` takes archived soft symbols for that later stage.

## Wired

- **Server:** DecoderHost extension `lrpt` (a decoder slot, decode thread, IQ via the skimmers' path); `{"type":
  "lrpt_status","data":{carrier, sync, mode, diff, symbolRate, lock, offsetHz, frames, bad, packets, lines, channels[]}}`
  once a second; the preview as SSTV's frames (0x07 392×1600 · 0x02 "METEOR LRPT" · 0x01 RGB rows, each row once, in
  order · 0x05 at the pass's end) — so a joiner is caught up by the host's image replay. Gallery: `LrptGalleryTap`
  (`DecoderTap::picture`), kind `lrpt`, one PNG per live channel + the composite, titled "METEOR LRPT · RGB 221 false
  colour · 1568×680"; filed when the pass ends (30 s without a frame) or, marked partial, when the decoder stops.
  Reach: `vibe_decoder_reach.h` ("is on 137.100 and 137.900 MHz"); `vibe_radio_caps.h` (never an Icom: no IQ); hwinfo
  `lrpt` / `lrptWhy` (absent on an Icom); `modes` list; the owner switch on the setup page and ServerModeScreen;
  `decoderBlocked`; the "who lost input" label.
- **Benchmark row** `dec_lrpt` (slot, primary for `lrpt`; `Need::Lrpt`: a radio reaching 137.1 or 137.9 MHz, not an
  Icom): one satellite at 150 kS/s through the host's IQ path. **Not run here** (the full benchmark); `test-bench-decoders`
  will exercise it in the coordinator's run.
- **Web page:** `data-dec="lrpt"` key (VHF · other), greyed with its reason off 137 MHz, the image box, status line, CLR,
  the gallery's METEOR filter. **App:** DecoderClient `lrpt`, the ModeSelector key, DecoderPanel's picture box, the
  capability from hwinfo, ServerModeScreen switch, the picture glyph. **R36S:** the METEOR key (VHF · SATELLITE), the
  picture box (SSTV path; `kSstvMaxDim` 1024 → 2048 for a whole pass), the status line — `-fsyntax-only` clean in
  `vibe-hh-map:trixie`; **not run on the handheld**, no canned feed, no golden.
- **Copy:** About V12 note (says plainly what is tested on real signals and what is not). `sdrTour` is generic (no change);
  `website/index.html` lists no decoders (no change).

## Results

**Real recording** — `~/VibeSDR-test-audio/lrpt/sigidwiki/LRPT_16-29-02_137900kHz.wav` (METEOR-M N2, 137.9 MHz, 72k
QPSK, 187.5 kS/s 16-bit, 105 s, 2018; from the sigidwiki LRPT page's I/Q link, **no licence stated → test input only,
not in git**). `test-lrpt --wav <file> <dir> --rate-auto` (what the clients send):

| | |
|---|---|
| frames | 922 found, **918 pass RS** (4 lost: one partial strip) |
| channels | APID 64 (1 visible), 65 (2 near-IR), 68 (5 thermal) — 85 strips, 1568 × 680 |
| picture | recognisable: NL / IJsselmeer / Wadden / DK coast, cloud bands; RGB 221 composite and the three channels, `decoded/` beside it |
| margin | + complex noise of 0.5× / 1× / 1.4× / 2× the recording's RMS: 918 / 910 / 867 / 665 frames |
| CPU | 0.45–0.5 % of one M5 core (≈ 80 M instructions a second, AArch64: demod ≈ 55 %, Viterbi ≈ 25 %, RS ≈ 8 %) |

**Synthetic** (`test-lrpt` 52 / 0, ≈ 8 s): RS 0–16 errors corrected 310 / 310, 17–21 never miscorrected; Viterbi 0 bit
errors to Eb/N0 3 dB, SIMD == scalar decisions; every ambiguity; QPSK 150 / 187.5 / 300 kS/s; −2.8 kHz + 65 Hz/s + 80 ppm;
OQPSK + differential; 80k OQPSK interleaved + differential; AUTO and rate AUTO on each; PSNR 52 dB against the source.
Sensitivity (QPSK, 6 scan periods, 4–6 seeds): Es/N0 4 dB 100 %, **3 dB ≈ 95 %** (6 of 6 seeds), 2.5 dB ≈ 80 %, 2 dB
≈ 50 %, 1.5 dB ≈ 0. **Noise: 0 pictures** — 60 s each of AUTO 72k, OQPSK and 80k, and 15 s of random-bit QPSK (a locked
carrier with no LRPT in it): RS ok 0. SIMD vs scalar (FIR + Viterbi) at Es/N0 5: the same picture byte for byte.
`test-lrpt-host` 33 / 0 (host, preview shapes, status, gallery Store with real PNGs, pass end, noise, reach, caps).
`scripts/test_lrpt_app.ts` 16 / 0. Unchanged: `test-decoder-reach` 76 / 0, `test-radio-caps` 247 / 0. Cross-built and run
under qemu user mode (`ft8perf-x86` image; armhf built in `vibeserver-cross:bookworm-armhf`, GCC 12): x86-64 SSE2,
i686 SSE2, armhf NEON (`-march=armv7-a -mfpu=neon-vfpv4`) and armhf `VIBE_FORCE_SCALAR` — `test-lrpt --quick` **51 / 0 on
each**, the SIMD and scalar pictures identical on each (Viterbi kernel 2 / 2 / 1 / 0). The CPU figures under emulation
are not a measure of those machines.

**Cost estimate for small boards** (not measured on them): AArch64 instruction count on the M5 ≈ 80 M/s at 187.5 kS/s
(≈ 105 M/s at a 300 kS/s slice). At the Pi 2's measured NEON-kernel IPC (0.35–0.48, `PI2-REAL-CPU.md`), allowing ≈ 1.2×
more A32 instructions: **≈ 22–30 % of a Pi 2 core** at 187.5 kS/s, ≈ 30–40 % at 300 kS/s; **≈ 2 % of a Pi 500 core**.
AUTO's contest doubles the demodulator (≈ +55 %) for the first ~12 frames only.

### Dead ends (in the history)

- Strips numbered by each channel's own first packet — channel 2 sat one strip below channel 1 (fixed: the shared time).
- Elapsed time ÷ 1232 ms — drifted off the grid within a minute (1228 ms steps); now counted from the last period.
- Holding acquisition on ASM lock — an ASM is "found" with the carrier 400 Hz out at 3 dB; 2 of 5 seeds never recovered.
- "First modulation to decode wins" — OQPSK on a QPSK signal half-decodes; replaced by the contest.
- A per-sample FIR + cubic interpolator — 392 instructions a sample; replaced by the polyphase filter at the points.

## RESUME HERE

0. **beta91: a real Meteor-M2-4 pass was captured and replayed — see `LRPT-LIVE-PASS.md`** (OQPSK + differential at
   72k confirmed on air; the receiver's sensitivity, relock, rate choice and pass end were fixed against it). Item 1 below
   is done for 72k OQPSK; the 80k mode is still untested on air.
1. **A real N2-3 / N2-4 pass** (OQPSK, differential; and the 80k mode if one of them is in it). Needs a 137 MHz aerial and
   a radio with 0 listeners — **the coordinator's capture**: 150–250 kS/s complex around 137.1 or 137.9 MHz for a whole
   pass (check sats.json / SatNOGS for which is live), then `test-lrpt --wav` (16-bit stereo WAV) or add a `.cu8` reader.
   If sync is found but RS never passes: the differential code's convention (per stream vs across I/Q) is the first
   suspect, then the OQPSK pairing; if RS passes but the picture is wrong: the 80k interleave order.
2. Run `test-bench-decoders` (the coordinator's run-tests) for the `dec_lrpt` row; then a Pi 2 / Pi 500 figure by the
   qemu instruction-count method (`AVIATION.md §Cost`: the `ft8perf-x86` image has qemu-arm but the TCG count plugin was
   not found in it on 2026-11-19).
3. The satellite engine: pass `symbol_rate` from sats.json in the attach, and either move the dial or fill
   `Env::satDopplerHz`; a pass list (max elevation) in the LRPT box.
4. The manual fallback keys (QPSK / OQPSK / 80k, differential on/off) under the LRPT key in the three clients (the server
   takes them; no client draws them yet). The R36S: a canned feed + golden, a run on the handheld, an LRPT gallery tab.
5. The deferred / resumable decode and client-assisted stage (DECODERS-GALLERY.md §3.2, §5) — `feedSoft` is the hook.
6. Further CPU: the demod's per-sample path (≈ 40 instructions) and the Viterbi's mask loads; a 2× decimation before the
   matched filter at 300 kS/s slices.

### Decisions for Stuart

- The composite is **RGB 221** (R = G = channel 2, B = channel 1) — the familiar METEOR false colour; others (123, 125)
  not offered.
- The live preview is **quarter size** (392 px) so a 1 GB phone's canvas stays ≈ 2.5 MB; the gallery keeps full size.
- A pass is filed **30 s after the last frame**; stopping the decoder files what it has, marked partial.
- AUTO order for an unnamed rate: 72k OQPSK → 80k OQPSK → 72k QPSK (N2 is silent; it is still tried).
- The 2018 test recording has no stated licence: kept locally under `~/VibeSDR-test-audio/lrpt/`, never in git.
