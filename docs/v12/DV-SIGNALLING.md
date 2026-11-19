# DV ID — DMR, D-STAR, System Fusion, NXDN and dPMR signalling (V12)

Branch `v12-dvsig` (from `v12-experimental` 3de409c9a). Written 2026-11-19.

> ★★★ **NEVER THE VOICE.** AMBE / AMBE+2 / IMBE are permanently excluded (Stuart). The vocoder bits of every system are
> skipped by their place in the frame and never read. Encrypted traffic is named as encrypted and nothing more.
>
> ★★★ **NOTHING HAS BEEN DECODED OFF AIR.** Every frame this receiver has read came from our own transmitter
> (`vibe_dvsig_tx.h`), built from the same facts as the receiver. A misreading shared by both passes every test here. The
> first real capture is the next step (RESUME HERE).

A listener tunes a repeater or a hotspot and picks **DV ID** (after NFM and M17). The server listens for all five systems at
once (AUTO) and reports who is on the channel; the NFM audio keeps playing (an analogue over on a mixed-mode repeater is
heard) and is muted while a digital transmission is being followed.

| System | What is shown (amateur band) | Public copy (elsewhere) |
|---|---|---|
| DMR (ETSI TS 102 361) | colour code, slot, source ID → talkgroup or private ID, talker alias (7-bit, ISO-8, UTF-8, UTF-16BE), embedded GPS (GPS Info LC), Short Data Defined text (SMS), data headers ("IP data to UDP port N"), privacy flag → "encrypted", data preambles | "DMR heard · CC 1 · TS2" |
| D-STAR (JARL STD7.0) | MY / MY2 → UR via RPT1 (RPT2), the slow-data 20-character message, D-PRS (APRS text with `$$CRC`) and NMEA RMC/GGA positions, the header rebuilt from slow data for a late joiner | "D-STAR heard" |
| YSF / System Fusion | source → destination callsigns, downlink / uplink, DN / VW / V/D 1, DG-ID, the GPS in the DCH data frames (Mic-E) | "YSF heard · DG-ID 23 · DN" |
| NXDN (48 and 96) | RAN, unit ID → group / unit, the cipher type → "encrypted" | "NXDN heard · RAN 5 · NXDN48" |
| dPMR (ETSI TS 102 490) | called ID, own ID, channel code | "dPMR heard · CC 0x28A" |

## RESUME HERE

1. **The first real signals** (the coordinator, or Stuart — nothing of Stuart's servers was touched by this work). With the
   receiver's log on (`VIBE_DVSIG_LOG=1` on the server's environment prints every DMR data burst it reads, with its slot type,
   colour code and BPTC fixes; extend the other systems the same way when a capture needs it), tune in DV ID:
   - **a UK 70 cm DMR repeater** (GB7xx, 430–440 MHz: Brandmeister / Phoenix / FreeDMR) while a QSO or a talkgroup is busy —
     the most valuable capture: CC, slot, IDs, **talker alias** (most BrandMeister users send one), and GPS if anyone sends it.
     The repeater's idle/hang bursts on the other slot are useful too (they prove the BS sync and CACH path).
   - **a UK 70 cm YSF / Fusion repeater** (GB3xx DN, or a WiRES-X node) — callsigns and DG-ID; GPS if an FT-5D/FTM-300 user
     has it on.
   - **a D-STAR repeater** if one is in reach (GB7xx ICOM) — the header, the slow-data message, D-PRS.
   - **a DMR hotspot near the Pi 500** (MS-sourced, simplex) — the MS sync path.
   - NXDN and dPMR: only if one is heard (business / PMR446 dPMR handhelds) — the public copy is all anyone sees there.
   A raw IQ recording of each (Raw IQ out, 48 kHz around the channel, a minute) lets this be iterated offline:
   `Receiver::feedIq` takes any rate. Keep recordings out of git.
2. **If a sync is found but nothing decodes** — the levels/polarity or the bit order of that system's frame is the suspect
   (the log says which step failed). **If nothing syncs at all** — the deviation sign. Nothing in VibeServer inverts the
   spectrum; a TX that does would need the negative patterns.
3. **Unverified facts to check first on air** (table below, ⚠): dPMR's whole FEC chain (the scrambler's output tap, the
   Hamming bit order, CRC presets, the interleaver's read order), NXDN's LICH parity and the VCALL cipher field, the DMR
   CRC-32 octet order (both orders are accepted), YSF's GPS block layout.

## Where it is offered (the reach gate, no dead controls)

- **A mode, per listener** (and one shared receiver on a shared dial / one-pipeline radio), like M17 — `paramsFor("dvsig")` is
  an NFM-shaped 12.5 kHz channel whose complex baseband goes to our receiver; the width is fixed (no BANDWIDTH row).
- **Only where the radio reaches VHF 136–174 MHz or UHF 400–470 MHz** (`vibe_radio_caps.h` "dvsig" — 2 m, 70 cm, the VHF /
  UHF PMR and business bands, PMR446). The HF+ gets it (136–174 is in its 60–260 MHz). **Never an Icom** (it reads IQ).
- **No library**: our own code everywhere, so also on Android's own hardware and VibeServer Lite (the shim is the same) —
  the app's ServerModeScreen has the switch (test-bench-defaults checks the two lists agree).
- **Owner switch** "Digital voice ID" on the setup page and the app's server screen; under it the opt-in
  **"show radio IDs off the amateur bands (to you only)"** (`dvsig+ids`, a "+" token, **off by default**) with its notice.
- **Benchmark row `dec_dvsig`**: one listener in AUTO on a DMR call with a talker alias, 18 dB C/N, 48 kHz IQ in real time;
  rule `"dvsig"` primary — below green the mode starts off on a fresh setup, the owner can always tick it back.
- **VFO B is not available in DV ID** (as M17).

## The law (UK WTA 2006 s.48) — decided on the server

`Receiver::json(ids, band)` writes two copies; the shim picks one per socket (`dvIdsFor` in local_sdr_shim.cpp):

| Where the dial is | Who | Copy |
|---|---|---|
| an amateur allocation of the server's ITU region (`vibe_bands.h namedBands`, region unknown = Region 1, as TONES) | everyone | **identities** |
| anywhere else (PMR, business, PMR446) | everyone | **public**: system, colour code / RAN / channel code, slot, kind, encrypted — no ID, callsign, alias, text or position |
| anywhere else, owner opted in (`dvsig+ids`) | the owner's own signed-in spectrum socket (`adminSocks`) | identities |

Positions go on a map **only from an identities copy on an amateur band** (`dvsigMapPoints`) — never the owner's opted-in
non-amateur copy. The clients never filter: a public copy simply has no identities in it, and the client parser drops any
it finds there anyway (test_dvsig).

## The receiver (`android/app/src/main/cpp/vibe_dvsig*.h`)

| File | What |
|---|---|
| `vibe_dvsig_fec.h` | the codes: Hamming (7,4) (12,8) (13,9) (15,11) (16,11), Golay(20,8) (M17's Golay(24,12) shortened), QR(16,7), BPTC(196,96), the embedded LC BPTC, RS(12,9) from its generator matrix, CRC-CCITT, X.25, CRC6/12 (NXDN), CRC7/8 (dPMR), the D-STAR K=3 code + interleave + PN, YSF FICH/DCH, NXDN SACCH/FACCH1, dPMR blocks |
| `vibe_dvsig_front.h` | the shared front end: IQ → 24 kHz → 5.8 kHz channel filter → vibedsp's FM discriminator (Hz) → RRC α 0.2 at 4800 Bd (24 kHz) and at 2400 Bd (decimated to 12 kHz), both 5 samples a symbol; the correlator bank |
| `vibe_dvsig.h` | one class per system (candidates → frames → transmissions), the Receiver (AUTO / pin, the log of 60, the report), the Listener thread |
| `vibe_dvsig_tx.h` | our transmitter for each system (tests, benchmark, e2e clip) |

- **Sync and timing together** (M17's method, generalised): every sync of every system correlated at every sample on its
  path — DMR BS / MS / direct TS1 / TS2 (the voice and data syncs are each other's negative), YSF, D-STAR frame sync / data
  sync / end, NXDN96 on path A; dPMR FS1–FS4 and NXDN48 on path B. Normalised and mean-removed, so level, deviation and the
  tuning error do not matter. A peak is interpolated to a fraction of a sample, which is the frame's timing; levels and DC
  are least-squares fitted from the known sync symbols (and refined from the decided symbols for 4-FSK).
- ★★★ **The correlators are integer arithmetic** (int16 samples × int16 coefficients `L·s − Σs` → int32), so NEON
  (vmull / vmlal), SSE2 (pmaddwd) and scalar are bit-identical by construction (the shim builds with `-ffp-contract=fast`,
  so float kernels could not be). test-dvsig checks 20 000 random full-scale windows and a whole decode both ways.
- **Following a transmission**: DMR keeps a 30 ms burst timebase (CACH TACT gives the slot on a repeater downlink; DM syncs
  give it in direct mode; an MS-sourced burst carries no slot number — shown as slot unknown); voice bursts B–F are read at
  their expected place (EMB QR(16,7), the embedded LC fragments by LCSS); 12 bursts with nothing valid ends it. YSF, NXDN
  and dPMR follow their frame period with a flywheel; D-STAR follows its 96-bit frames, re-timed at every data sync.
- **Late entry**: DMR from three agreeing EMBs (voice "IDs pending" until the embedded LC decodes); D-STAR from two data syncs
  420 ms apart, the header then rebuilt from slow data (CRC-checked); YSF from two FICHs; NXDN from FACCH1 or two SACCHs;
  dPMR from two CCHs 80 ms apart.
- **Threading and memory** (M17's pattern): the pipeline thread only copies its IQ into a 2 s bounded queue; everything
  runs on a `vibe-dvsig` thread at **decoder** priority (Network > Audio > Spectrum > Decoders); about 150 kB of sample
  history (1 s per path), 60 log entries — well under 1 MB a listener.

### Where every fact came from (the house rule)

| Fact | Source | Check |
|---|---|---|
| DMR sync patterns, burst layout, CACH/TACT interleave, slot type, EMB, BPTC(196,96) and its interleave, embedded BPTC, Golay(20,8), QR(16,7), Hamming matrices, RS(12,9) generator matrix and field, CRC-CCITT, CRC-9/CRC-32, checksum, the CRC masks, data types, data header / DD header layouts, DD formats | ETSI TS 102 361-1 V2.7.1 | Golay = table B.11 row by row; RS: table B.18's codewords vanish at α, α², α³ (two published facts agree); CRC check value |
| DMR LC layouts: group / unit voice, service options (privacy), GPS Info, talker alias header / blocks and formats, preamble CSBK | ETSI TS 102 361-2 V2.5.1 | — |
| ⚠ DMR CRC-32 octet order in the block | our reading of B.3.9 figure B.8C | both orders accepted |
| dPMR syncs, frame layouts, header / CCH / END fields, CRC7/CRC8, Hamming(12,8), scrambler polynomial, interleave matrices, channel code dibits, 2400 Bd ±1050 Hz | ETSI TS 102 490 V1.9.1 | — |
| ⚠ dPMR scrambler output tap, Hamming bit order, CRC presets (0), interleaver read order | our reading of §7 | **nothing** — the first capture settles it |
| D-STAR frame sync, header fields, P_FCS (CRC-CCITT), scrambler x⁷+x⁴+1 from all ones, LSB first, "1" = +deviation, K=3 rate ½, the 24-row interleave, data sync, end pattern, slow-data mini headers, D-PRS format | JARL STD7.0 (Japanese) | the interleave and the scrambler as generated = MMDVM firmware's tables (scratch script, not shipped); slow-data PN = 70 4F 93; X.25 check value |
| D-STAR conv taps (1+D+D², 1+D²) and output order | Ap2 + MMDVM firmware (GPL-2, read) | — |
| ⚠ D-PRS `$$CRC` = X.25 over the text after the comma | our reading of §6.8.1 / Ap2 | a position is also accepted without it if the format parses strictly |
| YSF sync, FICH layout, Golay(24,12) ×4 + CRC-CCITT, K=5 (M17's polynomials), 20×5 / 20×9 dibit interleaves, CSD / DCH placement by FN, the whitening | MMDVMHost (GPL-2), **read for facts** — no free Yaesu spec | the whitening as generated (x⁹+x⁵+1) reproduces the 20 published bytes |
| ⚠ YSF GPS block (0x03 + additive sum, type octets, radio code, then Mic-E) | YSFClients' gateway (GPL-2, read for facts); the Mic-E decoding is ours from APRS101 ch. 10 | — |
| NXDN FSW, frame layout, PN9 per-symbol scrambling, LICH, SACCH / FACCH1 sizes, puncturing, interleaves, CRC6 / CRC12, VCALL fields | MMDVMHost (GPL-2), **read for facts** — the NXDN Forum's specifications need a log-in, not used | the scrambling sequence as generated (x⁹+x⁵+1) reproduces MMDVM's table |
| ⚠ NXDN LICH parity (even over the top 4 bits — consistent with MMDVM's two cases), the cipher field (octet 7 top bits), NXDN96 = NXDN48's frame at 4800 Bd | our reading | **nothing** |

**No GPL code was copied**; the tables we use are generated from polynomials, read from the ETSI / JARL documents, or are
the published constants a specification is made of.

## Tests

| Suite | What | Result |
|---|---|---|
| `vibeserver/test-dvsig.cpp` (run-tests.sh) | §1 the specs' vectors (above) and every code through bit errors; §2 the correlator kernels SIMD = scalar; §3 our transmitter for each system → FM → AWGN → receiver: DMR downlink group call + alias + GPS at 30/18/14 dB, ±1.2–1.5 kHz off the dial, via the 48 kHz resampler; DMR MS simplex private call with privacy; DMR SMS (DD ISO 8859-1, rate ½, CRC-32); YSF DN callsigns + DG-ID + GPS; D-STAR header + message + D-PRS; D-STAR late join; NXDN48; NXDN96; dPMR; pins; a whole decode scalar = NEON; §4 3 min of noise, 1 min of analogue NFM speech, 30 s with CTCSS, a carrier plain and with a 1 kHz tone — **zero entries**; §5 the two copies; §6 the listener thread; §7 CPU | **48 / 0** (NEON); **48 / 0** forced scalar (`-DVIBE_FORCE_SCALAR`); 15 minutes of noise: 0 entries |
| `test-dvsig sweep` | the sensitivity table below | tool |
| `test-dvsig clip OUT.cu8` | the e2e clip (each system in turn at +25 kHz) | tool |
| `scripts/test-server-dvsig.mjs` (run-tests.sh) | the real server on a fake rtl_tcp: offered; 70 cm identities (DMR 2345678 → TG 235 with alias and GPS, YSF, D-STAR with the message, NXDN, dPMR); status, pins, NFM after; PMR446 range-locked public copy and **no identity in anything sent**; PMR446 shared dial with the opt-in: the signed-in owner gets IDs, the listener none; the owner's switch | **23 / 0** |
| `scripts/test_dvsig.ts` (run-tests.sh) | the client rule module: offer, parse (a public copy keeps no identity even if sent), each system's line, the VTS line, the map points (amateur identities only) | **27 / 0** |
| `handheld-ui/test/run_vts_golden.sh` | the R36S line generated from `dvsig.ts` (ops DV / DO) | **2476 checks, 0 failed** |

Also re-run and passing: test-radio-caps (249), test-bench-hw (60), test-bench-defaults, check-setup-page (432 ids).
The only-mode / DAB-only tests (`test-server-only-mode`, `test-web-only-mode`, `test-server-dab-only`, `test-web-dab-only`)
were updated to know the new demodulator but **not run here** (headless Edge — the coordinator's run-tests.sh).

### Sensitivity (synthetic AWGN, C/N in 9 kHz, 10 transmissions a point — `test-dvsig sweep`)

| C/N | DMR IDs | DMR alias | YSF | D-STAR | NXDN48 | dPMR |
|---|---|---|---|---|---|---|
| 6 dB | 8 | 0 | 3 | 6 | 10 | 9 |
| 8 dB | 10 | 6 | 10 | 10 | 10 | 10 |
| ≥ 10 dB | 10 | 10 | 10 | 10 | 10 | 10 |

(NXDN48 and dPMR are 6.25 kHz signals, so 9 kHz C/N flatters them by ~1.6 dB.)

## CPU

| Machine | Cost |
|---|---|
| M5 Pro, test-dvsig, AUTO (every system's correlators), no thread | **0.2 % of one core** (signal + noise); 0.19–0.23 % on noise alone |
| M5 Pro, forced scalar | 0.26–0.28 % — clang vectorises the scalar integer loop itself, so NEON gains little here |
| Pi 2 / Pi 3 / Atom | **not measured** (the `dec_dvsig` row measures it on the box and sets the default) |

- The **SSE2** kernel compiles for x86-64 (`-arch x86_64`) but **was not run** (no x86 machine, no Rosetta).
- The **ARMv7 NEON** kernel syntax-checks for armv7 (`--target=armv7-apple-ios9`); not run on a Pi.

## Clients

| | Where | What |
|---|---|---|
| Web | `web/client/src/main.ts` (dvBoxSync), `spectrum.ts`, `mobile.ts`, `index.html` (#dvBox) | the **DV ID** key after NFM / M17 where `modes` lists "dvsig"; the VTS line; the DV ID box: AUTO + DMR / D-STAR / YSF / NXDN / dPMR pins, the note (amateur or not), the log newest first, **MAP** (the existing pins map — amateur positions only), EXIT, minimise |
| App | `src/components/DvsigPanel.tsx`, `SDRScreen.tsx`, `VibeServerClient.ts`, `sdrProtocol.ts`, `sdrTypes.ts`, `ServerModeScreen.tsx` | the key, the VTS line, the box (pins, the log, EXIT); the owner's switch + opt-in with the same notice. **No MAP in the app yet** (its MapOverlay has no DV layer) |
| R36S | `handheld-ui/src/dvsig.h`, `vts.h`, `vts_core.h`, `app.h`, `engine.h` | the key; the bar's line ("DMR · G4ABC Stuart · CC 1 · TS2"); AUTO only, no list |
| Shared | `src/services/dvsig.ts` | the rules all three read |
| Copy | AboutOverlay (a V12 note, no on-air claim; the FAQ's DMR answer), the web tutorial's modes card | the app tour's mode card is generic ("digital radio, data modes") and stays right |

`vibe_web_page.h` was **built locally to run the e2e and not committed** (it is regenerated at release — build-web).

## Not built

- **Voice of any kind** — never.
- **DMR rate ¾ (trellis) and rate 1 data blocks** — counted and named "(rate ¾ data, not decoded)". Many Motorola LRRP
  position reports use rate ¾, so **LRRP positions are not decoded**; IP data is summarised ("IP data to UDP port 4001").
  LRRP itself is Motorola's, documented only by listeners.
- **DMR Tier III trunking**, CACH short LC, reverse channel, MBC / UDT, confirmed-data CRC-9 checks.
- **D-STAR fast data**, DV data packets (DD mode), code squelch.
- **YSF VW-mode DCH callsigns** (from the header only), Data-FR mode contents.
- **NXDN** layer-3 messages other than VCALL / TX_REL, trunking, NXDN96 checked against a real signal.
- **dPMR** slow data, type 1/2/3 data contents, the dialled-string form of IDs (shown as numbers).
- **A RadioID.net-style lookup** — deliberately not (no outbound calls; raw IDs are shown).
- **App MAP**, R36S pins and log list.
- The web headless test (`test-web-dvsig.mjs`) — not written; the web box was checked only by type/syntax and the bundle.

## Decisions for Stuart

1. **Mode name and place** — "DV ID", a mode after NFM / M17 (not a decoder in DECODERS…), because it needs the channel's IQ.
   OK, or another name ("DMR / D-STAR / YSF")?
2. **NFM audio in the mode** — plays, muted only while a digital transmission is followed. OK, or always silent?
3. **The reach** — VHF 136–174 / UHF 400–470 MHz on every SDR (HF+ included on VHF). OK?
4. **D-STAR and YSF off the amateur bands** — treated like DMR (public copy). They are amateur systems in practice; OK to keep
   the one rule?
5. **The owner's opt-in shows IDs to the owner only** (as 5-tone digits), never to listeners, never on the map. OK?
6. **dPMR IDs as plain numbers** (not the dialled-string form of TS 102 490 annex A). OK until someone asks?
7. **Benchmark default** — the row follows the other listener modes (off below green). At 0.2 % of an M5 core, a lighter rule?
