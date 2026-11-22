# FT8 / FT4 / FT2 / JS8 sensitivity (V12, 2026-10-26)

Stuart, 2026-10-27: *"get our digital modes decoder to be world class"* — then: *"I want to beat the competition on CPU
usage whilst remaining comparable if not better on decodes."*

Branch `v12-ftx-sensitivity` (from `v12-experimental`). Everything here is written from the published protocol
(K1JT, K9AN, G4WJS, *"The FT4 and FT8 Communication Protocols"*, QEX 2020) and textbook detection theory; the
reference decoder was **run as a black box** (Debian's packaged `jt9`, on the same WAVs) and never read for code.

## What was built

| Part | Where | What it does |
|---|---|---|
| The second look | `decoders/ftx_fine.{h,cpp}` | A candidate the waterfall's likelihoods could not decode is measured again from the audio (below). |
| A-priori book | `FtxApBook` in `decoders/ft8_decoder.*` | What this dial heard without assumptions in the last 10 minutes: calls, pairs working each other. |
| AP decoding | `Ft8SlotDecoder::apDecode` | Hypotheses from the book, screened, decoded on near misses only, every decode guarded. |
| OSD pairs | `ft8Osd(llr, maxErrors, pairSpan, …)` | OSD-1 plus every pair of flips among the 40 least reliable independent bits. |
| Guards | `ft8OsdPlausible`, `apCheck`, the all-zero refusal, the unpack check | No decode that is not a message people send. |
| Cheaper passes | `onlyChanged`, `bp_decode_ex` (stall exit) | A later pass skips what nothing changed near; BP stops when it stops improving. |
| Subtraction | `Ft8SlotDecoder::subtract` | The amplitude box is ±½ symbol for decodes at −5 dB and stronger (±1 below). |
| FT4 time search | `kFt4TimeMin/Max` | DT −1.65 … +1.23 s (it was −0.98 … +0.46: a late station was never a candidate). |
| JS8 | `Js8SlotDecoder` + `FtxFrame` | The same second look on JS8's frame, in place of its coherent stage. |
| Bench | `tools/ft8-bench/ftx_sweep`, `ft8_wav` options, `tools/ft8-bench/ref/` | Synthetic bands with known answers; real recordings scored against UberSDR and against jt9 on the same WAVs. |
| Tests | `vibeserver/test-ftx-sensitivity.cpp` (registered), `test-js8` | Kernels unchanged, guards, the measurement, sensitivity, the book, noise with every candidate tried. |

### The second look (`FtxFine`)

ft8_lib finds candidates on a waterfall — a Hann-windowed STFT two symbols long, half a symbol and half a tone apart,
stored as uint8 in half-dB steps — and decodes from the same numbers: a bit's likelihood is a difference of dB levels.
That is a good search and a poor detector. The textbook detector for continuous-phase FSK is the matched filter, and
because the tone spacing equals the symbol rate every tone ends a symbol on a whole cycle, so the phase carries from
one symbol to the next and symbols can be detected **together** (multiple-symbol noncoherent detection).

1. **One real FFT of the window per pass** (only when the first candidate needs it). A candidate's baseband is cut
   from that spectrum (raised-cosine band from 1.5 tones below tone 0 to 1.5 above the top tone) and brought back by
   an inverse FFT 60× shorter (FT8): an ideal band-pass and decimator in one.
2. **Fine sync** on the Costas symbols: start in 1/32 symbol (FT4/FT2 1/16), frequency in 1/16 tone.
3. **The matched filter**: every symbol's complex amplitude at every tone, phase-referenced to the slot.
4. **Early out**: fewer than 10 Costas symbols that are their own strongest tone (of 21 FT8, 16 FT4) = noise, stop.
5. **Residual frequency and timing from the phase**: between consecutive Costas symbols the correlation turns by
   2π·δf·T plus ψ per tone step (a start a fraction of a sample off, and GFSK's transitions). Both are found and taken
   out of every symbol — left in, ψ (3-17° per tone step on a clean FT4 signal) costs what step 6 is for.
6. **Likelihoods** over groups of 1, 2 and 3 symbols (FT4/FT2: 1, 2, 4), max-log over |Σ group|; BP tries the two-,
   three- and one-symbol sets in turn; OSD (with pairs) only after BP came within 12 unsatisfied checks.

### A-priori decoding for a receiver

The paper's AP decoding gives the decoder message bits it can expect. A transceiver expects its own call; a receiver
has none, so the book supplies: `CQ ? ?`; `X ? ?` and `? X ?` for each call heard; `X Y ?` both ways for each pair
heard working; `X Y RR73/73/RRR`. Hundreds of hypotheses on a busy band are scored by **region sums** (each call
once as first and once as second call; pairs and sign-offs are additions) — cheaper than one BP. The best three are
decoded (bits clamped to 1.01 × max |L|): BP, then OSD only at screening z ≥ 4. A sign-off fixes every bit, so it is
**detected**, not decoded: its codeword is built and must agree on all but 20 of its 97 other bits (a wrong
codeword disagrees in ~48: P ≈ 1e-8) with whole-codeword z ≥ 5. AP runs only on near misses (BP within 19).
Guards on every AP decode: CRC-14; the assumed bits survive; a standard message that unpacks; the unassumed call a
standard callsign; the last field a real grid/report/RRR/RR73/73; ≤ 30 % hard disagreements on the bits not
assumed. A decode made with AP never feeds the book.

### The live decoder (`Ft8Decoder::runDecode`)

| | Loaded server | Spare CPU |
|---|---|---|
| Passes | 1 | up to 3, through the existing gate (40 % of the time left) |
| Second look | yes, BP only | yes, BP + OSD (pairs) |
| A-priori | the book learns, no AP | yes |
| Waterfall BP | stall 3 | stall 3 |

One pass of the second look costs a third of the old single pass and decodes more than the old full depth, so it runs
everywhere. The benchmark's Band row (`maxPasses = 1`) therefore measures the cheaper loaded path.

## Results

### Synthetic band (known answers, 200 signals an SNR, non-overlapping, true SNR in 2500 Hz)

50 % decode point, dB (lower is better). jt9 decoded the **same WAVs** (`ftx_sweep --wavdir`).

| | FT8 | FT4 | FT2 |
|---|---|---|---|
| old single pass | −18.7 | −14.2 | −11.1 |
| old full depth (3 passes + OSD-1) | −18.8 | −14.3 | −11.2 |
| **new, loaded (1 pass)** | −20.7 | −16.1 | −13.2 |
| **new full depth** | **−20.8** | **−16.3** | **−13.3** |
| new full depth, calls heard before (AP) | −21.6 | −17.1 | −14.0 |
| jt9 -d 2 (WSJT-X 2.6.1, Debian) | −20.7 | −16.9 | — |
| jt9 -d 3 | −20.9 | −17.5 | — |

The FT4 "old" rows already include the wider time search. Gains, full depth: FT8 +2.0 dB, FT4 +2.0, FT2 +2.1 (AP for
heard calls a further 0.7-0.8). CPU on this band (M5, ms a slot at the threshold SNRs): FT8 3.4 → 3.9, FT4 7.5 → 4.9,
FT2 4.5 → 3.1.

JS8 (test-js8, 8 signals an SNR, 2 passes + OSD): Turbo −16 dB 0/8 → 3/8, Fast −18 dB 2/8 → 7/8, Normal −20 dB 4/8 →
8/8, Slow −24 dB 0/4 → 1/4, at ~35 % less CPU than the coherent stage it replaces.

### False decodes

Pure noise, the live full depth with a 400-call AP book, and a **stress** run (every candidate down to sync score 0,
300 a pass, second look down to 6 Costas hits, AP on every candidate):

| | slots (time) | live | stress |
|---|---|---|---|
| FT8 | 480 (2 h) | 0 | 0 |
| FT4 | 960 (2 h) | 0 | 0 |
| FT2 | 1,920 (2 h) | 0 | 0 (1 before `ft8OsdPlausible`: "1/Y2ESZU4Y3 <...> 73") |
| JS8, four submodes | 100 / 100 / 100 / 40 | 0 (102,000 candidate trials) | |

Real audio: every decode UberSDR did not report was checked against the calls UberSDR or PSKReporter (STUEY3D) heard
in the same capture (`tools/ft8-bench/ref/corrob.py`). Of the 409 such decodes on the eleven recordings, 400 carry only
corroborated calls. The other nine: eight of a +20 dB station (OH6VC with two JA calls) that plain BP decoded — the old
decoder too — before the reference's stream had connected, and `BH3WBQ UN3QA MN83` (a Kazakh call with a Kazakh
grid). JS8: one uncorroborated frame at −26 dB (`N3DNA: K4EXA HEARTBEAT SNR -24`).

### Real recordings, same audio (Stuart's UberSDR, RX888)

Totals over seven FT8 recordings (359 slots, 20/40/80 m, quiet to very busy) and four FT4 (348 slots).
"calls" = distinct senders; "grid" = those with a locator (what reaches the map). CPU: one M5 core, ms per slot.

| FT8 | decodes | matched UberSDR | calls | calls w/ grid | ms/slot |
|---|---|---|---|---|---|
| UberSDR stream (STUEY3D) | 9,516 | — | 1,057 | 857 | — |
| old single pass | 5,358 | 5,271 | 749 | 587 | 10.0 |
| old full depth | 6,947 | 6,811 | 880 | 701 | 16.2 |
| **new, loaded (1 pass)** | 6,122 | 6,029 | 842 | 662 | **3.0** |
| **new, 2 passes** | 8,573 | 8,263 | 995 | 794 | 13.4 |
| **new full depth** | **8,938** | **8,567** | **1,008** | **813** | **15.8** |
| jt9 -d 2 | 9,560 | 8,876 | 1,032 | 837 | 955 |
| jt9 -d 3 | 10,059 | 9,182 | 1,063 | 870 | 2,243 |

| FT4 | decodes | matched | calls | grid | ms/slot |
|---|---|---|---|---|---|
| UberSDR stream | 1,064 | — | 77 | 63 | — |
| old full depth | 865 (103 were empty OSD codewords) | 762 | 57 | 48 | 4.1 |
| **new full depth** | **970** | **932** | **68** | **55** | **3.6** |
| jt9 -d 2 | 970 | 963 | 72 | 61 | 43 |
| jt9 -d 3 | 1,039 | 1,021 | 74 | 62 | 136 |

Per recording: [the full table](#per-recording). The busy 40 m quarter-hour (2,782 UberSDR decodes, 290 calls):
old full depth 1,973 matched / 258 calls → new 2,474 / 277; jt9 -d 3 2,649 / 292.

FT2 (20 m, 10 min): the same 15 decodes (both senders STUEY3D reported), 2.7 → 1.8 ms/slot.
JS8 (40 m hour): 28 → 31 frames, every one of STUEY3D's 9 senders.

### CPU on the Pi 2 build (armhf, qemu instruction counter, 10 slots)

| 40 m | busy (16:20 UTC, ~45 stations a slot) | moderate (21:19 UTC) |
|---|---|---|
| old single pass | 3,335 M — 224 decodes | 3,428 M — 126 |
| old full depth | 18,699 M — 315 | 12,388 M — 148 |
| **new, loaded** | **1,222 M — 252** | **1,174 M — 153** |
| new, 2 passes | 5,510 M — 375 | 3,571 M — 202 |
| new full depth | 7,473 M — 400 | 4,291 M — 205 |
| jt9 -d 2 (Debian armhf) | 356,883 M — 427 | |
| jt9 -d 3 | 683,881 M — 452 | |

On the busy slots the new full depth uses **1/48 of jt9 -d 2's instructions and 1/92 of -d 3's** for 94 % / 88 % of
their decodes. An instruction count is not a cycle count (an A7 takes two cycles for a 128-bit NEON op); it counts the work.
**Against the old single pass:** the loaded path is 0.35×; full depth is 1.25× on a moderate band and 2.2× on the
busiest (the subtraction of ~27 decodes a slot, ~7 M instructions each, is most of the difference). Full depth is
2.5-3× cheaper than the old full depth everywhere. On a Pi 2 the pass gate grants passes 2-3 only while they fit in
40 % of the time before the next slot, as before.

### What each technique costs and gains (FT8 unless said)

| Technique | Gain | Cost |
|---|---|---|
| Second look (fine sync, matched filter, 1/2/3-symbol sets) | +1.8 dB synthetic; real +17-25 % decodes | 15-40 µs a candidate that fails waterfall BP (M5; the early out is the cheap end); one window FFT a pass |
| ψ (residual timing) correction | +0.3 dB FT8, +0.6 dB FT4, +0.4 dB FT2; real +2.3 % | negligible (41 × 18 complex products a candidate) |
| OSD pairs (span 40) | real +0.6 % | +4 % CPU |
| AP (book, region screening, sign-off detection) | +0.5-0.9 dB for heard calls; real +3.6 % (8,272 → 8,567 matched) | +15 % CPU |
| onlyChanged | −0.1 % decodes | −12 % CPU at full depth |
| BP stall exit (waterfall 3, second look 5) | −0.1 % | waterfall BP 1.4 → 0.6 ms a pass (80 m) |
| AP OSD only at z ≥ 4; sign-offs detected | −0.5 % | −17 % CPU |
| Subtraction box ±½ symbol at ≥ −5 dB | busy 40 m +2 %, busy 20 m +2 % | none |
| FT4 wider time search | synthetic plateau 60 % → 100 % for late stations | search ×2 (0.04 → 0.08 ms a pass) |

## RAM

| | FT8 | FT4 | FT2 |
|---|---|---|---|
| Second look (window FFT + work + baseband + tables), measured `FtxFine::ramBytes()` | 1.56 MB | 0.79 MB | 0.57 MB |
| AP book (≤ 400 calls, ≤ 200 pairs) | ~25 KB | ~25 KB | ~25 KB |

JS8 makes one per submode: Normal 1.56 MB, Fast ~1.0, Turbo ~0.6, Slow ~3.1 — about 6 MB for all four, on top of its
4.5 MB. The trade: the window's spectrum is what makes each candidate's baseband one small inverse FFT instead of a
filter over 150,000 samples — and it is allocated on the first candidate that needs it.
Band mode runs one engine per dial: FT8 + FT4 + FT2 on 20 m adds about 2.9 MB.

## What remains

- ★ V12 beta91: the deep tier (above) closes part of this — on a busy 40 m evening +5 % matched decodes; what is left is
  mostly overlapping signals, not weak ones.
- **The last 3-5 % of calls.** jt9 -d 3 still finds ~7 % more decodes and ~5 % more calls, at 140× the CPU.
  On the misses: most are near misses BP could not finish; more candidates (`minScore` 7) gave +0.8 % for +37 % CPU
  and was not taken.
- **FT4 is 0.6-1.2 dB behind jt9 on the synthetic band** (FT8 is level). FT4's GFSK (BT 1) spreads each symbol into
  its neighbours; a matched filter for the whole waveform of a symbol group, not a pure tone per symbol, is the next
  thing to try.
- **The subtraction is most of a busy band's Pi 2 cost** (~7 M instructions a decode). Ideas measured and not taken:
  subtracting only decodes an undecoded candidate overlaps (−5 % decodes on busy bands), an interleaved sine table
  (more instructions). A block-linear phase reference would roughly halve its largest loop.
- **JS8's AP** (its own message format) is not done; JS8 uses the second look and OSD only.
- **FT2's SNR** is checked against only two senders (STUEY3D's PSKReporter reports, 07:34 UTC; the 17:59 recording
  had none we both decoded): median −1 dB, so its −6 dB stands until a busier FT2 hour is recorded.

## Deep tier — "dig deeper" (V12 beta91, 2026-11-22)

Stuart, 2026-11-22: *"yes please as it may benefit the lenovo and the pi500 too"*. Branch `b91-ft8deep`. Own code from the
protocol paper's AP idea; WSJT-X was not read for this work.

### RESUME HERE

- ★★★ **AUTOMATIC (Stuart, 2026-11-22: "should be automatic and simply enable when the cpu has headroom with a benchmark
  driven toggle in the settings with the rest of the digital modes, no user facing toggles").** The rule is one header,
  `decoders/ft8_deep_auto.h`, read by the shim (`vsFt8DeepApply`, every minute and on every mode-list change), the setup
  page (`ft8DeepAuto`) and the app (`ft8DeepAutoFrom`):
  1. the owner's word wins: `ft8+deep` (on) / `ft8-deep` (off) in the radio's mode list — ticking or unticking the box under
     Digital spots writes one; "Back to automatic" removes both;
  2. else the stored benchmark: its `dec_ft8deep` row below 25 % of a core; a beta90-or-older benchmark (no such row): its
     Band row × 1.25 below 25 % (decided at start-up, no re-run needed); a Band row listed as not run = off;
  3. no benchmark at all: the shim measures `benchdec::ft8DeepRow` ONCE itself — its own idle thread, three minutes after
     start, only while the process is under 40 % of a core (that baseline is taken off the figure) — and keeps the figure
     in `bookmarks-<radio>.ft8deep.txt` beside the radio's bookmarks. Off until then.
  The benchmark never writes the owner's word (it only shows the automatic answer). Listeners see nothing
  (scripts/test-ft8-deep-owner-only.mjs); the tier still runs only on slots with time to spare.
- **Built and wired:** decoder (`Ft8Decoder::deepTier()`, `FtxApBook::Rep`, `Ft8SlotDecoder::repeatDetect`), the owner's
  box (setup page and Android ServerModeScreen, under Digital spots, with its hint and the automatic answer), the shim's
  decision and idle probe, the benchmark row `dec_ft8deep` (`benchdec::ft8DeepRow`) with its own green line
  (`benchdec::kDeepGreenPct` = `ft8deep::kGreenPct` = 25 %, `gradeFor`), the beta91 About line.
- **Tests:** test-ft8-deep-auto (fresh / upgraded / current benchmark / owner override), test-ft8-deep-owner-only.mjs,
  test-ftx-sensitivity § 8, test-bench-decoders (the row).
- **Not done:** the R36S's native UI has no owner decoder switches, so nothing to add there; JS8 has no deep tier.
  A web client listener sees nothing new (the tier is server-side).
- **To check live (coordinator):** after upgrading, the Lenovo / Pi 500 log "FT8 dig deeper: on (from the benchmark)" from
  their existing benchmarks' Band rows; the Pi 2 "off". Compare an evening's PSK Reporter spot count off and on (the owner's
  box) to confirm the gain live.

### What it adds (on top of full depth, on spare CPU only)

| Lever | What | FT8 | FT4 / FT2 |
|---|---|---|---|
| **Repeats** (new) | Every standard message heard without AP in the last minute (FT8 4 slots, FT4 8, FT2 16; ≤ 256) is kept with its frequency and whole 174-bit codeword. A candidate within ±2 tones of one is held against it — z = Σ±L/√ΣL² ≥ 5 over all 174 bits and ≤ 20 of the 97 CRC+parity bits disagreeing (the sign-off test, P ≈ 1e-8 for a wrong codeword) — before the AP hypotheses. No BP: a detection, not a decode. | yes | yes |
| AP hypotheses decoded | 8 a near miss (full depth: 3) | yes | yes |
| Second-look gate | 8 Costas hits of 21 (full depth: 10) | yes | no (8 of 16 doubled FT4's CPU for +1 %) |

The tier runs only where full depth runs (not loaded, the pass gate free): a loaded slot decodes exactly as before.
The repeat book learns whenever the switch is on, so it is ready when the CPU is. A repeat detection never feeds the book.

### Results — same audio, before and after

Real recordings, Stuart's UberSDR (RX888), "matched" = message for message against the UberSDR decoder stream recorded
beside the WAV (`score_ref.py`); CPU one M5 core.

| Recording | full depth: matched / calls / ms a slot | **dig deeper** | gain | not in the reference |
|---|---|---|---|---|
| FT8 40 m 22 Nov 18:06 (61 slots, ref 3,598) | 2,705 / 299 / 28.0 | **2,852 / 302 / 33.8** | **+5.4 %** | 37 → 38 |
| FT8 80 m 22 Nov 18:06 (61 slots, ref 2,561) | 2,058 / 213 / 21.1 | **2,139 / 215 / 22.1** | **+3.9 %** | 26 → 29 |
| FT4 40 m 22 Nov 18:16 (123 slots, ref 837) | 784 / 54 / 5.3 | **799 / 55 / 5.8** | **+1.9 %** | 31 → 44 |
| FT8 40 m 5 Oct 19:08 (239 slots, no reference) | 10,713 decodes / 612 senders / 24.2 | **11,243 / 619 / 28.1** | **+4.9 %** | — |

Repeat detections were 250 of the 40 m run's 2,890 decodes (some replace what AP found before). FT4's extra
not-in-the-reference decodes were inspected: stations calling on their own cadence at their own frequency (EW8DU calling
DL1KMD every second slot, two stations on DL1KMD's frequency at once) — consistent with real repeats the reference could
not separate, but not provable without a second reference. The 5 Oct hour's 530 new decodes: every one carries calls the
hour heard, bar three with plausible grids for their calls.

Synthetic (`ftx_sweep`, 200 signals an SNR, each message heard a minute before at its frequency): the 50 % point moves
FT8 −21.0 → −22.1 dB, FT4 −16.3 → −17.3, FT2 −13.1 → −14.2. Without anything heard before, the tier changes little
(AP hypotheses and the gate only).

### False decodes

- Noise, a full book (400 calls, half in pairs, + 256 repeats at random frequencies), every candidate down to sync score
  0, 300 a pass, second look down to 6 Costas hits, AP and repeats on every candidate: FT8 480 slots 0, FT4 960 slots 0,
  FT2 1,920 slots 0 (as live: no OSD for FT2).
- **Siblings** — the worst case for a repeat: every signal's sibling (the same calls, the last field one step away: report
  ±1 dB, the next grid square, RR73 ↔ 73) put in the book at its frequency, repeats tried on every candidate however far
  BP was, FT8 −24 … −10 dB: 0 siblings claimed; FT4, FT2 the same (test-ftx-sensitivity § 8).
- The one false decode of all the stress runs came from a lever NOT taken: OSD pairs over 60 bits, FT2
  ("<...> 5NDK/V66VMD").

### Measured and not taken

| Lever | Gain (real 40 m) | Cost |
|---|---|---|
| OSD pairs over 60 bits | +0.4 % | +2 ms a slot; the stress runs' only false decode |
| A fourth pass | +0.9 % (40 m), +0.4 % (80 m) | +15 %, +29 % CPU |
| minScore 7 | +0.3 % | +15 % CPU; a hex telemetry decode nobody else heard |
| 200 / 250 candidates, AP near-miss 26 / 30, repeats tried to BP 60, the gate at 7 | ≤ +0.3 % each | up to +20 % |
| FT4/FT2 second look at 8 of 16 | +1 % | ×2 FT4 CPU |

What is left of the gap on a busy 40 m evening (746 reference decodes we did not match) is mostly NOT weak: 300+ of
them at a reference SNR of −15 dB or better, concentrated at 1000-1500 Hz where the band is densest — overlapping
signals, the subtraction's territory, not the detector's. 211 were repeats of a message we also missed the first time.

### AUTO: which boxes get it (automatic; the owner can override)

The row `dec_ft8deep` decodes the Band row's busiest band (20 m: every FT8/FT4 dial, 15 strong + 15 weak stations a
dial, two slots so repeats exist) with no load and the tier on. **On below 25 % of one core** (not the usual 70: the
tier rides on top of everything else the box carries). Not run where Band at one pass already measured below green. An
older benchmark without the row decides from its Band row × 1.25; none at all, the server measures the row itself once.

| Box | the row (estimated from the M5's) | default |
|---|---|---|
| M5 (measured) | 0.97 % | on |
| Lenovo i5-11300H | ~2 % (≈ 2× an M5 core) | **on** |
| Pi 500 (Cortex-A76 2.4 GHz) | ~3-4 % (≈ 3.5×) | **on** |
| Pi 4 (A72 1.5 GHz) | ~10 % | on |
| Pi 3 / Zero 2 W (A53) | ~25-30 % | off (borderline) |
| **Pi 2 (A7 900 MHz)** | ~50 % (≈ 50×: below) | **off** |

**The Pi 2 build** (armhf, qemu instruction counter, `tools/ft8-bench/qemu/run.sh`), the busy 40 m of 22 Nov, 10 slots:

| | instructions | decodes |
|---|---|---|
| one pass (a loaded server) | 1,415 M | 289 |
| full depth | 7,718 M | 467 |
| **dig deeper** | **9,204 M (+19 %)** | **485 (+3.9 %)** |

≈ 920 M instructions a slot: at a Cortex-A7's ~0.6 instructions a cycle, ~1.7 s of a 900 MHz core every 15 s for one FT8
dial — ~50× the M5's 33.8 ms, so the row reads ~50 % there: amber against its 25 % line, OFF.

In the live decoder the tier is also gated slot by slot exactly as full depth is (`passAllowed`: not loaded, passes inside
40 % of the time before the next slot), so even an owner who ticks it on a Pi 2 only gets it on slots with time to spare.

### Reproducing

```
OUT=/tmp/fb bash tools/ft8-bench/build.sh
F="--passes 3 --osd 24 --osd2 40 --fine 1 --stall 3 --changed 1 --ap 1"
/tmp/fb/ft8_wav REC.wav START_UTC $F [--deep] > ours.txt           # --deep = what runDecode adds (FT8's gate only for FT8)
python3 tools/ft8-bench/ref/score_ref.py FT8 skimmer_40m_FT8 uber_sse.txt START_UTC 930 ours.txt
/tmp/fb/ftx_sweep --snr -24:-18:1 --slots 20 --sigs 10 $F --deep --repseed 1        # heard before
/tmp/fb/ftx_sweep --snr -24:-10:2 --slots 20 --sigs 10 $F --deep --repseed 2 --repnear 99   # siblings: every decode of one is FALSE
/tmp/fb/ftx_sweep --noise 480 $F --aptable 400 --deep --reptable 256 --repnear 99 --apnear 99 --minscore 0 --cands 300 --finesync 6
```
The 22 Nov recordings are in `~/VibeSDR-test-audio/ft8/b91/` (WAV, start_utc in the .log, UberSDR stream beside them).

## Spot SNR scale (2026-10-26)

Stuart's rule: a spot's SNR reads on the scale other stations report. One constant per mode
(`Ft8SlotDecoder::kSnrCalFt8/Ft4/Ft2`), set from the median of ours minus STUEY3D's on the same messages in the same
slots (the UberSDR decoder stream recorded beside each WAV), checked in test-ftx-sensitivity section 7:

| | before | after | sample |
|---|---|---|---|
| FT8 | +2 dB (constant +3) | 0 / +1 (constant +1) | 6,701 decodes, seven recordings |
| FT4 | −6 dB (constant −6: +3 − 9) | 0 (constant 0) | 926 decodes, four recordings |
| FT2 | −1 dB (constant −6) | unchanged | 2 senders (PSKReporter) |

## Reproducing

```
OUT=/tmp/fb bash tools/ft8-bench/build.sh
/tmp/fb/ftx_sweep --snr -24:-16:1 --slots 20 --sigs 10 --passes 3 --osd 24 --osd2 40 --fine 1 --stall 3 --changed 1
/tmp/fb/ftx_sweep --noise 480 --passes 3 --osd 24 --osd2 40 --fine 1 --stall 3 --changed 1 --ap 1 --aptable 400
/tmp/fb/ft8_wav REC.wav START_UTC --passes 3 --osd 24 --osd2 40 --fine 1 --stall 3 --changed 1 --ap 1 > ours.txt
python3 tools/ft8-bench/ref/score_ref.py FT8 40m-ft8 uber_sse.txt START_UTC SECONDS ours.txt
tools/ft8-bench/ref/jt9.sh …        # jt9 as a black box on the same WAVs (Docker, Debian's wsjtx)
bash tools/ft8-bench/qemu/run.sh HEAD REC.wav START_UTC <options> --from A --to B   # Pi 2 instructions
```

## Per recording

Recordings in `~/VibeSDR-test-audio/ft8/` (UberSDR stream and STUEY3D PSKReporter XML beside them). "old" = the
decoder before this branch (`v12-experimental`); jt9 CPU from `/usr/bin/time` in Docker on the same M5 core.

| recording | decoder | decodes | matched UberSDR | calls | calls w/ grid | CPU ms/slot (M5, 1 core) |
|---|---|---|---|---|---|---|
| ft8 20m 2044 (25 slots) | UberSDR stream (STUEY3D) | 54 | — | 16 | 14 | — |
| | old 1 pass | 34 | 34 | 9 | 7 | 5.3 |
| | old full depth | 35 | 35 | 9 | 7 | 2.9 |
| | new, loaded (1 pass) | 40 | 40 | 11 | 8 | 1.4 |
| | new, 2 passes | 43 | 43 | 12 | 10 | 2.9 |
| | new full depth | 43 | 43 | 12 | 10 | 2.8 |
| | jt9 -d 2 | 50 | 47 | 16 | 13 | 175 |
| | jt9 -d 3 | 51 | 48 | 16 | 13 | 995 |
| ft8 40m 2044 (25 slots) | UberSDR stream (STUEY3D) | 666 | — | 124 | 96 | — |
| | old 1 pass | 360 | 350 | 73 | 57 | 11.4 |
| | old full depth | 455 | 444 | 88 | 69 | 15.2 |
| | new, loaded (1 pass) | 452 | 438 | 99 | 74 | 3.1 |
| | new, 2 passes | 649 | 608 | 116 | 91 | 12.6 |
| | new full depth | 667 | 622 | 117 | 92 | 15.0 |
| | jt9 -d 2 | 697 | 629 | 118 | 93 | 977 |
| | jt9 -d 3 | 727 | 648 | 121 | 95 | 2426 |
| ft8 80m 2044 (25 slots) | UberSDR stream (STUEY3D) | 877 | — | 140 | 117 | — |
| | old 1 pass | 496 | 468 | 101 | 77 | 10.7 |
| | old full depth | 666 | 631 | 123 | 97 | 18.7 |
| | new, loaded (1 pass) | 574 | 544 | 110 | 88 | 2.9 |
| | new, 2 passes | 803 | 760 | 132 | 109 | 14.4 |
| | new full depth | 835 | 788 | 135 | 112 | 17.5 |
| | jt9 -d 2 | 916 | 831 | 141 | 117 | 1083 |
| | jt9 -d 3 | 957 | 857 | 143 | 120 | 2463 |
| ft8 40m 2118 (61 slots) | UberSDR stream (STUEY3D) | 1349 | — | 132 | 102 | — |
| | old 1 pass | 794 | 773 | 90 | 72 | 10.6 |
| | old full depth | 954 | 932 | 99 | 78 | 13.0 |
| | new, loaded (1 pass) | 951 | 932 | 108 | 85 | 2.9 |
| | new, 2 passes | 1250 | 1189 | 123 | 96 | 11.2 |
| | new full depth | 1271 | 1204 | 124 | 97 | 12.9 |
| | jt9 -d 2 | 1375 | 1259 | 125 | 98 | 758 |
| | jt9 -d 3 | 1428 | 1289 | 131 | 104 | 2181 |
| ft8 80m 2118 (61 slots) | UberSDR stream (STUEY3D) | 2013 | — | 162 | 139 | — |
| | old 1 pass | 1197 | 1184 | 134 | 107 | 10.5 |
| | old full depth | 1531 | 1503 | 148 | 127 | 17.3 |
| | new, loaded (1 pass) | 1354 | 1341 | 144 | 117 | 2.9 |
| | new, 2 passes | 1840 | 1787 | 157 | 133 | 13.5 |
| | new full depth | 1905 | 1841 | 158 | 134 | 16.5 |
| | jt9 -d 2 | 2056 | 1926 | 163 | 140 | 1024 |
| | jt9 -d 3 | 2148 | 1981 | 165 | 141 | 2454 |
| ft8 20m 1619 (61 slots) | UberSDR stream (STUEY3D) | 1775 | — | 193 | 153 | — |
| | old 1 pass | 1063 | 1054 | 130 | 101 | 10.3 |
| | old full depth | 1315 | 1293 | 155 | 123 | 17.8 |
| | new, loaded (1 pass) | 1191 | 1181 | 146 | 115 | 3.1 |
| | new, 2 passes | 1607 | 1560 | 182 | 143 | 14.8 |
| | new full depth | 1652 | 1595 | 185 | 146 | 16.4 |
| | jt9 -d 2 | 1794 | 1656 | 187 | 148 | 979 |
| | jt9 -d 3 | 1885 | 1710 | 195 | 159 | 2088 |
| ft8 40m 1619 (61 slots) | UberSDR stream (STUEY3D) | 2782 | — | 290 | 236 | — |
| | old 1 pass | 1414 | 1408 | 212 | 166 | 9.8 |
| | old full depth | 1991 | 1973 | 258 | 200 | 21.6 |
| | new, loaded (1 pass) | 1560 | 1553 | 224 | 175 | 3.6 |
| | new, 2 passes | 2381 | 2316 | 273 | 212 | 18.1 |
| | new full depth | 2565 | 2474 | 277 | 222 | 22.5 |
| | jt9 -d 2 | 2672 | 2528 | 282 | 228 | 1316 |
| | jt9 -d 3 | 2863 | 2649 | 292 | 238 | 2593 |
| ft4 20m 2044 (51 slots) | UberSDR stream (STUEY3D) | 54 | — | 9 | 8 | — |
| | old 1 pass | 25 | 25 | 6 | 5 | 4.1 |
| | old full depth | 35 | 25 | 6 | 5 | 2.8 |
| | new, loaded (1 pass) | 39 | 39 | 7 | 6 | 1.7 |
| | new, 2 passes | 47 | 46 | 7 | 6 | 2.8 |
| | new full depth | 47 | 46 | 7 | 6 | 2.6 |
| | jt9 -d 2 | 27 | 27 | 7 | 6 | 26 |
| | jt9 -d 3 | 30 | 30 | 7 | 6 | 69 |
| ft4 40m 2044 (51 slots) | UberSDR stream (STUEY3D) | 57 | — | 4 | 4 | — |
| | old 1 pass | 44 | 44 | 3 | 3 | 4.4 |
| | old full depth | 50 | 44 | 3 | 3 | 3.3 |
| | new, loaded (1 pass) | 47 | 46 | 5 | 4 | 1.8 |
| | new, 2 passes | 52 | 49 | 5 | 4 | 3.2 |
| | new full depth | 52 | 49 | 5 | 4 | 2.9 |
| | jt9 -d 2 | 54 | 53 | 5 | 5 | 32 |
| | jt9 -d 3 | 58 | 57 | 5 | 5 | 99 |
| ft4 40m 2118 (123 slots) | UberSDR stream (STUEY3D) | 36 | — | 2 | 2 | — |
| | old 1 pass | 14 | 14 | 1 | 1 | 4.0 |
| | old full depth | 30 | 14 | 1 | 1 | 2.1 |
| | new, loaded (1 pass) | 18 | 18 | 2 | 2 | 1.9 |
| | new, 2 passes | 23 | 23 | 2 | 2 | 2.6 |
| | new full depth | 23 | 23 | 2 | 2 | 2.3 |
| | jt9 -d 2 | 31 | 31 | 2 | 2 | 22 |
| | jt9 -d 3 | 34 | 34 | 2 | 2 | 69 |
| ft4 20m 1619 (123 slots) | UberSDR stream (STUEY3D) | 917 | — | 62 | 49 | — |
| | old 1 pass | 614 | 605 | 46 | 38 | 7.0 |
| | old full depth | 750 | 679 | 47 | 39 | 7.0 |
| | new, loaded (1 pass) | 692 | 684 | 49 | 42 | 2.4 |
| | new, 2 passes | 846 | 812 | 54 | 43 | 5.2 |
| | new full depth | 848 | 814 | 54 | 43 | 5.5 |
| | jt9 -d 2 | 858 | 852 | 58 | 48 | 75 |
| | jt9 -d 3 | 917 | 900 | 60 | 49 | 246 |
