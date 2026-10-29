# CW (V12): a listener's Morse decoder and a skimmer, from one light engine

Stuart, 2026-10-25: *"CW skimming would be awesome but … EXTREMELY CPU intensive so if we can do it and make it
lightweight by using SIMD NEON SSE2 etc then that would be a HUGE win"* — and *"we also need to compensate for variable
WPM for real human operators"*.

Branch `v12-cw` (worktree `~/VibeSDR-cw`). Engine: `android/app/src/main/cpp/decoders/cw_skimmer.{h,cpp}`. Server:
`vibe_cw_run.h` (one CW run inside a decoder host), `vibe_decoder_host.h` (ext `cw` and `cwskim`), `vibe_wide_tap.h`
(a skimmer's slice on a one-pipeline radio), `local_sdr_shim.cpp`, background decoding + gallery in `vibe_bgdecode*.h`.
Web: `web/client/src/decoders.ts`, `main.ts`, `index.html` (the CW button, ONE SIGNAL / SKIMMER, the list and spots),
`adminBg.ts` (the admin line), the gallery's CW chip. App: the gallery's CW chip (`src/components/GalleryOverlay.tsx`), and since 2026-10-25 the whole CW panel — Mode → CLIENT DECODERS → CW (`src/services/cwList.ts`, `src/components/CwView.tsx`, `DecoderClient` 'cw', `scripts/test_cw_list.ts`).

Built from ITU-R M.1677 (the code and its timing) and textbook DSP. No other decoder's source was read.

## Architecture

**One windowed FFT per hop over the whole segment** (PFFFT; ≈ 47 Hz bins whatever the rate, hop N/4 ≈ 5.3 ms), never a
filter and a decoder per channel: a 96 kHz segment with forty stations costs the same transform as an empty one. Every
loop over ALL bins — window, power, the three per-bin averages (short, long, the noise floor), the "anything here" test —
is a NEON body (ARMv7-safe intrinsics), an SSE2 body (x86-64 and the i386 package) and the scalar reference
(`VIBE_FORCE_SCALAR`, also what the ARMv6 Pi Zero/1 runs).

The **per-signal** work is scalar and runs only for carriers that are there: a tentative signal (a local peak standing
above the floor) gets four **speed lanes** — whole decoders at fixed smoothing for 45, 25, 13 and 7 WPM (fixed, because a
decoder that adapts its own smoothing has a trap either way) — and is shown only once its keying has Morse's shape and it
is not a copy of a stronger neighbour (sidelobes, clicks, splatter correlate with the strong signal; two operators never
do). Its first seconds are then decoded again with the timing learnt since.

### Human fists — the live timing (2026-10-25)

Each lane follows **five centres** element by element — dot, dash, element gap, character gap, word gap — so the dash:dot
ratio (2.5–4:1), the weighting and the spacing are the sender's own, not 1:3:7 assumed. Each split is the geometric mean
of its two neighbours.

- **Speed changes inside an over** (15→25 WPM): one element moves its centre a clipped step; a mark's move scales the
  gaps with it (√f), so spacing is followed at once.
- **Between overs** (the reply is another operator): after a pause every lane holds the new over until six marks say its
  speed, then decodes it with that speed. The signal itself starts the over **on hold**: the best lane FOR THIS OVER is
  chosen and its text shown in one piece — so a speed change that moves the signal to another lane loses no character.
  Otherwise a lane hand-over waits for a word boundary in both lanes.
- **Ragged fists** (straight key ±20–30 %, a bug's hand dashes): the centres' steps and re-centrings shrink as the marks'
  spread grows (to a quarter at ±30 %), so misread elements do not drag them. An over's re-sync rescales only for a change
  bigger than its own jitter could show.
- **Gap clusters that cannot lock wrong**: re-centred from two starts — the live centres and 1/3/7 of the marks' dot (the
  marks cannot be corrupted by the gaps). The live result is kept unless it is impossible (an element-gap centre outside
  0.4–2.2 dots, a character-gap centre under 1.7 dots) or the anchored one is clearly tighter. Without it a lane once took
  its element gaps for character gaps for a whole QSO ("G4ABC" → "TTE EIET").
- **Uneven / Farnsworth spacing**: an empty middle cluster is the character gaps stretched, not "no character gaps"; the
  word centre is held between 2 and 3.5 character gaps (noise blips cut the pauses between overs into word-sized pieces).
- **A lane slower than the signal** is marked down on a clean, ragged signal (its smoothing fills a straight key's short
  element gap: two dots read as a dash, D → M) — never on a weak one, where the smoothing is the point.
- **Noise tails**: a shown signal that starts to read like noise (three E/T in a row, or its lane's speed out of range) is
  held, and what was held is dropped if it reads as noise. The signals list shows the text actually sent, and the speed it
  was read at.

## Human-sent Morse: soft timing, a beam, a small model of ham CW (2026-10-29)

Stuart's 80 m screenshots (Lenovo RSP1A, 21:16–21:35) showed both ways a hard split fails a human fist: **3555.30,
G4TPJ at 24 WPM** — word gaps barely longer than letter gaps, so the words ran together
("OPISRICHARDRICHARDESQTHISNRTONBRIDGE…"); and **3554, slow hand-sent at 6–10 WPM** — stretched gaps inside letters read
as letter and word ends ("E E BT LA TEE M T O"). Plus false locks ("52 WPM" YEEYEE…, "62 WPM" TNTTTMM…), a suspect spot
(DB70DLRS) and an Italian QSO on 3554 that must stay Italian.

- **The reader** (`decoders/cw_reader.h`, included by `cw_skimmer.cpp` only). What is SHOWN now comes from it; the lanes'
  hard decisions still drive every statistic (quality, holds, lane choice, confirmation), so what was tuned there is
  unchanged. Each mark gets P(dash) and each gap P(element / letter / word end) from the station's own centres and
  spread; the reader keeps the best 8 readings of the recent run and commits text only where they all agree, at a gap
  no reading could take for anything but a word end, or when a run is too long to hold. Cost: a few hundred operations
  a CHARACTER per lane, only on confirmed signals.
- **Per-station letter / word centres for the reader**: two clusters of the recent letter-or-word-sized gaps, stored
  in the station's own units (so they follow a fist that speeds up or slows down). Where the two are clearly apart the
  decision is sharp; where they are barely apart (G4TPJ: ≈ 1.2:1) the spreads are widened so timing only leans and the
  model decides.
- **The model — "the cheap option" Stuart chose** (21:45): a character 5-gram model of ham CW with Witten–Bell
  back-off, ≈ 1.1 MB of tables, plus a 526-word list (`decoders/cw_lm_data.h`, generated by `scripts/gen-cw-lm.mjs`
  from `src/utils/cwVocab.ts` — the SAME vocabulary the app's PLAIN key and CW TERMS GUIDE use). Trained on a SYNTHETIC
  corpus: QSO, contest and park templates, ITU-series callsigns, RST, names, rigs, weather, and the same exchanges in
  Italian, Spanish, French and German. Nothing scraped. Its weight (0.55) is small beside clear timing: it only chooses
  among readings the timing allows and never puts in or changes a letter.
- **Slow stations get 24 s, not 10, to show they are Morse** (8 WPM with long gaps made 13–16 marks in 10 s; the
  confirmation asks for 20) — only when the keying already looks keyed.
- **Fast locks need real text**: above 35 WPM a lane must read ≥ 2.5 elements a letter and its own reading must fit the
  model (under 5.5 nats a symbol: calls, CQs and reports ≈ 1.4–2.5; the 80 m locks measured 5.9–10). **The speed
  sanity check**: a station with an established speed does not double or halve it from one word to the next without a
  clearly better reading.
- **The speed as a RANGE** (`wpmLo`/`wpmHi` in `cw_signals` rows and `cw_spot`): the 20th–80th percentile of the last 24
  letters' own speeds, around the figure shown, clamped to −30 %/+45 %; one figure when within ±6 % or the text is poor.
- **Spots must make sense**: the call's first characters must be an ITU series (no Q, no 0/1, a lone letter only for
  B F G I K M N R W, the letter-figure blocks A2–A9, C2–C9, E2–E7, S2–S9, T2–T8, V2–V8, Z2–Z8 …); a four-letter suffix
  whose shorter form the signal also sent is that call (DB70DLRS → DB70DLR), otherwise it needs one more hearing.

- **Clients (app + web)**: the speed range (`src/utils/cwFormat.ts` `cwWpmText`); a **PLAIN** key beside CLR in the CW
  box — a toggle, remembered per device — that shows the text through `plainEnglish()` (codes only; callsigns, names,
  numbers and other languages as sent); a **CW TERMS GUIDE** and a **QRZ.COM ↗** key under "NOT SURE WHAT A SIGNAL IS?"
  (`src/components/CwTermsGuide.tsx`, `web/client/src/cwGuide.ts`); every spotted or decoded callsign (CW, PSK, JTTY,
  FT8/digital spots) opens `qrz.com/db/<CALL>` in the same web view as the Signal ID wiki. QRZ shows full details only
  when signed in; nothing is fetched or stored from it. Tests: `scripts/test_cw_vocab.ts`, `scripts/test-web-cw.mjs`.

### Scored (`cw_bench human`, 48 kHz IQ; before → after)

| case | CER +5 dB | CER 0 dB | words run together (0 dB) |
|---|---|---|---|
| **the Tonbridge line**: G4TPJ, 24 WPM, word gaps 1.2 × letter | 22.8 → **0.3 %** | 23.8 → **2.5 %** | 85/92 → **0/92** |
| word gaps 1.4 × letter, 24 WPM | 15.9 → 0.3 % | 17.5 → 0.3 % | 68/90 → 0/90 |
| word gaps 1.3 × letter, hand key 20 WPM | 16.9 → 4.8 % | 14.5 → 3.2 % | 34/51 → 4/51 |
| ITALIAN, word gaps 1.3 × letter (must stay Italian) | 20.1 → 1.6 % | 20.9 → 1.9 % | 73/75 → 5/76 |
| slow straight key 8 WPM, long gaps | never shown → 20.3 % | never shown → 14.6 % | — |
| ITALIAN 18 WPM, keyer 20 WPM (must stay clean) | 0.3 % both | 0.3 % both | 0 |

The fists table above, after: bug key 11.5/19.8 → 8.4/9.7 %, straight key σ 30 % 29/32 → 19/25 %, long uneven gaps
7.6/6.5 → 2.3/2.1 %, everything else equal or better. Accuracy at 40 WPM −5 dB 23–26 → 20 %.

**Real bands** (Stuart's UberSDR, 2026-10-29 22:01–22:51 UTC, 48 kHz I/Q in `~/VibeSDR-test-audio/cw/`:
`uber_80m_3530000_…`, `…3550000…`, `…3590000…`, `uber_40m_7020000_iq48_2026-10-29.wav`; `cw_bench wav <file> <centre> 44000`):

| recording | spots before → after (calls) | rows at ≥ 40 WPM (false locks) before → after |
|---|---|---|
| 80 m 3530 (20 min) | 11 (7) → 14 (9) | 10 → 1 |
| 80 m 3550 (25 min) | 9 (5) → 12 (8) | 4 → 0 |
| 80 m 3590 (25 min) | 0 → 0 | 7 → 3 (a machine-keyed signal at 3599.4) |
| 40 m 7020 (25 min) | 5 (5) → 9 (7) | 1 → 0 |

Every spot after is a plausible real call (G0ORC, DL1HR, F6IDP, DL5YAS, EA1EYL, CT1BMW, EA5AKM …) except one doubtful
GBC6K. A hand-checked sample of ten windows of human-sent text on 3550 (F5IND/FR5ZEP, DL1HR, IZ2AVK, GI3BST, an Italian
operator near Monza, DJ5AZ, G0ORC — the reference written from the keying where it was unambiguous): CER 20.1 → 8.3 %.
Words the model split or joined wrongly where the timing allowed both ("TAVERN ES", "TOQUICK", "HOMEM MADE") are the
price of a tie-breaker. New on 3590: the beacon IQ5VK/B JN53IB, never shown before. The German QSO on 3530 (HB9AUK)
reads "NUR 500 METER VON DER GRENZE", not "G R E N Z E Z U".

**Pi 2 — MEASURED on the hardware** (Cortex-A7 900 MHz, the armhf NEON build from the release cross image, a standalone
`cw_bench` in `/dev/shm` at nice 19, removed afterwards; vibeserver untouched), % of ONE of its four cores, before → after:

| input | before | after |
|---|---|---|
| real 80 m, 48 kHz IQ (3 min of the 3550 recording) | 3.76 % | 3.91 % |
| busy synthetic, listener 12 kHz audio | 0.44 % | 0.47 % |
| busy synthetic, skimmer 48 kHz IQ (109 keyed tones) | 6.0 % | 6.4 % |
| busy synthetic, skimmer 96 kHz IQ (224 keyed tones) | 16.0 % | 17.3 % |
| busy synthetic, skimmer 192 kHz IQ (449 keyed tones) | 43.0 % | 44.3 % |

So the reader and the model cost ≈ 4–8 % more than the engine alone. (The synthetic runs are 6 s, so most of those tones
are still tentative — the reader runs only on confirmed signals; the real recording is the fair figure.) ★ The engine
itself on a busy 96 kHz segment is ≈ 16 % of a Pi 2 core — the estimate below (7–11 %) was low.

## Measured

`scripts/test-cw.sh` builds the bench (`tools/cw-bench`) twice — SIMD and scalar — and runs it. Synthetic Morse is PARIS
timing, SNR quoted as carrier over noise in 2.5 kHz; CER = character errors / characters sent, spaces count.

**Accuracy, one signal, 70 s of text** (IQ 48 kHz skimmer / audio 12 kHz listener):

| SNR | 12 WPM | 20 WPM | 30 WPM | 40 WPM |
|---|---|---|---|---|
| +10 dB | 0.6 / 0.6 % | 0.6 / 0.6 % | 0.6 / 0.6 % | 0.3 / 0.0 % |
| 0 dB | 0.6 / 0.6 % | 0.6 / 0.6 % | 0.6 / 0.6 % | 1.1 / 0.9 % |
| −5 dB | 0.6 / 0.6 % | 3.4 / 2.3 % | 8.6 / 4.6 % | 23 / 26 % |
| −8 dB | 6.9 / 5.7 % | 21 / 6.9 % | — / 33 % | — / — |
| −10 dB and below | not decoded | | | |

(0.6 % is the decoder joining a few characters in. One draw at 40 WPM 0 dB ranges 0.3–3.4 % with the noise seed;
test-cw judges the mean of three.)

**Human fists**, 8 overs each (a QSO), 48 kHz IQ — beside the **timing floor**: the same keying cut by an oracle at the
sender's TRUE centres, with no noise — what any fixed-threshold reader loses to the fist itself.

| fist | +5 dB | 0 dB | floor |
|---|---|---|---|
| clean 20 WPM | 0.3 % | 0.0 % | 0 |
| ramp 15→25 WPM in each over | 0.3 % | 1.0 % | 0 |
| ramp 28→16 WPM in each over | 0.3 % | 0.3 % | 0 |
| step between overs 15/28/18/32/14/26/20/35 | 0.3 % | 0.3 % | 0 |
| dash:dot 2.5:1 / 4:1 | 0.3 % | 0.3 % | 0 |
| heavy (+30 %) / light (−25 %) keying | 0.3 % | 0.3 % | 0 |
| element gaps 1.5 / 0.7 dots | 0.3 % | 0.3–0.5 % | 0 |
| Farnsworth 25/12 | 0.3 % | 0.3 % | 0 |
| straight key σ 10 % / 15 % | 0.3 / 1.3 % | 0.3 / 0.8 % | 0 / 0.5–0.8 % |
| straight key σ 20 % | 6.5 % | 6.5 % | 3.4–4.4 % |
| straight key σ 30 % | 29 % | 32 % | 20–22 % |
| bug (dots exact, dashes and gaps ±25 %) | 11.5 % | 19.8 % | 9.4–15.1 % |
| long, uneven gaps (5 / 10 dots, ±25 %) | 7.6 % | 6.5 % | 3.4–3.9 % |
| all at once: ramp, σ 15 %, 3.4:1, heavy | 1.3 % | 0.5 % | 3.1–3.7 % |

**False signals**: 30 minutes of pure noise at 96 kHz IQ and at 12 kHz audio — 0 signals, 0 spots, 0 characters.
**Many signals** (`cw_bench multi`: 12 in 48 kHz, 60–300 Hz apart, half hard-keyed, one at +30 dB): 9 decoded, 10
spotted, one false spot (TK1RR: OK1RR's call misread the same way twice).

**Real recordings** (Stuart's UberSDR, 192 kHz I/Q, `~/VibeSDR-test-audio/cw/`): 40 m afternoon — IZ4PHG spotted, QSO
text readable ("CQ DE IZ4PHG", F5MGU's "TNX DR MICHAEL - QRG BUSY", OH3UQ's CQ, GD0OUD/LA4SI); 40 m evening
(`uber_40m_7020000_iq192_evening.wav`, 2026-10-25 19:45 UTC) — S53BV spotted, LA1MFA's CQ, F8BCU's CQ, ZS6BKW, IU3RGS,
"QSO, HPE CUL =73 + GD0OUD DE LA4SI <SK>"; 20 m was nearly empty (one weak station). Weak and ragged signals on the real
band still decode as broken text, and a few rows are noise read as fast E/I/T soup.

**CPU** (Apple M5 Pro, one core, `cw_bench cpu`: a busy segment, one keyed signal every ≈ 400 Hz):

| input | signals | SIMD (NEON) | scalar loops | scalar loops + scalar FFT |
|---|---|---|---|---|
| listener 12 kHz audio | 8 | 0.010 % | 0.016 % | 0.018 % |
| skimmer 24 kHz IQ | 51 | 0.068 % | 0.082 % | 0.093 % |
| skimmer 48 kHz IQ | 102 | 0.155 % | 0.181 % | 0.208 % |
| skimmer 96 kHz IQ | 205 | 0.358 % | 0.409 % | 0.468 % |
| skimmer 192 kHz IQ | 376 | 0.81 % | 0.95 % | 1.08 % |

The real 40 m recordings at 192 kHz: 0.33–0.36 % of an M5 core. With hundreds of signals the per-signal (scalar) lanes
are most of the cost, which is why the SIMD gain is 25–35 % here rather than the 3–4× of the per-bin loops alone.

**Pi 2 (Cortex-A7, 900 MHz): an ESTIMATE, not measured on the hardware.** The bench cross-builds for armhf (the release
cross image, `-marm -mfpu=neon-vfpv4`, NEON kernels) and under qemu-arm (Cortex-A7) gives a fists table identical to the
M5's and no false signal on noise — so the ARMv7 NEON path computes the same answers; its SPEED was not measured on a Pi. An A7 at 900 MHz is roughly 20–30× slower than an M5 performance
core on this float work, which puts a busy 96 kHz skimmer at ≈ 7–11 % of one of its four cores and a listener's decoder
under 0.5 %.

## Tests

- `vibeserver/test-cw.cpp` (run by `scripts/run-tests.sh`): the code table, callsign rules, the floor's calibration, CER at
  0 / −5 dB on IQ and audio (each the mean of three draws), every human-fist case against its limit, 5 min of noise →
  nothing, six stations → six spots and no false one, both host paths, the gallery's CW summary card.
- `scripts/test-server-cw.mjs`: the real server on a fake rtl_tcp replaying the bench's clip — the skimmer on a
  one-pipeline radio and a range-locked one, a second listener's own segment, ONE SIGNAL's text and its spot's RF.
- `scripts/test-web-cw.mjs`: the built web page in headless Edge (`--mute-audio`) — the CW button, the skimmer list and
  spots, a tap on a row to tune, ONE SIGNAL, 390 px with nothing scrolling sideways, the gallery's CW chip; screenshots.
- `scripts/test-setup-bgdecode.mjs`: the setup page offers the three skimmers on USB and its list mirrors the server's.
- `scripts/test_admin_bg.ts`: the admin line names a skimmer "CW skimmer" with its live spot count.

## Not done

- Weak signals: nothing below −8 dB (2.5 kHz). A signal is confirmed only when its keying is Morse beyond doubt; a lower
  bar was tried and made −8 dB worse.
- QSB deeper than ≈ 15 dB at +5 dB: 30–50 % CER (the troughs go below what is decodable; a faster mark-level sink helped
  −5 dB and hurt −8 dB, so it was not changed).
- Straight keys at ±30 % and bugs remain ≈ 1.3–1.5× their timing floor.
- The app: only the gallery chip. The app has no CW decoder screen of its own yet.
- Real ARMv7 timing (the Pi 2 figure above is an estimate).
