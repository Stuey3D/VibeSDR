# DTMF and 5-tone selective calling (V12)

DECODER-SURVEY.md §2.12 (DTMF), §7 and row 30 (5-tone selcall). Two small tone decoders that run together on a listener's
NFM or AM audio:

- **DTMF** — the keypad tone pairs of ITU-T Q.23. On the amateur bands these are repeater, EchoLink and AllStar control
  codes; on PMR and telephone patches they are a dialled number.
- **5-tone selective calling** — a burst of single tones, one per digit, that calls a radio or identifies the sender
  (CCIR, EEA, EIA, ZVEI-1, ZVEI-2, ZVEI-3, DZVEI, PZVEI). Still used in European PMR and some amateur repeater access.

A listener tunes the channel in NFM (or AM) and presses **DTMF / 5-TONE**; VibeServer writes one line per DTMF string or
5-tone call it hears.

**Nothing has been decoded off air by us.** Everything below was tested on our own transmitter (unit tests and a fake
radio end to end), on 24 minutes of macOS `say` speech and singing voices, and on synthetic speech and music.

## Where it is offered

- **A listener's decoder** (`audio_extension_attach`, `extension_name: "tones"`), like SELCAL — one channel a person tunes.
- **Any frequency, every radio** (not in the reach table): DTMF and 5-tone live on NFM/AM channels anywhere from 10 m FM
  up, and every radio we drive demodulates NFM/AM. An Icom's FM/AM audio works too (`vibe_icom_decode.h`, as tuned).
- **Owner switch:** "DTMF and 5-tone selective calling" on the setup page and the app's ServerModeScreen; under it the
  owner's opt-in **"5-tone: show the called numbers off the amateur bands (to you only)"** (`tones+digits`, a "+" token in
  the CSV when ticked, **off by default**) with a notice beside it.
- **Benchmark:** row `dec_tones` (DTMF strings and three 5-tone calls in noise, both decoders): the switch's primary row;
  below green it starts OFF on a fresh setup with the figure beside it — never hidden. 0.31 % of an M5 Pro core.
- **Clients:** web and app, key **DTMF / 5-TONE** under **Amateur on the VHF and UHF tabs**. Under the key: **DECODE**
  (DTMF, 5-TONE — never both off) and **5-TONE STANDARD** (AUTO, or one of the eight tables pinned: the manual fallback).
- Copy updated where the key is described: `sdrTour` (the mode card's VHF/UHF Amateur sentence), the web tutorial's
  DECODERS card, an AboutOverlay V12 note (no on-air claim).
- **Not in the gallery.** A listener's tone lines are not kept (they are mostly "heard" lines, and kept digits could be
  replayed to someone else).
- **R36S handheld: not built** (see "Not built").

## The privacy rule ⚖ — decided on the server (`vibe_tones_run.h`)

DECODER-RESEARCH.md §4.2–4.3: amateur traffic is meant for anyone to hear; a professional system's addresses are "the
sender or addressee of a message". **Amateur is decided from the frequency**, never from a user's claim: the band plan of
the server's ITU region (`vibe_bands.h namedBands`, every entry labelled "amateur"; region unknown = Region 1, which has
the narrowest 2 m (144–146) and 70 cm (430–440) — PMR446 is not amateur). The dial is read when a sequence ends.

| | On an amateur frequency | Anywhere else |
|---|---|---|
| DTMF | the digits (`DTMF  *73#1234 · 8 digits … · 2 m amateur`) | `DTMF heard` — **never the digits, to anyone, no switch** (it can be a phone number) |
| 5-tone | the digits and the table (`ZVEI-1 5-tone  12345 · 70 ms tones · 2 m amateur`) | the format (`ZVEI-1 5-tone heard · 5 tones, 70 ms`); with the owner's opt-in, the digits **to the owner's own signed-in sockets only** |

★ **Why 5-tone digits are shown on the amateur bands (the decision the brief asked for):** amateur 5-tone is repeater
access and station identity on traffic that by licence is unencrypted and meant for general reception — the same footing
as the callsigns and talkgroups §4.2 already shows for amateur DMR/YSF. A wrong "amateur" call can only come from the
band plan, which is conservative where the regions differ.

★ **How "to the owner only" is enforced:** the host sends that line through `textFramePriv` — each decoder socket is asked
`DecoderPeer::privileged()`, which in the shim is "is this socket's session signed in as admin now" (`decoderSockAdmin`:
the session's spectrum/audio socket in `adminSocks`; on a single-listener radio, the radio's admin flag). Everyone else,
the gap log (a listener coming back) and any gallery tap get the public line. Tested on a shared dial where a listener's
socket is a mirror of the owner's decoder (`test-server-tones`). No client filters anything.

## The receivers (`decoders/tones_*.h`)

One receiver (`tones_rx.h`) on the listener's 48 kHz audio, on the decoder host's own thread (decoders last:
Network > Audio > Spectrum > Decoders): 48 → 8 kHz (`tones_dsp.h Decim6`, 96-tap Hamming sinc, flat to 2.9 kHz), a 300 Hz
high-pass (a CTCSS sub-tone would otherwise take a share of every block's energy), then either or both decoders.

**SIMD:** both hot loops (the decimator's dot product and the Goertzel bank, four filters a lane) run NEON / SSE2 via
`q65_simd.h`; the scalar path does the same IEEE operations in the same order (`volatile` products: no fused
multiply-add). `test-tones` checks them **bit for bit** in one binary and every decode the same.

### DTMF (`tones_dtmf.h`) — ITU-T Q.23 / Q.24

Every 5 ms, a 205-sample (25.6 ms) block, mean removed; for each of the 8 tones three Goertzel points (its frequency and
±19.5 Hz, half a bin either side, so a tone 1.5 % off reads full strength and its offset is measured) plus its second
harmonic — 32 filters. A block is a digit when:

1. the two tones carry ≥ 75 % of the block's energy (talk-off: speech and music spread theirs);
2. the next tone in each group is ≥ 8 dB down;
3. **twist** (high − low) within **−4 … +8 dB**;
4. each tone's measured offset within ±2 % (between Q.24's must-accept 1.5 % and must-reject 3.5 %);
5. each second harmonic ≥ 10 dB down, and the high tone is not at twice the low one (a note and its own harmonic:
   G♯5 830.6 Hz and 1661 Hz sit within 2.5 % of 852 / 1633).

Timing from the blocks: a block is "on" when it passes and holds half the digit's strongest block's power. A D-ms tone
gives ≈ (D − 10.6)/5 + 1 on blocks; a digit needs ≥ 30 ms by that measure (40 ms tones pass, 20/25/33 ms do not); a break
under 20 ms is bridged (Q.24: ≤ 10 ms bridged, ≥ 40 ms separates). Digits less than 1.5 s apart are one sequence, reported
when it ends (≤ 40 digits).

### 5-tone (`tones_fivetone.h`)

Every 5 ms a 20 ms Hann-windowed block on a 204-point Goertzel grid (400–2937.5 Hz, 12.5 Hz apart); a block holds ONE tone
when its peak (refined between grid points) carries ≥ 60 % of the energy. Blocks within ±1.5 % make a segment; segments
≤ 45 ms apart make a sequence, closed by 70 ms with no tone. A sequence is a call when it has **5–12 tones**, its period
(median spacing of the segment centres) is **25–130 ms** with every spacing within ±35 %, every tone is within **±2 %** of
one table's tone (each table's tones are ≥ 5.8 % apart), the whole call from **one** table, and **no tone twice running**
(that is what the repeat tone is for — a wavering note gives exactly that). **The repeat rule:** E is the digit before it
("123E4" → 12334).

**AUTO** picks, among the tables that fit, the one needing the fewest A–F tones other than its repeat, then the nearest
nominal period, then the order CCIR… (`toneTables()` order: ZVEI-1, ZVEI-2, ZVEI-3, DZVEI, PZVEI, CCIR, EEA, EIA).
**What AUTO cannot tell apart, by the tables themselves:** ZVEI-1/PZVEI (differ only in A, C), ZVEI-2/ZVEI-3 (B, C),
CCIR/EEA (A, C, F — the period decides: 100 vs 40 ms), and **DZVEI from the ZVEI family: DZVEI's digit n is ZVEI's tone
for n − 1**, so every DZVEI call is also a ZVEI call one digit lower. The line names a table that fits with *different*
digits beside the call ("ZVEI-1 5-tone 12345 (or DZVEI 23456)"); pinning DZVEI reads it right.

## Where every fact came from (the house rule)

★ **Published standards first; GPL code (multimon-ng, OpenWebRX) was not read for this work at all.** Everything is ours.

| Fact | Source |
|---|---|
| DTMF frequencies, keypad | ITU-T Q.23 |
| ±1.5 % accept / ±3.5 % reject, 40 ms accept, ~23 ms reject, 40 ms pause, 10 ms break bridged | ITU-T Q.24's receiver figures **from our knowledge of it — the document itself was not re-read in this session**; its twist limits are per administration, ours are our own (above) |
| 5-tone tables, all eight, digits 0–F | Wikipedia "Selcall", Tone Sets table (read 2026-11-16) |
| ZVEI family check, 70 ± 15 ms, ±1.5 % decode / ±4.5 % reject | Signal Identification Wiki "ZVEI Table" (a transcription of a manufacturer's table) — agrees for ZVEI-1, ZVEI-2, PZVEI; **disagrees for ZVEI-3 and DZVEI** (see decisions) |
| CCIR 100 ± 10 ms | Wikipedia "CCIR (selcall)" |
| EEA 40 ms, EIA 33 ms | common radio programming-manual values; **no primary source found** — used only to break ties, never to refuse a call |
| The repeat rule (E) | Wikipedia "Selcall" |

## CPU and memory

- **CPU (M5 Pro):** both decoders on 121 s of audio in 0.26 s (NEON) / 0.86 s (scalar) — **0.2 % of one core**;
  `dec_tones` benchmark row 0.31 %. A Pi 2 is perhaps 10–20× slower (~3–6 % of a core) — **not measured**; the row measures
  it on the box and sets the default.
- **Memory:** a few KB (two block rings, coefficient tables, a 600-block run buffer, ≤ 16 segments, ≤ 40-digit sequences).
  Every structure is bounded.

## Tests

| Suite | What | Result |
|---|---|---|
| `test-tones` (vibeserver/, run-tests.sh) | tables; all 16 keys; 200 random dialled strings at 25 / 15 / 10 dB SNR (3 kHz) — every one exactly right; twist −3.5 / +7 read, −6 / +10 refused; 40 ms tones and pauses read, 20 ms not; a 10 ms break bridged, a 40 ms pause two digits, a 1.5 s held key one digit, two sequences 2 s apart; ±1.5 % 16/16 keys, ±3.5 % 0/16; under an 88.5 Hz CTCSS; every 5-tone table 60 random calls at 20 dB (repeats in them) all right; every table at 40/50/70/100 ms; the repeat rule both ways; a 7-tone call; a 3-tone burst refused; the period tie-break (CCIR 100 ms vs EEA 40 ms); DZVEI pinned / listed beside a ZVEI call; a pin reads only its table; ±1.5 % read; ±4.5 % never the call sent; CTCSS; 80 calls at 15 / 10 dB all right; **nothing** in 5 min of white noise, 2 min noise + CTCSS, 6 min + 2 min of synthetic speech, 5 min of synthetic music (harmonics, chords, arpeggios), 10 min of PURE-SINE melody at 70/100 ms a note (the worst case: ≤ 3 allowed, 0 measured), DTMF read by 5-tone, 5-tone read by DTMF; SIMD = scalar bit for bit; cost | **66 / 0** |
| `test-tones wav <file>…` | recordings as a talk-off corpus: 24 min of macOS `say` (18 voices incl. the singing ones: Bells, Cellos, Good News, Organ; files in the scratchpad, **not committed**) | **0 decoded** |
| `test-server-tones.mjs` | the real server, fake rtl_tcp, NFM clip: 2 m — DTMF and both calls with digits, the band named, status, pins (CCIR, DTMF off), detach; **PMR446 — "heard" lines and the digits in nothing any socket was sent**; the opt-in on a shared dial — the signed-in owner gets 5-tone numbers, the listener's mirror never, DTMF digits reach nobody; opted in, nobody signed in — no numbers; range-locked radio; blocked | **24 / 0** |
| `test-web-tones.mjs` | the binary's embedded page, headless Edge `--mute-audio`: VHF tab, the key under Amateur, the options (AUTO + 8, both decoders lit), the lines with digits, the status, CCIR pinned + DTMF off, never both off, 390 px, no exception | **20 / 0** |
| `test_tones_app.ts` | the shared client half (the standards = the server's table), both clients' wiring, both setup screens (same notice), the copy | **35 / 0** |

Also re-run and passing: test-radio-caps (244), test-decoder-reach (76), test-decoder-hosts, test-bench-decoders
(`dec_tones` 0.31 %, decodes, has a rule), test-selcal, test-setup-radio-caps (the Icom tab lists tones), check-setup-page,
test-bench-defaults, test-setup-mode-tree (64), test-web-decoder-reach, test-server-decoder-reach, test_decoder_bands,
test_balloon_app, test_ncdxf_app, test_decoder_reach_clients. tsc: no new errors.

Measured, not pinned: DTMF at 6 dB SNR 180/200 strings exactly right, 20 with a digit lost or split; 5-tone at 6 dB 79/80.
Through the server's NFM chain the DTMF tone length reads ~61 ms for a 70 ms tone (the length estimate is shown only as
information; nothing is refused on it).

## What is verified, and what is not

- **Verified:** our transmitter → the real server (NFM demodulator, decoder host, privacy routing) → both clients; talk-off
  against macOS synthesised speech and singing voices.
- **Synthetic only:** every decode. No off-air DTMF or 5-tone has been decoded; no real repeater, PMR or EchoLink signal
  has been through it. The talk-off corpus has no real off-air speech or broadcast music (the recordings in
  ~/VibeSDR-test-audio were deleted 11-09).
- **The ZVEI-3 and DZVEI tables** (see decisions) and the EEA/EIA periods are unconfirmed against a primary source.
- The app's options under the key have not been seen on a device (tsc and source checks only); the web page was checked
  in headless Edge on this Mac only.
- Pi 2 CPU: not measured.

## Not built

- **Off-air verification** — needs a 2 m / 70 cm repeater with DTMF traffic (or a handheld keyed into a dummy load near
  Stuart's Pi 500 RTL) and a PMR system using 5-tone.
- **R36S handheld view** — its decoder views are the separate C++ UI (handheld-ui/); not a trivially small addition.
- **Tone-burst access (1750 Hz), CTCSS/DCS identification, two-tone sequential paging, FMS/BOS (skip: restricted)** — out
  of scope.
- **A gallery copy** — deliberately not kept (above).
- **Status-gap / two-period status tone variants** (Wikipedia "Status Gap") — a status digit after a gap of one or two tone
  periods currently ends the call before it (the call is reported without its status digit; the status digit alone is
  too short to be a call) — by construction, **not tested**.

## Decisions for Stuart

1. **ZVEI-3 / DZVEI tables.** Wikipedia's ZVEI III has ZVEI-1's digits with E = 970 Hz; the Signal Identification Wiki's
   transcribed table has ZVEI-3 = DZVEI's digits (2200 Hz for 0) with E = 2400 Hz, and different DZVEI letters. We use
   Wikipedia's (complete) and name a fitting alternative beside the call. A primary ZVEI document would settle it.
2. **5-tone digits on the amateur bands** — shown (my recommendation, reasons above), or format-only there too?
3. **"To the owner only" for the opt-in** (as ACARS text is) — or should the owner's opt-in show numbers to every listener
   on their server? I kept it owner-only: the law (WTA s.48) is about disclosure, and the owner is the one who decided.
4. **DTMF twist −4 … +8 dB** (ours, not a telephone figure): the low side is held to 4 dB because every speech-like false
   digit we measured had the low tone 4–7 dB louder. A radio with a badly de-emphasised high group (high > 4 dB down)
   would lose digits. OK?
5. **Group placement** — the key sits under Amateur on VHF and UHF; PMR use (446 MHz) is under the same heading. A
   "PMR / Utility" group on VHF/UHF would be more honest but is a new group in both clients.
6. **Default when the benchmark is below green** — the rule follows the other listener decoders (off with the figure).
   It costs 0.3 % of an M5 core; worth a lighter rule?
