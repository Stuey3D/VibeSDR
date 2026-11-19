# HF data modes: ALE (2G), DOMINOEX, MT63 (V12, 2026-11-19)

Branch `v12-hfdata` (worktree `.claude/worktrees/v12-hfdata`), off `v12-experimental`. Three HF data modes decoded on a
VibeServer, from a listener's USB audio:

- **2G HF ALE** (MIL-STD-188-141, Appendix A): its own key, **ALE**, in the HF tab's Utility group. The link-setting bursts
  of HF nets (soundings, calls, replies) read back into one line per transmission, with the time.
- **DOMINOEX** 4 / 5 / 8 / 11 / 16 / 22 (and MICRO, 44, 88, and the old **FEC option**): a sixth family in the one
  **MFSK** key (textmodes.h). It shares THOR's spectra and tone search.
- **MT63** 500 / 1000 / 2000, short and long interleave: another family in the same key (the FAMILY row is now
  AUTO · MFSK · OLIVIA · CONTESTIA · THOR · DOMINOEX · MT63), with its own receiver (mt63.h).

## RESUME HERE

Everything below is built, committed and tested on this Mac. **Not done / owed:**

1. **Off-air DOMINOEX and MT63.** Neither was heard live (15 min of 3.582 MHz recorded 2026-11-19 20:56 UTC: see
   "Real signals"). Both are proven on the public sigidwiki reference recordings (real transmitters, through mp3) and on
   our own transmitter only.
2. **MT63 weak-signal edge.** At −2 to −4 dB (3 kHz, `tm_file`'s dilute measure) 10 of 18 synthetic overs come out whole;
   most of the misses lose only the first one to three characters (the lock comes after the interleaver has filled).
   Ideas, not tried: keep raw chips for the seven places before a place is chosen, so the first characters can be
   re-read; a soft finder (the band-edge score is the weak link below −4 dB).
3. **ALE sensitivity**: hard decisions and a 2-of-3 majority, as the standard describes; a soft vote would buy ~2 dB.
   Edge now ≈ −7 dB in 3 kHz (a 4-word sounding 20/20 at −6 dB, 1/20 at −9 dB).
4. **ALE in Band spots** (a wide capture's every ALE channel): not built. ALE nets scan, so a listener's dial catches only
   what lands on it; the Band machinery (DIGITAL-SPOTS.md) could run AleDecoder per HFLink channel cheaply (it costs
   0.02 % of an M5 core per channel), but the spots pipeline is built around slotted/period modes and a gallery tap, and
   ALE has neither. A design question for Stuart (below), not a quick add.
5. **Not compiled here**: the Android gradle build, the iOS prebuilt library (`modules/vibe-local-sdr/build_ios.sh` lists
   mt63 and ale_decoder now — the coordinator's build must run it, or iOS clients' local engine lacks both), the R36S
   handheld (`handheld-ui` is Linux-only: its headers do not compile on macOS), the whole-project `tsc` (it needs more than
   the 1 GB a process may use on this shared Mac — run by the coordinator). The changed TS services and ModeSelector were
   type-checked in isolation: no new errors.
6. `scripts/run-tests.sh` was NOT run (agent rules). `test-ale` and `test-textmodes` are picked up by it (deps added).

## Where every fact came from (the house rule: published first, GPL read for facts only, never copied)

### DOMINOEX
| Fact | Source |
|---|---|
| 18 tones, IFK+ (step − 2 mod 18 is the nibble; 16, 17 never sent), speeds, spacings (×2 for 4/5/8) | ZL1BPU, "DominoEX — an IFK Mode for HF" and "DominoEX Technical Information" (qsl.net/zl1bpu/MFSK) |
| The Nibble Varicode, primary and secondary alphabets | "DominoEX Varicode Table.pdf" v2.0 (28 July 2009), transcribed by script → `decoders/dominoex_varicode.h` |
| Two misprints in the PDF (both give two characters one code): "<" is 0,10,8 / 5,10,8; secondary "4" is 4,14,8 | the table's own pattern (each fills its run's one unused slot); "<" confirmed by reading fldigi's decode table (dominovar.cxx) |
| MICRO = 2 Bd at 8 kHz (4000 samples), single spacing; 44 and 88 at 11.025 kHz (256 / 128 samples, double / single) | read from fldigi's dominoex.cxx (facts) |
| The FEC option: MFSK16's coder and varicode on IFK+, interleaver L = 4 | ZL1BPU's page ("L=4 instead of L=10"); fldigi's `interleave(4, 4)`; **confirmed** on sigidwiki's DominoEX 11 FEC recording |
| FEC option's secondary alphabet (MultiPsk's code substitution) | not implemented (rare); its characters print as their primary codes |

### MT63
| Fact | Source |
|---|---|
| 64 carriers 4 bins apart in a 512-point FFT at R = 2 × bandwidth; 200 samples a symbol (5 / 10 / 20 Bd); odd carriers half a symbol after even; the lowest carrier at the centre − bandwidth / 2 | read from fldigi's mt63base.cxx / mt63base.h (Pawel Jalocha's code, GPL) as facts |
| DBPSK: a 1 keeps the phase, a 0 turns it π; Walsh-64 characters (code < 64 → +W, else −W); the transmit transform's butterfly; 127 escapes the next character; codes < 8 not shown | the same, as facts |
| The interleave patterns (short: 4,5,6,7 repeating, L = 32; long: 1…63,0, L = 64) | mt63intl.dat, as facts (two short sequences) |
| **The symbol shape** | **ours**: 14 cosine terms fitted (numpy Levenberg–Marquardt, scratch script) to the properties a shape must have — no ISI at ±200 / ±400 samples, a carrier 4 bins away orthogonal at ±100 / ±300 (the staggered neighbour), energy inside ±6 bins. It came out 0.9995 correlated with the transmitter's (which was not copied). |

### ALE
| Fact | Source |
|---|---|
| 8-FSK 750–2500 Hz, 250 Hz apart, 125 Bd, 3 bits a tone | Wavecom's MIL-188-141A decoder page |
| 24-bit word = 3-bit preamble + three 7-bit characters; Golay (24,12) each half; interleaved; stuff bit; ×3 = 49 symbols; 2-of-3 majority; word sync by unanimous votes | NTIA TM-95-166 (§ data link layer), NTIA "HF ALE Simulator" |
| Gray-coded tones, first-sent bit heaviest; A1 B1 A2 B2 interleave; the stuff bit last; B's check bits inverted; the Golay generator (the check bits of each unit data word); preamble numbers (DATA 0 … REP 7); the 38-character address set | read from LinuxALE (Charles Brain G4GUO, GPL) as facts — confirmed on the sigidwiki recording and on air |
| Address continuation in DATA / REP words, '@' padding; AMD = a CMD word in the 64-character set | the standard as summarised by the above; our parser |
| The decoder (syndrome table, the receiver, word sync, the protocol reader) | ours |

## The receivers

**DOMINOEX** (`DominoRx` in textmodes.cpp): THOR's spectra (one FFT of 2 × the symbol every quarter symbol, already
there for THOR — DominoEX adds receivers, not FFTs) and its finder. Per symbol: the strongest of 18 tones, the step, the
nibble; characters end when the next initial nibble arrives. The lock is the modulation's own evidence, counted over the
last 48 steps: at most one step IFK+ never sends (2 in 18 on noise or any other MFSK), a tone standing out (≥ 8 × the
others on average), the tones not split (a faster signal read too slowly), 13 of 18 tones used, 24 varied characters
with no impossible one. A character is shown only if every symbol that carried it stood out (≥ 6 ×, and a third of the
signal's own level). The secondary alphabet (the station's ID while the keyboard is idle) is its own `[ID] …` line, once
per change. **THOR or DOMINOEX?** One modulation: a THOR (or DOMINOEX FEC) receiver on the same spectra whose code shows
(its two pairings disagree) wins; a DOMINOEX already in the box gives way only to a THOR lock.

**MT63** (`mt63.cpp`): one real FFT (PFFFT) of the 512·D window every eighth of a symbol (D = 8 / 4 / 2 at 8 kHz), each
bin's phase referred to absolute time. The band's edges place seven candidate carrier sets (± 3 bins); each is read at
the best of eight timings (the carriers' symbol-to-symbol products strongest), the common turn a symbol taken out (the
squared products' mean, halved), the chips normalised per carrier, de-interleaved both ways at once, Walsh-transformed.
★ **The half-turn trap**: a place one bin off turns 0.78π a symbol, which the squared estimate reads as −0.22π — and every
character comes out inverted (code ^ 64: idle as '@', "CQ" as control codes) with the same Walsh share. The lock is taken
only by the best reading of all that reads TEXT the right way up (an EMA of "this is text" − "its inverse is text").

**ALE** (`ale_decoder.cpp`): 48 → 8 kHz; 24 complex filters (8 tones × 3 offsets: −62.5, 0, +62.5 Hz — an SSB dial a
little off; table period 128), summed a block of 8 samples, the window the last 8 blocks: tone powers every 1 ms; each of
the 24 (offset, timing) pairs keeps a 147-bit register. Acquisition: ≥ 28 of 48 unanimous votes, both Golay halves within
1 error, an address word in the 38 characters — confirmed by the next word 49 symbols on. Then one word every 392 ms
(up to 3 errors a half, ≤ 30 split votes) until three are missed. The protocol reader: TO/TIS/TWAS/THRU/FROM start an
address, DATA/REP alternate to extend it (the same one twice = a lead word was missed), a repeated scanning-call word is
not a new address, an address heard twice keeps its longest reading; CMD in the 64-set = AMD text. Lines:
`[ALE] 21:04:12  sound  G4XYZ`, `… (TWAS: not taking calls)`, `call  K1ABC → W1XYZ`, `response  W1XYZ → K1ABC`,
`ack  K1ABC → W1XYZ  (linked)` (a call back within 60 s), `via <THRU>`, `· CMD x`, `· AMD message (N characters)`.

## ⚖ The law (WTA s.48)

Addresses are not message content: always shown. An **AMD message's text** is content: the host shows it on an
**amateur frequency** (the band plan of this server's region, `vibetones::amateurBand` — HFLink's channels are amateur);
anywhere else only with the owner's opt-in **"ALE: show AMD message text off the amateur bands (to you only)"**
(`ale+amd`, off by default, a notice beside it on both setup screens), and then **to the owner's own signed-in sockets
only** (`textFramePriv`, as the tone decoders' 5-tone digits). Listeners and the gap log get the line with the length.
No client filters anything. DOMINOEX and MT63 are amateur keyboard modes: their text is shown as every MFSK family's.

## Wiring

- **Server**: host ext `ale` (`buildAleLocked_`), vibeserver.json `"ale"` (+ `aleWhy` off HF, `vibe_decoder_reach.h`
  2–30 MHz), `vibe_radio_caps.h` (an HF decoder; Icom too), owner switches `ale` + `ale+amd` (setup page and the app's
  ServerModeScreen), benchmark row **`dec_ale`** (a sounding and a call; its switch's primary row; HF-gated). DOMINOEX and
  MT63 travel in `mfskModes` (63 entries: AUTO + six families' ANY and sub-modes) — the clients draw FAMILY / MODE from
  it, so no client lists them; `dec_mfsk` now measures the whole key's AUTO. CMake ×2, `build_ios.sh`, run-tests deps.
- **Web**: ALE key (HF · Utility), `ale_status` → the status line, reach greying; `textModes.ts` hint/status name the six
  families and the list limit is 80. **App**: ALE key in ModeSelector (hint under it), DecoderClient (`ale`, 0x03,
  `ale_status`), caps, panel label, icon, SDRScreen. **R36S**: ALE in the decoder table (UTILITY), caps, status, demo
  line; the MFSK hint and list limit. Shared words: `src/services/aleDecoder.ts`.
- **Copy**: AboutOverlay V12 note, web tutorial's decoder card, the MFSK hints (app, web, R36S), owner switch labels.
  `sdrTour` / `pickerTour` / website do not list decoders by name — nothing there was wrong.

## Tests and results (this Mac, M5 Pro)

| Suite | What | Result |
|---|---|---|
| `test-textmodes` (extended) | every DOMINOEX (15) and MT63 (6) sub-mode manual and through the one AUTO at +10 dB; THOR 16 never shown as DOMINOEX; DOMINOEX FEC never shown as plain DOMINOEX first; the ID line; ANY of THOR / MT63 / DOMINOEX / OLIVIA print nothing on the others; the grouped list (63, six families); 13 sigidwiki references; RTTY and a carrier: nothing; **10 min white noise through each AUTO (incl. the one key): nothing, never locked** | **183 / 0** |
| `test-ale` | Golay: 4096 words × 0–3 errors corrected, 4 errors always detected, minimum distance 8; sounding, TWAS, a 15-character address, call → response → ack (linked), AMD (public line: length only; private: text); dial ±95 Hz; 12 kHz input; SNR sweep (a 4-word sounding, 20 trials: 0 dB 19, −3 20, −6 20, −9 1, −12 0 whole; **never a wrong address**); RTTY, random 8-FSK at 100 Bd, a carrier: 0 words; **10 min white noise: 0 words**; the sigidwiki recording; (`VIBE_ALE_REC`) the off-air capture | all pass |
| SIMD = scalar | `test-textmodes` NEON vs `VIBE_FORCE_SCALAR` + `PFFFT_SIMD_DISABLE` (`-ffp-contract=off`): one fingerprint `42069cc26d82822a`; `test-ale` whole output identical | identical (SSE2 not built here: the x86 builds belong on the Lenovo) |
| vibeserver | builds (Mac, `-j6` through the lock) | OK |

**Reference recordings** (sigidwiki, mp3 → 8 kHz float32, in `~/VibeSDR-mfsk-refs/samples`, not in git), through AUTO:
DominoEX 4/5/8/11/16/22 all named right and "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 1234567890" with 0–4 wrong
characters (mp3 at its SNR); DominoEX 11 FEC "uick brown fox jumps over the lazy dog 12" (named DOMINOEX 11 FEC); MT63
500S/500L/1000S/1000L/2000S/2000L all named right, "… fox jumps over the lazy dog 123456789" — the first words lost
while the interleaver fills (the clips start with the text). 2G ALE: two `sound SHAEENQ2` and a call to `USMANQ7`.

## Real signals (Stuart's UberSDR, recorded — never tuned anything else; 1 of 20 slots in use at the start)

| UTC (2026-11-19) | Frequency | Length | Heard |
|---|---|---|---|
| 20:23–20:38 | 7.102 MHz USB (HFLink 40 m) | 15 min | no ALE |
| 20:41–20:56 | 3.596 MHz USB (HFLink 80 m) | 15 min | **SL3ZXB's TWAS soundings at +03:35 and +03:42** — decoded, 0 errors on most words, nothing else printed |
| 20:56–21:11 | 3.582 MHz USB (80 m digital) | 15 min | no MT63 / DOMINOEX on air; **nothing printed** by the one key's AUTO or by ALE |

All 45 minutes of real band audio through the one text-mode AUTO (with DOMINOEX and MT63) and through ALE: nothing
printed except SL3ZXB's soundings — real-band false-decode check passed.

Recordings in `~/VibeSDR-test-audio/hfdata/` (local, out of git).

## CPU (Mac M5 Pro, one core; Pi 2 ESTIMATES scale by ≈ 60, from the one key's qemu count in DECODERS-WAVE2.md §19)

| | M5 | Pi 2 (estimate) |
|---|---|---|
| the one MFSK key's AUTO, 600 s of noise, before | 0.98 s (0.16 %) | ≈ 9–12 % of a core (measured by qemu then) |
| …after (+ DOMINOEX ×6, DOMINOEX FEC ×6, MT63 ×3 bandwidths × 2 interleaves) | 1.41 s (0.24 %) | **≈ 13–17 %** |
| of which MT63 alone (`any-mt63`) | 0.38 s | ≈ 4 % |
| of which DOMINOEX alone (`any-dominoex`, its spectra shared with THOR) | 0.35 s | ≈ 3.5 % |
| ALE, 300 s of noise (NEON / scalar) | 0.06 / 0.07 s (0.02 %) | ≈ 1 % |
| benchmark rows (`vibeserver-bench 3`, this Mac): `dec_ale` | 0.0 %, 50 frames, green | — |
| `dec_mfsk` (now the six-family AUTO) | 0.2 %, 27 chars, green | — |

★ **The trade-off (Stuart's rule: say it first)**: the one key's AUTO costs ~44 % more CPU than before; RAM is a few
hundred KB more (MT63's seven places × two interleavers ≈ 25 KB each per bandwidth; DOMINOEX tables 1 KB). MT63 at four
timings instead of eight cost 35 % less (0.25 s) but lost decodes (weak synthetic 7/18 → 5/18 whole, two references
started later) — so eight it is (never decodes for speed). The benchmark's `dec_mfsk` row measures the new cost on each
box and sets the default as before (never hides).

## Decisions for Stuart

1. **One key or two?** DOMINOEX and MT63 joined the MFSK key (FAMILY row), as the brief's "per the existing text modes"
   read; the key's legend stays "MFSK" (MT63 is not MFSK — "TEXT" or "KEYBOARD" would be truer). ALE has its own key.
2. **The AUTO's cost** (≈ +44 %, Pi 2 ≈ 13–17 % estimated): keep everything in AUTO, or make DOMINOEX FEC (rare; only for
   the old program) manual-only (it is in AUTO because plain DOMINOEX otherwise locks on it and prints its coded bits),
   or MT63 2000 manual-only?
3. **ALE AMD off the amateur bands**: owner-only opt-in as the tone decoders' 5-tone digits — or not offered at all?
4. **ALE in Band spots** (every HFLink channel of a wide capture at once): worth a design?
5. **The R36S** gets ALE as a TEXT box (one line a transmission) — fine, or a list view like JTTY's?
