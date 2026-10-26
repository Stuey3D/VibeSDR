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
| jt9 -d 3 | JT9D3 | |

An instruction count is not a cycle count (an A7 takes two cycles for a 128-bit NEON op); it counts the work.
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
- **FT4's SNR scale** (reads ~8.6 dB high against STUEY3D, docs/v12/JS8-FT2.md) is unchanged.

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
