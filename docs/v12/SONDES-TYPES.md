# Radiosondes beyond the RS41 (V12): Graw DFM-06 / 09 / 17 and Meteomodem M10 / M10+ / M20 in the SONDES mode

Branch `v12-sondes2` (from `v12-experimental` 3de409c9a). Written 2026-11-19. Read `RS41.md` first: this work adds
types to **the same** SONDES mode, its band scan, its sonde table, its map, list and reports. There is no second mode.

> ★★★ **NOTHING HAS BEEN DECODED OFF AIR.** Every DFM, M10 and M20 frame this code has read came from our own
> transmitters (`vibe_sonde_tx.h`), built from the same write-ups as the decoders. A misunderstanding shared by both
> passes every test here. The first real capture of each type is the next step (RESUME HERE).

| Part | Code | Test | Status |
|---|---|---|---|
| One Manchester FSK demodulator (DFM 2500 chips/s, M10/M20 ≈ 9600) | `vibe_sonde_manch.h` | `test-sondes-types` §3–5 | Works on our transmitters |
| DFM: Hamming(8,4), interleaving, CONF / DAT packets, serial, epochs, temperature | `vibe_sonde_dfm.h` | §1, §3 | Self-consistent; **unverified on air** |
| M10 / M10+ / M20: frames, the 16-bit check, serials, GPS | `vibe_sonde_mxx.h` | §2, §3 | Self-consistent; **unverified on air** |
| The SIMD discriminator and scan power, moved out of `vibe_rs41_rx.h` unchanged | `vibe_sonde_disc.h` | `test-rs41` (unchanged 124/0) | |
| AUTO type per channel, the wide scan pass | `vibe_rs41_rx.h` (`Receiver`) | §4, §5 | |
| Rows keyed by (type, serial); `kind`, `model`, `calOf` in the JSON | `vibe_rs41_service.h` | §4, `test-server-sondes.mjs` | |
| Our transmitters | `vibe_sonde_tx.h` | — | |
| Web / app / R36S: the type in front of the serial, calibration per type | `src/services/sondes.ts`, `handheld-ui/src/map_modes.h` | `test_sondes_app.ts` | R36S syntax-checked only |

## Sources and the house rule

- **Listener documentation only** (no maker publishes these formats). The GPL demodulators by rs1729 were **read for
  facts** — frame layout, field offsets, scalings, the check's definition — and nothing was copied: the demodulator,
  the frame code, the serial assembly, the state machines and the thermistor fit are our own. No comparison with any
  other decoder is made or implied. radiosonde_auto_rx was not opened (not needed for facts beyond the above).
- **Public recordings: none obtained.** SondeHub / auto_rx sample sets were not downloaded (licence terms not checked
  this run, and no time left to do it properly). Nothing large is in git; `~/VibeSDR-test-audio/sondes/` does not exist yet.

### The facts used — all unverified on air

Where one is wrong the symptom is the same as the RS41's: **real frames are refused** (a Hamming block or the check
fails, or the serial never assembles), never a wrong sonde shown. `test-sondes-types --cu8 file.u8 rate centreHz`
prints every frame of every type in a real capture, and the channels with the type each locked to.

**DFM** (`vibe_sonde_dfm.h`)

| Fact | Used as |
|---|---|
| FSK, 2500 chips/s, Manchester "01" = 1; frames back to back (280 bits ≈ 0.224 s) | the demodulator (deviation unknown: tested ±1.5 … ±3.5 kHz) |
| Header raw chips `10011010100110010101101001010101` = bits `0x45CF` | sync (≤ 3 chips wrong of 32, either polarity) — §1 checks the two agree |
| CONF 7 + DAT1 13 + DAT2 13 codewords, block-interleaved (sent bit L·j+i = bit j of codeword i) | the decoder |
| Extended Hamming(8,4): data bits 0–3 (MSB first), parity p4 = d1⊕d2⊕d3, p5 = d0⊕d2⊕d3, p6 = d0⊕d1⊕d3, p7 = d0⊕d1⊕d2 | one error corrected, two refused (§1: all 128 / all 448) |
| DAT: 32-bit value, 16-bit value, id nibble. Packet 0's mode byte (bits 16–23) picks the layout | GPS |
| mode ≤ 2: 1 ms-in-minute, 2 lat 1e-7° + speed cm/s, 3 lon + direction 0.01°, 4 altitude cm + climb cm/s, 8 date + sats; mode 3/4: 0 ms + speed, 1 lat + direction, 2 lon + climb, 3 altitude | position, velocity, time |
| CONF: id nibble + 24-bit value; channels 0–8 measurements as a 24-bit float (exponent nibble, 20-bit mantissa) | calibration |
| Serial: channel 0xA–0xD, then nibble 0xC, a 16-bit half and which half; 0xA DFM-09 (≥ 23 000 000: DFM-17), 0xB DFM-17, 0xC DFM-09P, 0xD DFM-17P. DFM-06: a "null" channel 5 = 0x5A/0x5B + zeros, the serial = channel 6's six digits | serial, model |
| Temperature: R = Rf·(f₀ − f₃)/f₄, Rf 220 kΩ (DFM-06, DFM-09) / 332 kΩ (DFM-17); thermistor EPCOS B57540G0502 (R25 5 kΩ) | the temperature |

**M10 / M10+ / M20** (`vibe_sonde_mxx.h`): ≈ 9615 / 9600 chips/s FSK, Manchester then differential (a bit = 1 when its
Manchester bit equals the one before); the same 32-chip raw header; byte 0 = length − 1 (0x64 / 0x45), byte 1 the
type (0x9F M10, 0xAF M10+, 0x20 M20); the last two bytes a 16-bit check of all before (its documented linear update —
§2 checks it is linear and catches every single-bit error); the offsets and scalings in the file's header. **Neither
sends ECEF** (the brief expected it of the M10): both send latitude / longitude.

### Rules kept from the RS41

- **A sonde exists only when its own checks prove it.** DFM: every codeword of a CONF block clean or one bit corrected
  (≤ 1 correction), of a DAT block ≤ 3, no double error anywhere; the serial assembled from both halves — adopted at
  once when both halves needed **no** correction, else only when the same serial assembles twice. A position only from
  the packets of **one** GPS epoch (an epoch opens with packet 0), plausible (|lat| ≤ 90, |lon| ≤ 180, −500…60 000 m).
  M10 / M20: length + type bytes + the 16-bit check + a plausible serial.
- **Never a guessed temperature.**
  - DFM: only once channels 0–5 have all arrived clean (`cal` 6 of `calOf` 6), only for a type whose reference resistor is
    documented (DFM-06, DFM-09, DFM-17 as 0xB), and only −100…+60 °C. The curve is **our own** Steinhart–Hart fit of the
    thermistor maker's published R/T table (±0.3 °C at its points, §1); below −55 °C (the table's end) it extrapolates —
    stratospheric readings are the least trustworthy part. DFM-09P / DFM-17P / 0xA ≥ 23 000 000: `calOf` 0, no temperature.
  - M10 / M20: **not decoded** (`calOf` 0, the clients say "temperature not decoded for this type"). The write-ups mark
    the thermistor's maker and curve as uncertain; that is a guess by Stuart's rule. Humidity / pressure: none, any type.
  - The clients drop a `temp` sent beside an incomplete calibration whatever the server sends (`test_sondes_app`).

## Architecture — AUTO type inside the existing receiver

1. **The scan** (`Receiver::scan_`): the RS41's 10 kHz-box pass (RS41, DFM) and a second **20 kHz-box pass** for the
   M10 / M20, whose ≈ ±10 kHz spread the narrow pass rejects as a "broad block". The same three tests, scaled (carrier:
   middle bins < 30 % in the wide pass; broad block: each flank < 40 %). Candidates within 6 / 10 kHz are one.
2. **Each channel runs every type until one proves itself**: the RS41 demodulator and the DFM on the narrow slice
   (32 bins, 18.75 kS/s at 2.4 MS/s), the M10 / M20 on a **wide slice** (64 bins, 37.5 kS/s, the listeners' roll-off —
   the flatter taperDiv 16 was 3 dB worse, `SONDE_WIDE_TAPER`). The first frame that passes **its type's own** checks
   **locks** the channel: the other demodulators stop and their frames are ignored (§4: 0 frames of another type).
   30 s without a frame of the locked type and the channel listens to all again. The channels line carries `kind`.
3. **The Manchester demodulator** (`vibe_sonde_manch.h`): the RS41's discriminator (SIMD) → 8 samples a chip → a
   one-chip box integrator → the raw header by shift register + popcount at eight phases (≤ 3 of 32 wrong, either
   polarity) → the best phase by soft correlation → amplitude / offset / SNR by least squares on the header → chips with
   a **PI timing loop** (the M10 and M20 differ by 0.16 % in chip rate; one demodulator at 9608 follows either, and
   ±400 ppm beyond, §3) → Manchester pairs → soft bits to the type's sink, which says how many bits it wants (an M10 /
   M20 frame's length comes from its first byte).
4. **The table** (`Tracker`): one row per **(type, serial)**; RS41 rows unchanged. JSON per sonde adds `kind`
   ("RS41" / "DFM" / "M10" / "M20"), `model` where known ("DFM-09", "DFM-17", "DFM-09P", "DFM-17P", "DFM-06", "M10+"),
   and `calOf` (51 / 6 / 0). A DFM's or M10's UTC time from its GPS fills `gpsTime`.

**Memory**: per channel + ≈ 2 × 4096-float rings and ≈ 1100 soft bits per Manchester demodulator (≈ 40 KB more a
channel); six channels ≈ 0.25 MB more. The table's bounds are the RS41's (64 rows × 400 points).

**SIMD**: the hot loop is the discriminator, shared with the RS41 (NEON / SSE2 / scalar, `DECODER-SIMD-AUDIT.md`). The
box integrator (8 adds a sample at ≤ 77 kS/s a channel) and the scan are scalar — measured below, not worth vectors.
`VIBE_FORCE_SCALAR` passes the same suite (76 / 0). SSE2 (x86) not built this run.

## Wired

- **Server**: nothing new to configure — the SONDES mode, its bands, reach rule, owner switch and gain rule are the
  RS41's; the report gains `kind` / `model` / `calOf` per sonde and `kind` per channel.
- **Web / app**: `sondeLine` puts the type before the serial ("DFM-09 19012345 · 5,000 m · +5.0 m/s"), so the list, the
  map cards and the app's TrackLiveList all show it; `sondeTemp` uses `calOf` ("not yet calibrated (4 of 6)",
  "temperature not decoded for this type"). An older server's report (no `calOf`) reads as an RS41's 51.
- **R36S**: `mapSdLine` / `mapSdTemp` the same; the canned feed's third sonde is a DFM-09 with its temperature.
  `-fsyntax-only` on `main.cpp` in `vibe-hh-map:trixie` (arm64): OK. Not run on the handheld, no golden.
- **Benchmark**: `dec_dfm` (three DFM-09s, 12 s clip so serials complete) and `dec_m10` (two M10 + one M20), each a
  2 MHz band at 2.4 MS/s through the service in real time, scored by the hottest thread; rule beside `dec_rs41`
  (`sondes`, not primary — RS41 keeps setting the default); `Need::Sondes`; setup-page names. **The rows compiled into
  vibeserver; the full benchmark and `test-bench-decoders` were not run** (run-tests.sh builds that suite — coordinator).
- **Copy**: About V12 note ("MORE KINDS OF RADIOSONDE"), setup page + ServerModeScreen switch label, ModeSelector,
  MapOverlay title / legend, web mode menu / mobile row / tutorial, the empty list's words. `sdrTour` has no SONDES card
  (checked); `website/index.html` does not mention sondes.

## Results (all synthetic, this Mac — M5 Pro)

`test-sondes-types` **76 / 0** (`--quick` 76 / 0, `VIBE_FORCE_SCALAR` 76 / 0). 2.4 MS/s, SNR = signal over the noise
in 10 kHz (as `test-rs41`), 20 s; "positions" = correct positions decoded (the first seconds go to the scan and, for a
DFM, to assembling the serial):

| SNR | 4 dB | 6 dB | 8 dB | 10 dB | 12 dB | 15 dB | 20 dB | 30 dB |
|---|---|---|---|---|---|---|---|---|
| DFM (of ≈ 19 epochs) | 0 | 12 | 15 | 15 | 15 | 15 | 15 | 15 |
| M10 (of 20 frames) | 0 | 0 | 0 | 0 | 14 | 18 | 19 | 19 |
| M20 (of 20 frames) | 0 | 0 | 0 | 1 | 16 | 19 | 19 | 19 |
| RS41 for comparison (`RS41.md`) | 0 | 2 | 11/12 | | | | | |

- **Wrong frames: 0** in every run. **Cross-type: 0** — each type alone, 0 frames reported as another; RS41 + 2 DFM +
  M10 + M20 in one band all decoded, none wrong. **Noise** 60 s with three carriers and four random-chip FSK
  look-alikes at 2500 / 9600 / 9615 chips/s: **0 frames**.
- DFM at −812 / +640 kHz and 3 kHz from the centre, ±1.5 / ±3.5 kHz deviation, ±300 ppm, mode 3 + DFM-17, DFM-06: all
  15–16 positions (DFM-06 12), none wrong. M10 / M20 at −700 … +820 kHz, ±3 … ±7 kHz, ±400 ppm: 18–19 each.
- DFM-09 temperature = the transmitter's −21.5 °C within 0.3, never before 6 of 6; DFM-09P: none.
- **The M10 / M20 need ≈ 5 dB more than the RS41** (whole frames from ≈ 12 dB): 808 / 560 bits with no FEC through a
  discriminator at ≈ 6 dB CNR in a 37.5 kHz slice is past FM threshold. A non-coherent two-tone matched filter (the
  deviation measured from the header) is the obvious gain — **not built** (RESUME HERE 3).
- Server end to end: `test-server-sondes.mjs` with `VIBE_SONDES_TYPES=1` and the `test-sondes-types --write-clip` clip
  (DFM-09 402.500, M10 403.600, M20 403.900 MHz) **8 / 0** on the real vibeserver: three sondes, kinds and model right,
  the DFM's −18.0 °C with cal 6 of 6, the M10 / M20 without a temperature, each channel locked to its type.
- Unchanged: `test-rs41` 124 / 0, `test-rs41-service` 42 / 0, `test-server-sondes.mjs` 25 / 0, `test-web-sondes.mjs`
  20 / 0 (desktop + 390 px, the list now reads "RS41 S3310411 · …"), `test_sondes_app.ts` all passed (+6 checks).
- **CPU** (one thread, the Receiver alone, M5 Pro): three of a type in a 2 MHz band at 2.4 MS/s — RS41 0.0049 s, DFM
  0.0050 s, M10/M20 0.0054 s per capture second; noise alone 0.0040; one of each of four types 0.0057. The cost is the
  front end's FFT, as for the RS41; the extra demodulators until a lock add ≈ 10 %. **Not measured on a Pi 2.**

## Which types fly where (for a live test) — from memory of public sources, ★ CHECK SondeHub's map before planning

- **UK**: the Met Office's upper-air sites (Camborne, Herstmonceux, Watnall, Albemarle, Castor Bay, Lerwick) and Met
  Éireann's Valentia fly **Vaisala RS41**. No routine DFM / M10 / M20 launches in the UK that I know of; MoD ranges may
  fly others occasionally (unverified).
- **France** (Météo-France): **Meteomodem M10, moving to M20** — Trappes (Paris), Brest-Guipavas, Bordeaux, Nîmes, Nancy,
  Ajaccio and others. **Brest-Guipavas and Trappes are the nearest non-RS41 launches to southern England**; a sonde at
  altitude is in line of sight from Cornwall / the south coast.
- **Germany**: the DWD flies RS41; the **Bundeswehr flies Graw DFM-09 / DFM-17** (e.g. Meppen, Idar-Oberstein and other
  military sites) — the nearest DFMs to the Netherlands and Belgium.
- **Elsewhere in Europe**: Graw DFM-17 at some central / eastern European sites; M20 at some sites beyond France; RS41
  at most national weather services (Netherlands, Belgium, Spain, Scandinavia). MeteoSwiss Payerne flies Meteolabor
  SRS-C50 (not decoded).
- **Other types not built**: Meteolabor SRS-C50/C34 (Switzerland), Intermet iMet-4 / iMet-54 (some European users,
  mostly the Americas), Lockheed Martin LMS6 (USA), Meisei iMS-100 / RS-11G (Japan), Windsond (research), MRZ (Russia /
  Ukraine), the retired Vaisala RS92, Graw PS-15 (pilot sondes — its serial form 0x0 after 0x7/0x8 is not accepted here).

## RESUME HERE

1. **A real capture of each type** (the coordinator — no agent touches Stuart's servers): a radio with a UHF aerial and 0
   listeners, ≥ 10 min of 2.4 MS/s `.u8` around a launch, saved under `~/VibeSDR-test-audio/sondes/` (out of git):
   - **M10 / M20**: Brest-Guipavas or Trappes (launch times / frequencies from SondeHub), from the south coast.
   - **DFM**: a Bundeswehr launch, from the Netherlands / Germany (a public recording if one with a clean licence exists).
   Then `test-sondes-types --cu8 file.u8 2400000 <centreHz>`. Channel opened but no frame: a DFM — the Manchester
   polarity / pair alignment, then the interleaver order; an M10 / M20 — the differential rule's starting bit, then the
   check. No sync at all: the chip rate, then the header.
2. **Public recordings**: check the licence of auto_rx's / SondeHub's sample sets and fetch only licence-clean ones.
3. **M10 / M20 sensitivity**: a non-coherent two-tone matched filter in `ManchRx` (measure the deviation from the header)
   to close the ≈ 5 dB gap to the RS41. Keep the 0-false-decode tests.
4. Run `test-bench-decoders` and the server benchmark for `dec_dfm` / `dec_m10`; a Pi 2 cost (qemu method,
   `AVIATION.md §Cost`); an SSE2 build of `test-sondes-types`.
5. The R36S: run the canned feed on the handheld, a golden.

### Decisions for Stuart

- M10 / M20 temperature **not shown** (the documented thermistor is uncertain) — say if a labelled "approximate" reading is wanted.
- DFM temperature from **our fit of the thermistor's datasheet table**, extrapolated below −55 °C — OK, or only within the table?
- A DFM-09 sending id 0xA with a serial ≥ 23 000 000 is labelled DFM-17 **without** a temperature (its resistor is ambiguous).
- The type shown as the first word of the list line (not a separate column) — the same line on every client.
- RS41 keeps setting the SONDES default on a fresh setup; `dec_dfm` / `dec_m10` are shown beside it only.
