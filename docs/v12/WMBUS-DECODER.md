# Wireless M-Bus meters (V12): water, heat, gas and electricity meters in ISM mode

**Status:** BUILT 2026-10-26 on branch `v12-wmbus` (phases 1-2 and most of 3 — see "As built" just below). Designed
2026-10-25; the design text after "As built" is kept as written, with its errata marked there.
**Why:** Stuart wants to see the meters around him, mainly **Sensus** water meters (UK; the iPERL). He expects the
payload to be encrypted and wants the **serial numbers**.
**Related:** `ISM-DECODER.md` (the mode this lives in — read it first), `BACKGROUND-DECODING.md` (ISM profiles),
`DECODER-RESEARCH.md` §4.3 (overridden for meter serials, see "Policy").

```
Water meter · Sensus · 12345678 · v0x68 · encrypted (AES) · every 16 s · signal 24 dB
Cold water meter · Kamstrup · 87654321 · v0x1B · 123.456 m³ · flow 0.012 m³/h · 30 Sep 120.111 m³ · no alarms · every 16 s · signal 18 dB
Heat meter · Diehl · 00451223 · v0x20 · encrypted (AES, KDF) · every 2 min · signal 9 dB
```

---

## As built (2026-10-26)

**Code:** `vibe_wmbus.h` (the decoder), `vibe_ism.h` (`setChanRate`, `setKeepBaseband`, `Burst::bb`, µs smoothing),
`vibe_ism_service.h` (meters in the Tracker, the report, `dedupeMeters`), `vibe_ism_gallery.h` (the throttle, the card's
meter counts), `vibe_ism_bands.h` (868 `needHz` 450 kHz), `local_sdr_shim.cpp` (the 16 KB / 5 s report rule),
`web/client/src/ism.ts` + `src/services/ism.ts` + `IsmPanel.tsx` (METER chip, line, detail), `ism-offline`.
**Tests:** `vibeserver/test-wmbus.cpp` (312 checks with the real recording), `test-ism` (all families also through the
480 kS/s channel), `scripts/test-server-ism.mjs` and `test-web-ism.mjs` (meters on the wire and in headless Edge at
desktop and 390 px — `VIBE_WMBUS_CLIP`, made by `test-wmbus --write-clip`), `scripts/test_ism_app.ts`.

**The ASSUMED items, checked:**
- **3-out-of-6 table — CONFIRMED** against TI's own reference tables for AN067 (SWRA234's encode and decode tables; TI
  licence, not GPL) — all 16 words match §2.1, high nibble first, MSB first.
- **Preambles and syncs — CONFIRMED**: S `n×01 + 000111011010010110`, T `n×01 + 0000111101` (TI AN067 Table 9); C
  `16×01 + 0101010000111101` then `0101010011001101` (frame A) / `0101010000111101` (frame B) (Infineon TDA5340 wM-Bus
  note, Table 2-1). C1 deviation 43-47 kHz, T1 rate 90-110 kchip/s, S ±1.5 % TX / ±2 % RX (same notes).
- **CRC — CONFIRMED** (AN067 §5.4: the polynomial, init 0, final complement; check value 0xC2B7).
- **Frame format B with L > 127** — still ASSUMED (built and tested both ways; no real capture).
- **Maker names** — still ASSUMED: the DLMS FLAG directory is a script-driven form that could not be read; the table holds
  only 16 well-known codes, everything else shows its three letters.
- **ELL 0x8D encryption bits (SN bits 29-31), AFL length byte, the CW extension byte for modes 7/13, the 0xFD VIFE codes**
  — still ASSUMED, marked in the code. 0x8E/0x8F say "extended link layer" and parse no further.
- **The radio ID is the printed serial; ACC steps once per frame; T1 or C1 for the iPERL** — need Stuart's capture.

**Changes from the design, and why:**
- **Errata in §2.3:** the Kamstrup frame's date bytes `40 39` are day 0 (type G: day = bits 0-4); 30 Sep 2026 is
  `5E 39`. The CRCs printed are right for the bytes printed. The decoder refuses a day-0 date and shows the stored value
  as "stored 120.111 m³"; the tests use the corrected frame for the "30 Sep" line.
- **The interval (§4.3) is time over steps, not a median of gaps:** every gap carries two jitters, and the median of 8
  read "every 17 s" for a 16 s meter with ±1 s jitter. Now Σ time ÷ Σ steps over the last 16 samples, strays (> 25 % off
  the median step) left out. Frozen ACC still falls back to the median raw gap.
- **A frame out of two channels:** a T1/C1 burst is ~200 kHz wide and the detector can open a second channel on its
  skirt. The decoder gives up when its carrier is > 90 kHz off the channel centre, and the service keeps the
  better-centred copy of a frame heard twice within 0.2 s.
- **Preambles are searched earliest-first and judged in pairs of spacings** (a data stretch of 0101… — Manchester 0x00,
  NRZ 0x55 — can be longer than a short preamble; unbalanced NRZ data biases the first carrier estimate so single
  spacings alternate short/long).
- **The report-size rule was built** (§5.5 said: only if the number says so): a meter's row is ~480 bytes, 300 meters
  made the once-a-second report 141 KB/s per listener. Past 16 KB the full list goes every 5 s; web and app age "last
  heard" themselves between reports. **The meter cap of 500** is built too.
- **Example §4.2's "Meter · ZRI · …"**: ZRI is in the maker table (Zenner), so the letters example uses an unknown code.
- **The detail** is the row's hover on the web and a TAP (web and app) — touch has no hover.

**Measured (Mac, M-series; Pi 2 not touched):** the meter decoder 0.1 ms a burst (NEON and scalar alike — the SIMD
discriminator is a few % of it; the per-burst cost is the crossings and the chip integrals); 10.2 s of 868 capture at
2.4 MS/s with 50 T1 frames: 2.0 % of one core for the whole ISM front end + decoders. **Pi 2 estimated** at ~10-15× the
Mac per core: ~1-1.5 ms a burst, so a street of 50 meters at 16 s (~3 bursts/s) is ≤ 0.5 % of one Pi 2 core on top of
ISM's own front end (10-20 % estimated, ISM-DECODER.md). RAM: a burst's baseband 8 bytes a sample + the decoder's 16
(a 30 ms T1 frame ≈ 350 KB transient, freed at once; the decoder skips bursts > 250 ms); ~1 KB per listed meter.
**Sensitivity** (Kamstrup frame, 10 tries each, SNR in the 480 kHz channel): T1 100 % to 10 dB, 90 % at 7.1, 80 % at
5.4; C1-A 100 % to 8.4 dB, 70 % at 5.4; S1 100 % to 8.4 dB, 40 % at 5.4. **False meters: 0** — 20 s of noise, two
carriers, a sync-less preamble, 3000 hand-made bursts (noise, and FSK with a true preamble and sync then random chips),
and the Pi 500's real 30 s 868.6 MHz capture (no bursts at all in it — its MW antenna; a no-false-decodes check only).
**No real meter recording exists yet** — the validation waits for a 60 s capture near Stuart's iPERL (§7).

---

## 1. Decisions in one place

| # | Decision | Why |
|---|---|---|
| 1 | **Inside ISM mode, on the existing `868` band** — not a new mode, not a passband decoder. Meters are a new device `kind: "meter"` in the same list, gallery and background profile. | ISM already captures 868.3 and 868.95 together, steps bands, keeps a device list, a day log and a card. A meter is one more device. |
| 2 | **Modes T1, C1 (frame A and B), S1** in phase 1–2. N (169 MHz) optional later. R2/F not planned. | T1 and C1 are what fixed-network and walk-by meters transmit at 868.95; S1 at 868.3 is the older stationary mode. |
| 3 | **Never decrypt. No key entry.** Show what the standard sends in the clear. | Stuart's rule. A later owner-only "my own meter's key" is noted, not designed (§9). |
| 4 | **Serials are shown on every server, public included. No hide setting.** | **Decided by Stuart (2026-10-25):** "a listener could only tie a serial to an address by breaking into a water company's database". |
| 5 | **Not on the Airspy HF+** (ISM is not offered there at all) nor on an Icom. RTL and RSP only. | AGENTS.md: a control that cannot work on this radio is not drawn. |
| 6 | **No change to the ISM wire contract's shape** — meters arrive as devices in the `ism` report with extra fields; old clients still show a sensible line. | The shared-dial contract and every client already speak `ism`. |
| 7 | **The decoder is a sample-domain demodulator**, not the ISM run-length slicer. | A T1/C1 chip is 10 µs; ISM's finest smoothing is 12 µs. See §5. |

### 1.1 Clean room

No GPL or closed decoder source was read for this design, and none may be read while building it. Everything here comes
from the published standards (EN 13757-3, EN 13757-4, EN 13757-7), the OMS specification, chip vendors' application notes
(TI AN067/SWRA234, TI SWRA522E, Microchip's wM-Bus TRX configuration table), the CRC catalogue and the DLMS UA FLAG list.
Where a source left a detail open, the choice is marked **ASSUMED** here and in the code, with what will confirm it.

---

## 2. The physical layer (meter → other)

All four 868 MHz modes are 2-FSK. "Chip" = one symbol on air; a coded data bit costs one or more chips.

| Mode | Direction | Carrier | Tolerance | Deviation | Chip rate | Coding | Data rate | Preamble (chips) | Sync (chips) | Frame |
|---|---|---|---|---|---|---|---|---|---|---|
| **T1** | meter → other, frequent | **868.95 MHz** | ±60 ppm (868.90–869.00) | ±50 kHz (40–80) | **100 kchip/s** (90–110) | **3-out-of-6** | 66.7 kbit/s | n × `01`, n ≥ 19 | `0000111101` (10) | A |
| **C1** | meter → other, compact | **868.95 MHz** | ±60 ppm assumed as T1 | ±45 kHz | **100 kchip/s** | **NRZ** (none) | 100 kbit/s | n × `01`, n = 16 | `0x543D` then `0x54CD` (**A**) or `0x543D` (**B**) | A or B |
| **S1** | meter → other, stationary | **868.30 MHz** | ±60 ppm (868.25–868.35) | ±50 kHz (40–80) | **32.768 kchip/s** | **Manchester** | 16.384 kbit/s | n × `01`, n ≥ 279 (long) | `000111011010010110` (18) | A |
| S1-m / S2 | meter → other short preamble / two-way | 868.30 | as S1 | as S1 | 32.768 k | Manchester | 16.384 k | n ≥ 15 | as S1 | A |
| T2 (other → meter) | collector → meter | 868.30 | ±25 ppm | ±50 kHz | 32.768 k | Manchester | 16.384 k | n ≥ 15 | as S1 | A |
| C2 (other → meter) | collector → meter | 869.525 | — | — | 50 kchip/s | NRZ | 50 k | — | — | A/B |
| N1/N2 | both | 169.40–169.475 MHz, 12.5 kHz channels | — | GFSK / 4-GFSK | 2.4 / 4.8 / 19.2 kbit/s | NRZ | — | — | — | A/B |
| R2 | two-way | 868.03 + n × 0.06 MHz (n = 0…9) | — | ±6 kHz | 4.8 kchip/s | Manchester | 2.4 kbit/s | — | — | A |
| F2 | two-way | 433.82 MHz | — | ±5.5 kHz | 2.4 kchip/s | NRZ | 2.4 kbit/s | — | — | A/B |

Notes the code must honour:

- **T and C share their first 16 chips:** the last 6 preamble chips `010101` plus T's 10-chip sync `0000111101` =
  `0x543D`. A C frame follows it with `01010100` then `11001101` (frame A, `0x54CD`) or `00111101` (frame B, `0x543D`).
  `01010100` cannot occur at the start of a 3-out-of-6 coded T frame (TI SWRA522E), so **one sync search finds both, and
  the next 8 chips say T or C**.
- **The S sync contains `000` and `111`**, which Manchester can never produce, so it cannot be found inside data.
- **Preamble, sync and postamble are not data-coded.** The postamble is 2–4 × `01` and is ignored.
- **Bit order on air is MSB first** for every byte (and, in T mode, the high nibble's 6 chips go first). Multi-byte
  fields inside the frame are **little-endian** (M-field, ID, configuration word, data records).
- **Which tone is a 1** is not something to rely on: try both signs of the discriminator; the sync decides. Same for
  Manchester's convention (`10` = 1 or `01` = 1): try both, the L-field and the first CRC decide. (ISM's slicers already
  work this way.)
- **Priority:** T1 + C1 first (one sync search, 868.95, the modes most current meters use); then S1 (same front end at a
  third of the rate); N only if Stuart wants a 169 MHz band (§9); R2, F2, C2, T2 not decoded as meters (two-way
  readout traffic; T2/S2 frames decode through the S path anyway and are counted, not listed — their C-field is a
  collector's command).

### 2.1 3-out-of-6 (T mode)

Each nibble becomes 6 chips with exactly three 1s, high nibble first, so a byte is 12 chips and a frame is 1.5 × as long
as its bytes. Decode with a 64-entry inverse table; any other 6-chip word is a **code error** (end of the frame, or
damage).

| nibble | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B | C | D | E | F |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| chips | 010110 | 001101 | 001110 | 001011 | 011100 | 011001 | 011010 | 010011 | 101100 | 100101 | 100110 | 100011 | 110100 | 110001 | 110010 | 101001 |
| hex | 16 | 0D | 0E | 0B | 1C | 19 | 1A | 13 | 2C | 25 | 26 | 23 | 34 | 31 | 32 | 29 |

★ **Check the table against EN 13757-4's own table before coding** (it is the one thing in this doc taken from memory of
the standard rather than a document opened today). Two properties the test asserts: every word has exactly three 1s,
and all 16 are distinct. A real T1 capture is the final check: a wrong entry shows up as CRC failures on every frame
that carries that nibble.

### 2.2 Frame formats and the CRC

**CRC:** CRC-16/EN-13757 — polynomial **0x3D65** (x¹⁶+x¹³+x¹²+x¹¹+x¹⁰+x⁸+x⁶+x⁵+x²+1), init **0x0000**, MSB first, not
reflected, final value **complemented (xor 0xFFFF)**, sent big-endian (high byte first). Catalogue check value:
`CRC("123456789") = 0xC2B7` (verified for this doc with a 10-line script).

**Frame format A** (T1, S1, C1-A): the L-field counts every byte after itself **excluding CRCs**.

| Block | Bytes | CRC covers |
|---|---|---|
| 1 | L, C, M (2), A (6) = 10 | block 1 |
| 2 | CI + 15 data, or CI + ((L − 9) mod 16) − 1 if shorter | block 2 |
| 3… | 16 data each; the last ((L − 9) mod 16) | its own block |

Bytes on air (after L-field rules) = `1 + L + 2 × (1 + ceil((L − 9) / 16))`. Block 1's CRC proves the header on its
own, so **a frame whose later block fails still identifies the meter** (see §4.4).

**Frame format B** (C1-B): the L-field counts every byte after itself **including CRCs**.

| Block | Bytes | CRC covers |
|---|---|---|
| 1 | L, C, M, A = 10, **no CRC** | — |
| 2 | CI + data, at most 118 bytes **including** its 2 CRC bytes | blocks 1 + 2 |
| 3 (only if L > 127) | the rest, ending in 2 CRC bytes | block 3 only |

So if `L ≤ 127` the CRC is the last two bytes and covers L…(end − 2); otherwise the first CRC sits at frame bytes
126–127 (counting L as byte 0) covering bytes 0–125, and the second at the end covering byte 128 to the end − 2.
**ASSUMED** from the standard's block sizes (10 + 118 = 128): `test-wmbus` builds both shapes; a real C1-B capture with
L > 127 confirms block 3 (meters rarely send one).

### 2.3 Worked frames (the tests' vectors — CRCs computed for this doc)

Encrypted, Sensus water meter, format A (T1 or C1-A), ID 12345678, version 0x01, CI 0x7A, mode 5 with 2 encrypted
blocks, 32 placeholder payload bytes `A0…BF`:

```
2E 44 AE 4C 78 56 34 12 01 07 9C F2
7A 2A 00 20 05 A0 A1 A2 A3 A4 A5 A6 A7 A8 A9 AA 5B 94
AB AC AD AE AF B0 B1 B2 B3 B4 B5 B6 B7 B8 B9 BA 2F 4B
BB BC BD BE BF 6F 56
→ "Water meter · Sensus · 12345678 · v0x01 · encrypted (AES)" (access number 42 in the detail)
```

Unencrypted, Kamstrup (KAM = 0x2C2D) cold-water meter, ID 87654321, version 0x1B, format A:

```
28 44 2D 2C 21 43 65 87 1B 16 ED 2F
7A 0B 00 00 00 0C 13 56 34 12 00 0B 3B 12 00 00 C6 5A
02 FD 17 00 00 42 6C 40 39 4C 13 11 01 12 00 16 A1
→ records: volume 123.456 m³ · flow 0.012 m³/h · error flags 0 · 30 Sep 2026 (storage 1) · 120.111 m³ (storage 1)
```

The same as format B: `2A 44 2D 2C 21 43 65 87 1B 16 7A 0B 00 00 00 0C 13 56 34 12 00 0B 3B 12 00 00 02 FD 17 00 00 42
6C 40 39 4C 13 11 01 12 00 61 2F`.

(The Kamstrup frame is synthetic — no claim that that maker's meters send exactly these records.)

---

## 3. The link and transport layers: what is in the clear

Every field below is sent **in the clear in every security mode**: encryption covers only the application data after
the transport header (or, with an encrypting ELL, everything after the ELL — §3.6).

### 3.1 C-field (1 byte)

| C | Meaning | Shown as |
|---|---|---|
| 0x44 | SND-NR — a meter's spontaneous send, no reply expected | the normal case: a reading |
| 0x46 | SND-IR — installation request | "installation" |
| 0x47 / 0x48 | ACC-NR / ACC-DMD — access request / demand | "access request" |
| 0x08 / 0x18 / 0x28 / 0x38 | RSP-UD — a meter answering a collector | a reading |
| 0x40, 0x53, 0x73, 0x5A, 0x7A, 0x5B, 0x7B, 0x00 … | collector → meter commands, ACK | counted, not listed (not a meter talking) |

### 3.2 M-field (2 bytes, little-endian) — the manufacturer

Three letters packed into 15 bits: `M = (c1 − 64) × 1024 + (c2 − 64) × 32 + (c3 − 64)`; decode
`c1 = ((M >> 10) & 31) + 64`, `c2 = ((M >> 5) & 31) + 64`, `c3 = (M & 31) + 64`. Bit 15 set = a "soft" (not FLAG-registered)
code — show the letters anyway. **`SEN` (Sensus) = 0x4CAE → on air `AE 4C`.**

The name shown comes from a small table built **from the DLMS UA FLAG ID list** (public, flag-id list at dlms.com); each
entry is checked against that list when typed in. Unknown codes show their three letters ("Meter · ZRI · …"). Start with
the makers likely in the UK and Europe: SEN Sensus, KAM Kamstrup, ITW/ITR Itron, ELS Elster, HYD / DME Diehl, ZRI Zenner,
APA Apator, TCH Techem, IST ista, QDS Qundis, EFE Engelmann, BMT B Meters, MAD Maddalena, SON Sontex, LUG/LGB
Landis+Gyr — ★ each letter code **ASSUMED until checked against the FLAG list**; a wrong name is worse than three letters.

### 3.3 A-field (6 bytes) — serial, version, type

| Bytes | Field | Rule |
|---|---|---|
| 0–3 | **Identification number** | 8 BCD digits, **least significant byte first**: `78 56 34 12` → **12345678**. Print nibble by nibble; a non-BCD nibble (A–F, some makers do this) is printed as its hex digit, never rejected. |
| 4 | Version | shown as `v0x68` (it identifies firmware/variant, not a serial) |
| 5 | Device type | the medium — table below |

**The ID is the serial.** ★ **ASSUMED:** on most meters the 8-digit radio ID is the printed serial or its last 8 digits
(Sensus often prints a longer number with a year/batch prefix). Check against Stuart's iPERL when one is heard.

**Device types** (EN 13757-3 / OMS):

| Code | Shown | Code | Shown |
|---|---|---|---|
| 0x00 | Meter (other) | 0x0D | Heat / cooling meter |
| 0x01 | Oil meter | 0x0E | Bus / system component |
| 0x02 | **Electricity meter** | 0x0F | Meter (unknown medium) |
| 0x03 | **Gas meter** | 0x15 | Hot water meter (≥ 90 °C) |
| 0x04 | **Heat meter** (outlet) | 0x16 | **Cold water meter** |
| 0x05 | Steam meter | 0x17 | Hot/cold water meter |
| 0x06 | **Warm water meter** (30–90 °C) | 0x18 | Pressure meter |
| 0x07 | **Water meter** | 0x1A | Smoke detector |
| 0x08 | **Heat cost allocator** | 0x1B | Room sensor |
| 0x09 | Compressed air meter | 0x1C | Gas detector |
| 0x0A | Cooling meter (outlet) | 0x21 | Valve |
| 0x0B | Cooling meter (inlet) | 0x25 | Display unit |
| 0x0C | Heat meter (inlet) | 0x28 | Waste water meter |
| 0x31–0x33 | OMS collector / repeater | 0x36, 0x37 | Radio converter |

Anything else: "Meter (type 0xNN)". The `kind` stays `meter` for all of them (one filter chip); a smoke detector or room
sensor still appears — they are OMS devices like the meters.

### 3.4 CI-field (1 byte) — what follows

| CI | Header that follows | Notes |
|---|---|---|
| **0x7A** | **short** transport header: ACC, STS, CW (4 bytes) | the common meter frame; the A-field is the meter |
| **0x72** | **long** header: ID (4), M (2), version, type, ACC, STS, CW (12 bytes) | the meter's own address is HERE; the link-layer A-field is then a repeater/radio converter → show the long header's address, "via KAM 00123456" |
| 0x78 | none (no encryption possible) | records follow at once |
| 0x79 | none, **compact** frame (records without DIF/VIF, needs the format signature from an earlier full frame) | phase 3; until then "compact frame" |
| 0x8C | ELL short: CC, ACC | then another CI follows (in the clear) |
| 0x8D | ELL long: CC, ACC, session number SN (4), payload CRC (2) | if SN's encryption bits say AES-CTR, **everything after is encrypted**, the next CI included |
| 0x8E / 0x8F | ELL with an address (short / long) | as above, plus a 6-byte address |
| 0x90 | AFL — authentication and fragmentation | "authenticated"; then the next CI |
| 0xA0–0xB7 | manufacturer-specific | header unknown: show the link-layer fields only, "manufacturer format 0xA3" |
| others | — | "CI 0xNN", link-layer fields only |

★ The ELL/AFL field layouts beyond the CI byte are **ASSUMED** from EN 13757-4/-7 as summarised here; phase 3 confirms
them against the standard's tables before parsing more than "skip N bytes, then CI".

### 3.5 The transport header's fields

- **ACC — access number** (1 byte): counts up by one per transmission on most meters (wraps at 255). Used for the
  interval and the reception rate (§4.3). Shown in the detail, not the line.
- **STS — status** (1 byte): bits 0–1 application status (00 OK, 01 busy, 10 application error, 11 abnormal/alarm),
  bit 2 **power low**, bit 3 **permanent error**, bit 4 **temporary error**, bits 5–7 manufacturer-specific. Shown as
  "battery low", "permanent error", "error", "alarm" or nothing.
- **CW — configuration word** (2 bytes, little-endian): **security mode = bits 8–12**. For modes 5 and 7, **bits 4–7 = the
  number of encrypted 16-byte blocks**. The remaining flags (bidirectional, accessibility, synchronous, content, hop
  count) are carried in the JSON as `cw` and not interpreted in phase 1 — ★ their bit positions to be taken from
  EN 13757-7's table when they are wanted. Mode 7 and 13 add a **configuration-field extension** byte after CW
  (**ASSUMED**: one byte; skip it, it holds the KDF/key selection).

| Security mode | Shown | Meaning |
|---|---|---|
| 0 | "not encrypted" | records follow in the clear |
| 2, 3 | "encrypted (DES)" | legacy, deprecated |
| 4 | "encrypted (AES, mode 4)" | legacy |
| **5** | **"encrypted (AES)"** | AES-128-CBC, IV from the meter's address and ACC — the common OMS mode |
| **7** | **"encrypted (AES, KDF)"** | AES-128-CBC with an ephemeral key derived per message (OMS 4) |
| 8, 9, 10 | "encrypted (mode N)" | AES variants of EN 13757-7 |
| **13** | **"encrypted (TLS)"** | TLS 1.2 session (OMS 4, two-way) |
| ELL with AES-CTR | "encrypted (AES, link layer)" | §3.4 |
| anything else | "security mode N" | |

### 3.6 What a listener sees

| Encrypted (the expected case for the iPERL) | Not encrypted (mode 0, or CI 0x78) |
|---|---|
| manufacturer, **serial**, device type, version | all of the left, plus the data records (§3.7) |
| security mode in words | volume, energy, flow, temperatures, dates, error flags |
| status flags (battery low, error, alarm) — STS is in the clear | the same status flags |
| interval between transmissions (measured), the access counter, reception rate | |
| signal over the noise, the mode heard (T1 / C1-A / C1-B / S1), the frequency | |
| the encrypted length ("2 blocks") in the detail | |

**Never shown, sent, logged or stored: the encrypted bytes themselves.** Not in the line, the JSON, the gallery, the
admin log or `ism-offline`'s output (it prints the header fields and "payload 32 bytes, encrypted"). A ciphertext archive
is useful to nobody but a key-cracker.

### 3.7 Data records (DIF / VIF) — only when not encrypted

A record is `DIF [DIFE…] VIF [VIFE…] data`. Parse records until the payload ends or a special DIF stops it.

**DIF** bits 0–3, the data field:

| Code | Data | Code | Data |
|---|---|---|---|
| 0 | no data | 8 | selection for readout (no data) |
| 1 | 8-bit integer | 9 | 2-digit BCD |
| 2 | 16-bit integer | A | 4-digit BCD |
| 3 | 24-bit integer | B | 6-digit BCD |
| 4 | 32-bit integer | C | 8-digit BCD |
| 5 | 32-bit real | D | variable length (LVAR byte follows) |
| 6 | 48-bit integer | E | 12-digit BCD |
| 7 | 64-bit integer | F | special: 0x0F / 0x1F manufacturer data to the end (stop); 0x2F idle filler (skip) |

DIF bits 4–5: function (00 instantaneous, 01 maximum, 10 minimum, 11 value during error). Bit 6: storage number LSB.
Bit 7: a DIFE follows (each DIFE: bits 0–3 more storage bits, 4–5 tariff, 6 subunit, 7 another DIFE). Integers are
little-endian, signed two's complement; BCD is least significant byte first, a top nibble of F = negative.

**VIF** (bit 7 = a VIFE follows). The ones worth naming:

| VIF (bits 0–6) | Quantity | Unit and scale |
|---|---|---|
| 0x00–0x07 | Energy | Wh × 10^(n−3) → shown in kWh / MWh |
| 0x08–0x0F | Energy | J × 10^n → shown in GJ / MJ |
| **0x10–0x17** | **Volume** | **m³ × 10^(n−6)** → shown in m³, 3 decimals |
| 0x18–0x1F | Mass | kg × 10^(n−3) |
| 0x20–0x23 / 0x24–0x27 | On time / operating time | s, min, h, days (n = 0…3) |
| 0x28–0x2F | Power | W × 10^(n−3) |
| 0x30–0x37 | Power | J/h × 10^n |
| **0x38–0x3F** | **Volume flow** | **m³/h × 10^(n−6)** |
| 0x40–0x47 / 0x48–0x4F | Volume flow | m³/min × 10^(n−7) / m³/s × 10^(n−9) |
| 0x50–0x57 | Mass flow | kg/h × 10^(n−3) |
| 0x58–0x5B / 0x5C–0x5F | Flow / return temperature | °C × 10^(n−3) |
| 0x60–0x63 / 0x64–0x67 | Temperature difference / external temperature | K / °C × 10^(n−3) |
| 0x68–0x6B | Pressure | bar × 10^(n−3) |
| **0x6C** | **Date** (type G, 16-bit) | day = bits 0–4, month = bits 8–11, year = bits 5–7 + (bits 12–15 << 3), + 2000 |
| **0x6D** | **Date and time** (type F, 32-bit) | minute 0–5, hour 8–12, day 16–20, month 24–27, year = bits 21–23 + (28–31 << 3) |
| 0x6E | HCA units (dimensionless) | heat cost allocators |
| 0x74–0x77 | Actuality duration | s / min / h / days |
| 0x78 | Fabrication number | shown in the detail |
| 0x7C | Plain-text unit (ASCII length + text follow) | shown as sent |
| 0x7F / 0xFF | Manufacturer-specific | skipped |
| 0xFB + VIFE | Extension table 1 (energy in MWh/GJ, volume in feet³ …) | phase 3 |
| 0xFD + VIFE | Extension table 2: **0xFD 0x17 error flags** (binary, 0 = "no alarms"), 0xFD 0x0E/0x0F firmware/software version, 0xFD 0x74 remaining battery (days) | ★ VIFE codes **ASSUMED** — check each against EN 13757-3's table 2 before naming it |

Any VIFE after a known VIF (per time unit, "backward flow", "at phase L1" …) that is not in the table makes that record
"(qualified)" in the detail and keeps it **out of the line** — a value with a misread qualifier is worse than none.

**What goes in the line**, in this order, at most these: the storage-0 instantaneous **volume** (or **energy** for heat
and cooling meters, **HCA units** for allocators), **flow** (if non-zero), the storage-1 value with its date ("30 Sep
120.111 m³" — the billing-day reading most meters carry), **error flags** ("no alarms" / "alarm 0x0004"). Everything
else goes to the detail and the JSON `records` array. An unparseable payload: "data not understood" (never a guess).

---

## 4. The device list, the line, the interval

### 4.1 One row per meter

Tracker key: `meter:<mfr>:<id>:<type>` (the long header's address when CI 0x72). Kept 2 h after last heard, like
sensors. Repeats fold like every ISM device; a meter's line changes on every frame only through its signal and access
number, which are **not** part of the fold comparison (the fold compares the line without "signal" and the interval).

### 4.2 The readable line

`<type name> · <maker> · <serial> · v0x<ver> · <security or readings> · <status> · every <interval> · signal <SNR> dB`

- `Water meter · Sensus · 12345678 · v0x68 · encrypted (AES) · every 16 s · signal 24 dB`
- `Water meter · Sensus · 12345678 · v0x68 · encrypted (AES) · battery low · every 16 s · signal 6 dB`
- `Cold water meter · Kamstrup · 87654321 · v0x1B · 123.456 m³ · flow 0.012 m³/h · 30 Sep 120.111 m³ · no alarms · every 16 s · signal 18 dB`
- `Heat meter · Diehl · 00451223 · v0x20 · 4 512.3 kWh · no alarms · every 2 min · signal 9 dB`
- `Meter · ZRI · 31415926 · v0x01 · manufacturer format 0xA2 · signal 12 dB`
- Via a repeater: `Water meter · Sensus · 12345678 · … · via KAM 00123456`.

**Signal is "dB over the noise"**, not dBm: no VibeServer radio is calibrated in dBm (ISM's hover already says "N dB
over the noise"). The interval is shown once two frames have been heard; until then the line omits it.

★ **Old clients:** the web/app `ismLine` rebuilds a line from numbers when the listener chose mph or kPa. So the server
also fills `name` = `"Water meter · Sensus · 12345678"` and `extra` = the rest (`"v0x68 · encrypted (AES) · every 16 s"`),
which an old client prints as `name · extra` — the serial survives. New clients use `line` verbatim for `kind: meter`
(nothing in it has a unit choice).

### 4.3 Interval and reception from the access number

For each new frame with ACC delta `d = (acc − lastAcc) mod 256`, `1 ≤ d ≤ 16`: one gap sample = `(t − lastT) / d`. The
interval is the median of the last 8 samples, shown rounded (`every 16 s`, `every 2 min`). A delta of 0 is a repeat
(some meters send twice) and is ignored; > 16 resets. **Reception** = frames heard ÷ frames sent (Σ 1 ÷ Σ d) over the
last hour, in the detail as `heard 92 %`. ★ **ASSUMED:** ACC steps once per transmission; if a meter's ACC does not move
(some only step it on readout), fall back to the median raw gap and show no reception figure. OMS asks meters to jitter
their send time, so the interval is a median, never a single gap.

### 4.4 A damaged frame

Format A, block 1 CRC good but a later block bad: the meter is **heard** (row updated, its last-heard time, signal,
`count`, interval from ACC only if block 2 was good) and its readings are not touched. `damaged` counts it in the
status. Block 1 bad: nothing (an M/ID that fails its CRC is not a meter). Format B: no CRC on block 1, so both checks
must pass.

### 4.5 JSON for a meter device (added to ISM's device fields)

`kind: "meter"` · `make` (name or 3 letters) · `model` (type name) · `name` · `line` · `extra` · `id` (the serial, a
string of 8 characters) · `mfr` ("SEN") · `ver` (number) · `devType` (number) · `wm` ("T1" | "C1-A" | "C1-B" | "S1") ·
`sec` (mode number; absent = unknown) · `secText` · `encBlocks` · `acc` · `every` (s) · `rx` (%) · `sts` (number) ·
`batt` (0 when STS bit 2) · `via` ("KAM 00123456") · `vol` (m³) · `energy` (kWh) · `flow` (m³/h) · `records`
(`[{q:"volume", v:123.456, u:"m³", st:0, fn:"inst"}, …]`, at most 16) · plus ISM's `freq`, `level`, `snr`, `seen`, `age`,
`count`. Absent = not carried.

---

## 5. Where it sits: the 868 band, the front end, the CPU

### 5.1 Does the capture cover 868.3 and 868.95? Yes — with two changes

The 868 band is tuned to **868.60 MHz** (DC between the channels), and every rate ISM runs at (≥ 0.9 MS/s) holds
±450 kHz: 868.3 is −300 kHz, 868.95 is +350 kHz. But:

1. **The detector's usable span is too narrow on the high side.** `vibe_ism_service.h` searches `centre ± (needHz +
   40 kHz)` = 868.21–868.99 MHz. A T1/C1 meter's upper tone sits at 868.95 + 50 kHz + up to 52 kHz of crystal error =
   **869.05 MHz**. Change the 868 band's `needHz` in `vibe_ism_bands.h` from 350 kHz to **450 kHz** (span 868.11–869.09).
   `vsIsmReach` (shim) then wants 868.15–869.05 tunable — the RTL and RSP have it. `ismHwCentre` is unchanged (868.6 has
   no DC offset).
2. **At the 0.9 MS/s floor, 869.05 is outside the capture** (±405 kHz usable). ISM still runs, but the status line adds
   `868.95 not covered at 0.9 MS/s` and the meters at 868.95 are not promised. At 1.024 MS/s and above it is covered.
   (Every RTL server runs 2.048/2.4 MS/s; the RSP more.) No rate change, ever — ISM's rule.

### 5.2 The channel: wider on the 868 band

ISM opens a channel per burst at **250 kS/s** with a triangular (CIC-2) decimator and slices it to run lengths with
smoothing of 12 µs or more. A T1/C1 chip is **10 µs**: 2.5 samples a chip, and the finest smoothing averages over more
than a chip. So:

- `FrontEnd::setChanRate(double)` — on the 868 band the service asks for **~500 kS/s** (the work rate ÷ the integer M
  nearest 500 k: 2.4 MS/s ÷ 5 = 480 kS/s, 2.048 ÷ 4 = 512 kS/s), giving ~5 samples a T/C chip and ~15 an S chip. The
  triangle's response at ±100 kHz (the edge of a T1 signal) is −1.2 dB at M = 5; its first null moves to 480 kHz, so
  adjacent-burst leakage improves.
- `slice()`'s smoothing windows (`kWins`, now in taps at 250 kS/s) become **times in µs** converted at the channel's
  rate, so the other 868 families slice exactly as before. `test-ism` must still pass all its checks unchanged.
- `FrontEnd::setKeepBaseband(true)` (868 band only): the closed channel's complex baseband `bb` is **moved** into the
  Burst (`Burst::bb`, `Burst::bbRate`) instead of freed. Typical meter burst 5–35 ms → 20–140 KB; ISM's 1.6 s cap bounds
  the worst case at 6.4 MB transient, freed as soon as the decoders return.

CPU of the wider channel: the mixer runs per **input** sample whatever the output rate, so the front end's cost is the
same; only slicing an 868 burst doubles (a few hundred µs on a Pi 500 per burst).

### 5.3 The decoder (`vibe_wmbus.h`, sample domain)

Gate (cheap, first): the burst's frequency within 868.95 ± 130 kHz (T1/C1) or 868.30 ± 130 kHz (S), the discriminator
shows FSK (`fskDevHz` 25–110 kHz) or, failing ISM's FSK test, the envelope is ≥ 85 % on; duration 2–250 ms; `bb`
present. Anything else is not a meter and costs nothing more.

1. **Discriminator** `f[n] = arg(bb[n] · conj(bb[n−1]))` (ISM's polar discriminator); remove the mean of the preamble
   stretch (the carrier offset).
2. **Chip rate from the preamble**: the `0101…` run is a square wave at half the chip rate — measure its zero-crossing
   spacing over the first ≥ 30 crossings (T ≥ 38 preamble chips, C 32, S ≥ 30). That gives the chip period directly
   (T may be 90–110 k, S ±2 %), so there is no rate grid to search.
3. **Matched filter**: a boxcar one chip long on `f` (fractional length by linear weight at the ends).
4. **Sync search**, both signs of `f`: correlate the sliced chip stream against `0x543D` (T/C, 16 chips) or the 10-chip
   preamble tail + 18-chip S sync (28 chips), allowing **≤ 1 chip error**. The 8 chips after `0x543D`: `01010100` → C,
   then 8 more → `11001101` A / `00111101` B; anything else → T.
5. **Chip timing**: sample at chip centres from the sync's phase; at each zero-crossing nudge the phase by 1/16 of its
   error (a first-order early/late loop). Enough for a frame of ≤ 4 000 chips with ≤ 2 % rate error.
6. **Chips → bytes**: T — 6-chip words through the inverse table; the first two words give L, the frame's length in
   chips follows from §2.2; a code error before the end = damaged. C — NRZ bytes, L first. S — Manchester pairs, both
   conventions; a pair `00`/`11` = a violation (damage).
7. **Frame**: blocks and CRCs (§2.2) → link layer → CI → header → records if not encrypted (§3).
8. **Plausibility** (`bandPlausible`): a meter reading is only accepted at 868.2–868.4 (S) or 868.85–869.05 (T/C).

`decodeAll` tries the wM-Bus decoder **first** on gated bursts (a 16-bit CRC per block is a far stronger check than
most ISM families') and stops there on a success. It runs on ISM's own worker thread.

### 5.4 Threading (Network > Audio > Spectrum > Decoders)

Nothing new: the ISM service's thread is a decoder thread fed by copy through its half-second drop-oldest queue; the DSP
thread never waits on it, ISM switches the audio chain off, and the spectrum keeps running. The wM-Bus decoder adds no
thread and no lock.

### 5.5 CPU budget

- **The decoder itself**: a 30 ms T1 burst at 480 kS/s is 14 400 samples — one discriminator, one boxcar, a zero-crossing
  scan, a 16/28-chip correlation across the burst (~0.4 M multiply-adds) and a few hundred table lookups. Estimate
  **< 1 ms on a Pi 2 per burst**; a street of 50 meters at a 16 s interval is ~3 bursts a second → **≤ 0.3 % of one Pi 2
  core**. The target ("a few % of a core") is met with room for a dense street.
- **The ISM front end it rides on** is NOT a few %: ISM-DECODER.md estimates **10–20 % of one Pi 2 core** (unmeasured —
  the Pi 2 is not to be touched without asking). Meters do not add to it. Say so plainly in the CPU table when built;
  measure it when Stuart allows (`scripts/run-tests.sh` prints the CPU rows; `ism-offline … 868600000` prints % of real
  time).
- **The report's size** is the real cost of many meters: ISM sends the whole list once a second to every listener. 300
  meters × ~300 bytes = 90 KB/s per listener. Phase 2: when the device JSON passes **16 KB**, send the full list every
  **5 s** (and at once to a joiner); clients age `seen` locally from the report's arrival. Measure first with the
  synthetic busy-street test (§7); do not build this unless the number says so.
- **Device cap**: ISM's `kMaxDevices = 300` (least recently heard dropped). A town street can hold more meters than
  that within range of a good 868 antenna. Phase 2: meters get their own cap of **500**, so a busy street never pushes
  the weather station off the list.

---

## 6. Gallery, background decoding, clients

### 6.1 Gallery (`vibe_ism_gallery.h`)

Meters arrive through ISM's `Sink` like every device (kind, name, line). Two changes:

- **The day log is throttled for meters** — a meter sends every 16 s, so a street of 50 would write 270 000 lines a
  day. A meter's line reaches the log only when its **readings or status change** (the line without signal, interval
  and access number), or **once per 15 minutes** otherwise, or the first time it is heard that day. Other kinds as now.
  `17:01:12  868.950  Water meter · Sensus · 12345678 · v0x68 · encrypted (AES) · every 16 s · signal 24 dB`
- **The summary card** counts meters by type: "ISM 868 MHz · 2026-10-25 · 63 devices: 58 meters (51 water, 4 heat,
  3 gas), 4 weather, 1 sensor", and the last-reading table includes each meter's line, as for every device.
- Serials are written to the log and card (decision 4). Encrypted bytes never are (§3.6).

### 6.2 Background decoding

Nothing new to build: an ISM profile on the `868` band now decodes meters too. The setup page's band list may name it
"868 MHz SRD — sensors and meters" (text only; the band id stays `868`). Same rules: only with nobody here, never on a
locked radio, ISM's own door.

### 6.3 Web client (`web/client/src/ism.ts`)

- A **METER** chip in the filter (ALL / WEATHER / TPMS / SENSOR / DOORBELL / ENERGY / KEY FOB / **METER**), shown always
  like the others.
- `ismLine`: for `kind === 'meter'` return `d.line` (no unit choices).
- The hover/tap detail for a meter: mode heard, frequency, signal, `heard 92 %`, access number, encrypted length, the
  records table when not encrypted, "via …".
- The status line adds `N meters` when any, and `868.95 not covered at 0.9 MS/s` when the report says so
  (`"wmGap":1`).

### 6.4 App (with ISM's app work — ISM-DECODER.md "App (not built)")

The METER chip, the same line and detail, from the shared `src/services/ism.ts` (`ismLine` moves there with ISM). D-pad
and controller reach the chip and the rows. The watch: nothing.

### 6.5 Copy that says where things are (AGENTS.md)

When meters ship, grep and fix: `AboutOverlay` (V12 note + feature list: "ISM sensors **and meters**"), the web tutorial
(`web/client/src/tutorial.ts`) if it lists ISM's chips, `sdrTour` if the app's ISM card names the kinds,
`website/index.html` if ISM is described there. Verify the whole sentence, not the part being added.

---

## 7. Tests

`vibeserver/test-wmbus.cpp` (run-tests.sh), from the published facts, independently of the decoder's own path (an
encoder written from §2, not by inverting the decoder):

- **CRC**: `"123456789"` → `0xC2B7`; the three frames of §2.3 byte for byte, every block's CRC.
- **M-field**: `SEN` ↔ `0x4CAE`, `KAM` ↔ `0x2C2D`; bit 15 set still decodes; an unknown code prints its letters.
- **ID**: `78 56 34 12` → `12345678`; a non-BCD nibble printed as hex.
- **3-out-of-6**: every word has three 1s, all distinct; round trip of all 256 bytes; an invalid word = code error.
- **Format A**: L = 9 (header only), L = 10 (CI only), 0x2E (§2.3), and L where `(L − 9) mod 16 = 0`; a corrupted
  block 3 → meter heard, readings untouched; a corrupted block 1 → nothing.
- **Format B**: L ≤ 127 one CRC; L > 127 two CRCs; a corrupted byte in block 1 fails the frame.
- **Header**: CI 0x7A, 0x72 (via a repeater: the meter is the long header's address), 0x78, 0x8C + 0x7A, an
  ELL-encrypted 0x8D, 0xA2 manufacturer-specific, CW modes 0/5/7/13 and their text.
- **Records**: every VIF row of §3.7 in at least one record; 8-digit BCD, 32-bit int, negative BCD, a type G and a type F
  date; 0x2F fillers; 0x0F stops; an unknown VIFE keeps the record out of the line.
- **Line**: the five examples of §4.2 exactly; the old-client `name`/`extra` split.
- **Interval**: ACC steps 1 at 16 s with jitter ± 1 s → "every 16 s"; with every third frame missed → still 16 s,
  `heard 67 %`; ACC frozen → raw-gap fallback, no %.
- **Through the real front end**: each frame modulated as **T1**, **C1-A**, **C1-B** and **S1** into noisy IQ at
  2.4 MS/s, 2.048 MS/s and 1.024 MS/s, captured at 868.6 MHz; carriers at the nominal frequency and ±60 ppm; chip rate
  ±10 % (T), ±2 % (S); deviation 40 and 80 kHz; both tone polarities; a sensitivity sweep (decode rate against channel
  SNR, to set the gate); two meters 300 ms apart; a T1 and an S1 at once; 20 s of noise and a CW carrier at 868.95 decode
  nothing; every existing 868 family in `test-ism` still decodes (the wider channel changed nothing).
- **Policy on the wire**: the JSON, the day log and the card contain the serial and **never** a run of the encrypted
  bytes (search for every 4-byte window of the placeholder payload in hex and decimal).
- **Day log**: an unchanged meter writes one line per 15 min; a changed reading writes at once; the card's meter counts.
- **Busy street**: 300 synthetic meters at 16 s → report size, list cap, CPU row.
- **Reach**: the HF+ still shows no ISM; the 868 band needs 868.15–869.05.

`scripts/test-server-ism.mjs` gains a meter clip (`test-wmbus --write-clip` through `fake-rtl-tcp.mjs --ism`): the meter
row and line on the wire, the serial present, no payload. `scripts/test-web-ism.mjs`: the METER chip, a meter row at
390 px, the detail. All headless browsers `--mute-audio`.

**Real signals:** `~/VibeSDR-test-audio/ism/pi500-rtl4-868600-2400k-30s.u8` (30 s at 868.6 MHz, 2.4 MS/s, the Pi 500's
RTL on its MW antenna; ISM decoded nothing in it) — run `ism-offline` over it first; meters are unlikely on that
antenna, and that is a no-false-decodes check, not a validation. **The validation needs a new capture**: 60 s of raw IQ
at 868.6 MHz, 2.4 MS/s, from a radio with an 868 antenna near Stuart's iPERL (UK water meters often sit in a pavement
chamber — weak; within tens of metres). Store it beside the others, never in a scratchpad. That capture confirms the
mode (T1 or C1), the interval, the ACC behaviour, the version byte and whether the printed serial is the radio ID.

---

## 8. Phased build plan

**Phase 1 — see the meters (T1 + C1, encrypted view, web).**
`vibe_wmbus.h` (CRC, M/ID/type, 3-out-of-6, frame A/B, CI 0x7A/0x72/0x78, CW security mode, STS), the sample-domain
demodulator for T and C, `setChanRate` / `setKeepBaseband` / µs smoothing in `vibe_ism.h`, the 868 band's `needHz`, the
meter kind in the Tracker (key, line, `name`/`extra`, fold rule), JSON fields, the web METER chip and `ismLine`.
Tests: CRC, fields, 3-of-6, formats A/B, T1/C1-A/C1-B through the front end, policy, `test-ism` unchanged, the server
clip. ★ Done when: a synthetic Sensus T1 frame shows as the §4.2 line on the web through the real server.

**Phase 2 — the rest of the 868 picture.**
S1 (and T2/S2 counted), DIF/VIF records and the line's readings, interval and reception from ACC, damaged-frame
handling, the gallery throttle and card counts, the meter cap, the report-size rule if the busy-street test needs it,
the app's chip and detail, the `868.95 not covered` status, the copy fixes (§6.5). Tests: §7's records, interval, day
log, busy street, S1 through the front end, web/app.

**Phase 3 — the less common frames.**
ELL (0x8C–0x8F) and AFL (0x90) headers checked against the standard's tables, compact frames (0x79) with a per-meter
format signature remembered from a full frame, the 0xFB/0xFD extension VIFs that are confirmed, the FLAG-checked maker
table, validation on Stuart's real capture. Tests: each header layout; a compact frame after its full frame; real-capture
replay with its expected rows written down.

**Phase 4 — only if Stuart asks.** N mode as a fifth ISM band (`169`, 169.40–169.475 MHz, GFSK, needs its own
channel/rate design — not designed here); an owner-only key entry (§9).

---

## 9. Policy, privacy, open questions

- **Never decrypt.** The decoder contains no AES, no key store and no key UI, and the encrypted bytes never leave the
  decoder (§3.6).
- **Serials shown everywhere — decided (Stuart, 2026-10-25), no hide setting.** This is the opposite of ISM's TPMS rule,
  and deliberately: a tyre sensor's ID follows a person's car wherever it goes, so ISM hashes it to "TPMS #2"; a meter's
  serial is printed on the meter, the meter is bolted at one address, and that address is already inside the server's
  own reception area. Joining a serial to an address needs the water company's records — in Stuart's words, a listener
  "could only tie a serial to an address by breaking into a water company's database". This overrides DECODER-RESEARCH
  §4.3's placing of wM-Bus behind an admin opt-in.
- **Later, possibly:** an owner entering **their own** meter's key on their own server, to read their own consumption.
  Not designed, not phase 1–3; it would need its own decision (where the key lives, admin-only, one meter per key).

**Open questions for Stuart** (not decisions):

1. **Unencrypted readings on public servers.** Serials are decided. A meter that sends its volume in the clear also
   shows consumption over the day, which can hint at whether a home is occupied. This design shows those readings
   everywhere, as asked; keep that, or show unencrypted readings only to the admin?
2. **The day-log throttle**: one line per meter per 15 minutes unless its reading or status changes — right interval?
3. **169 MHz (N mode)**: worth a fifth ISM band (mostly gas and water meters in France and Italy), or not?
4. **The owner's-own-key feature**: wanted at all, ever?

---

## Sources (published, non-code)

- EN 13757-3 (application layer: DIF/VIF, device types), EN 13757-4 (wireless: modes, coding, frame formats, CRC),
  EN 13757-7 (transport and security: CI headers, configuration field, security modes).
- OMS (Open Metering System) Specification, Volume 2 (primary communication; security profiles A/B/C).
- Texas Instruments AN067 / SWRA234, "Wireless MBUS Implementation with CC1101 and MSP430" — S/T radio parameters,
  preambles and sync words (Table 9), frame format A blocks, the CRC polynomial, init 0 and the final complement.
- Texas Instruments SWRA522E, "CC13xx Combined wM-Bus C-Mode and T-Mode" — the shared `0x543D`, C's `01010100`
  signalling byte, `0x543D54CD` (frame A) and `0x543D543D` (frame B), 16 × `01` preamble for C.
- Microchip, wM-Bus stack "TRX Configuration" table — S1/S2/T1/T2 frequency, coding, rate, preamble, sync.
- The CRC catalogue (reveng), CRC-16/EN-13757: poly 0x3D65, init 0, xorout 0xFFFF, check 0xC2B7.
- DLMS User Association FLAG manufacturer ID list (the three-letter codes).
