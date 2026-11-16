# High-altitude balloons (V12): UKHAS RTTY telemetry, Horus Binary and SSDV pictures

DECODER-SURVEY.md §2.11 (and §6.4, SSDV from satellites). Amateur balloon payloads — school and club flights, Project
Horus — send where they are on 70 cm, mostly 434.000–434.650 MHz in the UK, as:

- **UKHAS RTTY telemetry** — `$$CALL,count,time,lat,lon,alt,…*CRC` sentences, CRC-16-CCITT, at 50–600 baud, 7 or 8 bits,
  1–2 stop bits, any shift from ~300 to ~900 Hz, received in USB;
- **Horus Binary v1 / v2** — a 22- or 32-byte packet, Golay-coded, 4-FSK at 100 baud (usually 270 Hz between tones);
- **SSDV pictures** — a JPEG cut into 256-byte packets (Reed–Solomon in each), sent as 8-bit RTTY between the sentences.

A listener tunes the payload in USB and presses **BALLOONS**; VibeServer finds everything on that audio by itself and
shows one row per payload (position, height, what else it sent) and the pictures building up as their packets arrive.

**Nothing has been decoded off air by us.** 434 MHz is out of reach of Stuart's HF UberSDR and no flight was on during
this work. What *has* been checked against the real thing: Project Horus's own published sample recordings (see
"Verified"), which pass their CRCs through our coder.

## Where it is offered

- **A listener's decoder on USB** (`audio_extension_attach`, `extension_name: "balloon"`), like RTTY — not a whole-radio
  mode like SONDES. Why: a payload is ONE narrow signal a person tunes (RTTY a few hundred hertz wide, Horus ~1 kHz, both
  inside a USB passband), and payloads sit on different frequencies, so each listener follows their own. Radiosondes are
  the opposite (many on a 2 MHz band at once, found by scanning it), which is why SONDES takes the radio.
- **Reach:** 70 cm (430–440 MHz) — `vibe_decoder_reach.h`. A radio that cannot get there (an HF+, a locked FM range, an
  RTL with an HF-only setup) draws the key **greyed with the reason**; vibeserver.json says `"balloon": false` and
  `"balloonWhy"`. An Icom's USB audio works (IC-9700 / IC-705 on 70 cm; `vibe_icom_decode.h` takes the dial as tuned).
- **Owner switch:** "Balloons (UKHAS, Horus, SSDV)" on the setup page and in the app's ServerModeScreen (the phone's own
  server runs it — it is plain C++ with no library, so it is NOT in `test-bench-defaults.mjs`'s NOT_IN_APP list).
- **Benchmark:** row `dec_balloon` (UKHAS 300 Bd + Horus, the whole AUTO search) — `ruleFor` makes it the switch's primary
  row and a decoder slot: below green the switch starts OFF on a fresh setup, with the figure beside it; never hidden.
- **Clients:** web and app — the BALLOONS key sits on the **UHF tab, under Amateur** (the panel opens there when the dial
  is on 434 MHz); its options are AUTO with the manual pins under it. Gallery: the reports as a text run ("Balloon
  telemetry"), each SSDV picture as a `.jpg` (complete, or partial with its packet count when the decoder was stopped).
- **R36S handheld: not built** (see "Not built").
- Copy updated where the key is described: `sdrTour` (the mode card's UHF/Amateur sentence), the web tutorial's DECODERS
  card, an AboutOverlay V12 note (no on-air claim).

## The receiver (`decoders/balloon_rx.h`)

One receiver runs everything at once on the listener's 48 kHz audio, on the decoder host's own thread (decoders are
last in Stuart's order: Network > Audio > Spectrum > Decoders).

1. **Front end** (`balloon_dsp.h`): 48 → 12 kHz, a 48-tap windowed sinc (cut 5 kHz). The hot loop: NEON / SSE2 four
   lanes at a time and a scalar path that sums the same four lanes in the same order — `test-balloon` checks the two are
   **bit for bit equal** (the scalar path uses `volatile` products so no compiler fuses a multiply-add).
2. **One spectrum** (4096 points, 2.9 Hz bins, ~1 s smoothing, ~6 a second) read by both tone finders: `findFour` (four
   equally spaced peaks within 15 dB of each other, spacing 80–1500 Hz) and `findPair` (two peaks 150–1200 Hz apart).
   ★ Fast RTTY's own spectrum (300 Bd and up) can show four evenly spaced peaks, so four tones are only BELIEVED as
   Horus once a packet has passed: before that a placement is a try (nothing passes its CRC, nothing is shown); while
   Horus is decoding (a packet in the last minute) a placement far from its tones is ignored, and only then do four
   tones stop the RTTY bank following the spectrum.
3. **UKHAS RTTY — the RTTY decoder we already have** (`decoders/fsk_decoder.h`: its tone filters, optimal ATC and
   start-bit UART), run as a **bank of candidates** on the measured pair: 50 / 75 / 100 / 150 / 300 / 600 baud × 7 or 8
   data bits × either polarity (24). A 1-stop decoder that waits for each start bit also reads 1.5- and 2-stop signals,
   so stop bits are not searched; nothing is snapped to the teleprinter shifts (balloon transmitters are not on them).
   ★ **The CRC is the judge:** a sentence from ANY candidate whose CRC-16 passes is telemetry; the same sentence from
   several candidates is shown once, and that candidate is "what it is sending" ("300 Bd · 8N1 · 600 Hz").
   Two small changes to `FskDecoder`, neither touching existing uses: `onData` (each good frame's raw bits, for SSDV) and,
   **above 110 baud only**, tone filters a baud wide and the envelope lowpass at 0.6 × baud (the old 167 Hz / 140 Hz
   smeared 300-baud bits together). RTTY AUTO (`rtty_auto.cpp`) is unchanged: its search is the teleprinter one
   (ITA2, 45–75 Bd, snapped shifts) and is judged by frame scores, which suits text but not a CRC-checked sentence.
4. **Horus 4-FSK** (`balloon_horus.h`): the spectrum PLACES the four tones; each tone is then held by its own frequency
   loop (the phase turn between the two halves of every symbol that was that tone, 1/20 of the error a symbol, kept within
   40 Hz of the placement) — the spectrum's 100-baud peaks are broad and wander by tens of hertz, and re-placing on every
   spectrum lost packets. Once Horus has decoded, the loops run only inside a frame (the 600 bits after a UW): the RTTY a
   payload sends between its packets would otherwise pull them (seen: a 270 Hz payload held at 254 Hz). A bank of four one-symbol integrators (**NEON / SSE2, one tone a lane**, scalar bit-equal), the
   symbol clock from where the strongest tone's share of the energy peaks, **soft bits** scaled by the average symbol,
   the UW `$$` in either tone order (reversed tones invert every bit) with up to 2 of 16 bits wrong, then decode as v1
   (45 bytes) and v2 (65 bytes) — Golay decoded **Chase-style from the soft bits** (the 6 least sure bits tried both ways).
5. **SSDV** (`balloon_ssdv.h`): the 8-bit candidates' raw bytes go to a 256-byte window tried whenever it starts
   `0x55 0x66/0x67`: CRC-32, and if it fails Reed–Solomon then the CRC again. Packets are kept per picture (callsign +
   image ID) and the JPEG is rebuilt from what has arrived (below).
6. **History:** the last 6 s of 12 kHz audio are kept, and a decoder the spectrum starts (the RTTY bank on new tones,
   Horus on its first placement) first reads them — otherwise the first sentence, packet or picture packet after a quiet
   carrier was lost while the spectrum settled.

### Believing a report

- UKHAS: only the **CRC-16** form; the old two-digit XOR checksum is refused (one in 256 garbled lines would pass it, and a
  position is what people chase a balloon by). Callsign `[A-Za-z0-9_-/]`, ≤ 300 characters, `0,0` is "no fix".
- Horus: CRC-16 **and** possible fields (h < 24, m, s < 60, |lat| ≤ 90, |lon| ≤ 180, finite floats).
- SSDV: CRC-32 (after RS when needed), sensible header (sizes ≤ 2048 px, offset inside the payload, MCU inside the picture).

### Building the picture

SSDV's payload is a baseline JPEG's entropy-coded MCUs with no markers, no byte stuffing, fixed tables, the first MCU that
starts in each packet on a byte with its DC values from 0. Our decoder writes a real JPEG: the headers (JFIF, the two
quantisation tables for the packet's quality level, SOF0 with the subsampling, the four Annex K Huffman tables, SOS), then
every MCU in order — a run of consecutive packets is one bit stream; each block's Huffman symbols are copied as they are
except the **DC, re-differenced** against what was written (so a packet's "from 0" DC lands right); a lost MCU is an
"empty" one (DC difference 0, end of block — the previous block's level, no detail); an MCU cut short by a lost packet is
ended cleanly; byte stuffing added; EOI. Rebuilt at most every 4 s while a picture grows, and at once when it completes.

## The formats, and where each fact came from (the house rule)

★ **Published standards first; GPL / LGPL code may be READ, never copied.** Everything here is our own code.

| Fact | Source |
|---|---|
| UKHAS sentence, CRC-16-CCITT (0x1021, 0xFFFF) | UKHAS wiki (communication:protocol) |
| Horus v1 / v2 field layout (sizes, order, little-endian, battery 0–255 = 0–5 V) | horusdemodlib README / decoder field list |
| Horus coding: UW `$$` (2 bytes), systematic payload then packed Golay check bits, the short last group coded as `value << 1`, interleaver b = 337 (v1) / 389 (v2) with LSB-first bit indexing, the 0x4A80 additive scrambler (taps bits 0 and 1, fed into bit 14), 4-FSK natural binary MSB first, the lowest tone symbol 0 | horusdemodlib `horus_l2.c`, `golay23.c`, `horus_api.c`, `fsk.c` (LGPL-2.1) — **read, not copied** |
| Golay(23,12) generator 0xC75 | M17's `vibe_m17_fec.h golayCheck` (the M17 spec's matrix); `test-balloon` proves it equals the 0xC75 long division for all 4096 words |
| SSDV packet layout, RS over bytes 1–223, lost-packet handling, "first MCU byte-aligned, DC from 0" | UKHAS wiki guides:ssdv (the format description) |
| CRC-32 (zlib), RS(255,223) CCSDS conventional basis (field 0x187, roots β^112…143, β = α¹¹), base-40 callsign, the reset MCU's DC rule (first Y block and Cb/Cr only), padding with 1s, the empty-block filler | the reference `ssdv` program (GPL-3, Codeberg fsphil/ssdv) — **read for the conventions, not copied** |
| Huffman tables | ITU-T T.81 Annex K.3 (the standard's) |
| Quantisation: Annex K's two tables with every value rounded up to even, except chrominance zig-zag 3 and 5 = 22; quality level 0–7 = IJG quality 1, 14, 29, 43, 50, 71, 86, 100 with IJG's scaling | the reference program's tables, **expressed as a rule over Annex K** (see decisions) |

## Threading, CPU and memory

- Runs on the decoder host's thread (its bounded 4 s queue: a host that falls behind loses audio, the listener never
  waits). One host per listener on a per-VFO radio, one on a shared dial.
- **CPU (M5 Pro):** 103 s of UKHAS + Horus audio with the whole AUTO bank searching (24 RTTY candidates + Horus) in
  0.51 s — **0.5 % of one core**. A Pi 2 is perhaps 10–20× slower: ~5–10 % of a core, **not measured** — the
  `dec_balloon` benchmark row measures it on the box and sets the default. The burst when a bank starts (6 s of history
  through 24 decoders) is the heaviest moment.
- **Memory per running decoder:** 288 KB history, ~100 KB spectrum and filters, 24 small RTTY decoders, ≤ 3 pictures in
  progress × ≤ 2048 packets × 220 B (≤ 1.4 MB, typically a few tens of KB), ≤ 12 payloads. Every structure is bounded.

## Tests

| Suite | What | Result |
|---|---|---|
| `test-balloon` (vibeserver/, run-tests.sh) | CRC check values; UKHAS parse/refuse/framer; Golay (all 4096, every ≤3-error pattern, Chase 4); Horus coding bit-exact v1/v2 + 3 errors in every codeword; Horus on air (our 4-FSK: 244/270/400 Hz spacing, low/high in the passband, reversed tones, 0.3 % fast clock, manual pins, threshold); UKHAS on air (50/100/300/600 Bd, 7/8 bits, 1/2 stop, 350–850 Hz, reversed, high in the passband; each pinned by hand; a wrong pin decodes nothing); SSDV (callsigns, RS 16 errors mended / 17 never mis-corrected, every subsampling mode + no-FEC exact, lost packet → empty MCUs only there, bounds); SSDV over 600 Bd RTTY exact; **5 minutes of noise and ~3 minutes of ordinary RTTY (Baudot, 8-bit text full of `$` and `*`, random bytes with sync bytes) — not one report or picture packet**; no Horus reported on fast RTTY; SIMD = scalar bit for bit; cost | **128 / 0** |
| `test-balloon wav <file> [rate]` | a recording through the receiver (report) | see Verified |
| `test-server-balloon.mjs` | the real server, fake rtl_tcp, `test-balloon clip` on 434.200 MHz: one-pipeline and range-locked radio — both UKHAS sentences and both Horus packets field for field, the SSDV picture complete as a whole JPEG, text lines, states; pinned 50 Bd → Horus only; 300 Bd 8-bit + `horus:false` → UKHAS only; detach stops; the gallery keeps the picture as image/jpeg and the reports as runs; blocked → false | **33 / 0** |
| `test-web-balloon.mjs` | the binary's embedded page, headless Edge `--mute-audio`: UHF tab, BALLOONS under Amateur, pins on AUTO, rows, the picture decoded as an `<img>` 96 × 64, a pin re-attaches and the rows stay, 390 px, no page exception | **21 / 0** |
| `test_balloon_app.ts` | the shared client half (incl. the rows kept across a re-attach) and both clients' wiring, the tour / tutorial / About copy | **51 / 0** |

`test-bench-decoders` runs the new `dec_balloon` row: **0.33 % of an M5 Pro core**, decoding its signal, with a defaults
rule. Also re-run and passing after the change: test-radio-caps (243), test-decoder-reach (76), test-decoder-hosts,
test-bench-decoders, test-rtty-shift, test-rtty-skimmer, test-bgdecode, test-gallery-store, test-navtex-fec,
test-bench-defaults, check-setup-page, test_decoder_bands (updated: UHF now holds BALLOONS and PAGING), test_ncdxf_app,
test_known_signals, test_m17_app, test_sondes_app.

Horus threshold on our synthetic signal (20 v2 packets, white noise, SNR in 3 kHz): 20/20 down to −2 dB, 18/20 at −4 dB
(Eb/N0 ≈ 7.8 dB), **0 at −6 dB** — the cliff is the tone *finding* (peaks 9 dB over the noise), not the FEC. UKHAS
300 Bd 8N2 600 Hz: 8/8 down to +6 dB in 3 kHz, 5/8 at +3 dB.

## What is verified, and what is not

**Verified against the real thing** — Project Horus's own sample recordings (horusdemodlib `samples/`, downloaded to a
scratch folder for this check and **not committed**; read by our own decoder only):

| Recording | Result |
|---|---|
| `horus_binary_ebno_4.5db.wav` (v1, 8 kHz, Eb/N0 4.5 dB) | 5 frames pass CRC (one payload repeated: shown once), Golay fixing ~17 bits a frame |
| `horus_v2_100bd.raw` (v2, 8 kHz, 26 s) | 9 of its 10 packets (the tenth is cut off by the end of the file); 6 with the UW at 1 bit wrong |
| `rtty_8n2.wav` (300 Bd 8N2, 900 Hz) | all 4 sentences |
| `rtty_7n1.wav` (100 Bd 7N1) | its one sentence, sent 4 times, each read clean (shown once) |
| `rtty_7n2.wav` | 9 sentences seen, **every one with character errors, none passes its CRC** — a weak recording; nothing shown |

So the Horus Golay / interleaver / scrambler / CRC conventions and the v1/v2 layouts match real frames, and the UKHAS path
reads real sentences.

**Verified only synthetically (our transmitter into our receiver):**
- **SSDV end to end.** No real SSDV packet data was found to test against, and running the reference `ssdv` program as an
  oracle was not done (building and running downloaded code was refused in this session). Our packets → our JPEG →
  macOS's own JPEG decoder (`sips`) opens it and the coefficients read back exactly; but **whether real SSDV packets
  decode to the right picture rests on our reading of the format** (the RS basis, the DC rule, the quantisation tables).
  A wrong RS convention would show as every packet refused (never a wrong picture: the CRC-32 still has to pass); a wrong
  table as a picture with wrong contrast/colour; a wrong DC rule as blocks shifted in brightness at packet starts.
- Horus at 50 / 300 baud and spacings other than 244 / 270 / 400 Hz; Horus v3 is not decoded at all.
- The CPU on a Pi 2; the benchmark row has not been run on a real box.
- The web end-to-end test ran on this Mac only; the app's BalloonView has not been seen on a device (tsc only).

## Not built

- **Off-air decoding** — needs a 70 cm receiver and a flight (or the Pi 500's RTL on 434 MHz when a UK payload flies).
- **SondeHub amateur upload** and **the map** (payload positions on the SHIPPING/aircraft-style map, a track). The survey
  says to share them with the radiosonde work; RS41 has neither yet either.
- **Horus payload names** (the payload-ID list is a file Project Horus publishes online; we show `HORUS-<id>`) and its
  **custom fields** (shown as hex only).
- **Horus v3** (ASN.1-encoded, 32–128 bytes, newer flights) and the old **LDPC** Horus modes.
- **SSDV erasure padding** (the wiki's "insert padding bytes when a byte is dropped" — a dropped RTTY byte loses the packet).
- **SSDV from satellites** (§6.4) — the SSDV layer is reusable; no satellite framing was built.
- **R36S handheld view** — its decoder views are a separate C++ UI (handheld-ui/); not a small addition, so not done.
- **Joining a running picture** — a second window (a mirror, a reconnect) gets the next draw (≤ 4 s), not a replay.

## Decisions for Stuart

1. **The SSDV quantisation tables.** Real SSDV pictures are quantised against the reference program's two tables, which
   are not the JPEG standard's. We wrote them as a rule over Annex K (every value rounded up to even; chrominance zig-zag
   3 and 5 = 22) — that rule *is* those numbers. They are format constants a decoder must use to get the levels right,
   but they came from reading GPL code. Keep (my recommendation: they are interoperability data, like a CRC polynomial),
   or switch to plain Annex K (pictures a few per cent off in contrast)?
2. **An interop check against the reference programs.** Building and running horusdemodlib and `ssdv` locally (never
   committed) would turn "synthetic only" into "matches the reference" for SSDV. It needs your permission in this setup.
3. **UW tolerance 2 bits** (horusdemodlib uses 1). It found 9 of 10 packets on their v2 sample instead of 6; minutes of
   noise still give nothing. OK?
4. **SondeHub upload** — an owner opt-in, as for RS41? It is the community's map for amateur payloads, and receivers
   uploading is what makes a chase work. Not built; your call (shared with the RS41 decision).
5. **Payload names** — fetch Project Horus's payload-ID list (a small JSON on GitHub) on the server once a day, or keep
   `HORUS-<id>`?
6. **Benchmark tier** — is the balloon switch's default worth having below green at all? It costs ~0.5 % of an M5 core.
