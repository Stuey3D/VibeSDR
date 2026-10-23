# Decoder research: every decoder we could add, signal tags, and the decoder menu (V12)

**Status:** research and brief only, 2026-10-23. Nothing here is built. The inventory (§2) was checked against
`v12-experimental` at the time of writing; everything else is a proposal.
**Related:** `BACKGROUND-DECODING.md` (the idle-decoding engine and the gallery every new decoder should feed),
`DECODERS-GALLERY.md` (satellite decoders, client-assisted decode), `RADIO-ROLES.md` (dedicated radios, the governor),
`SATELLITE-ENGINE.md`, `../DECODER-AUDIT-2026-10-04.md` (quality bugs in the decoders we already have).

**How to read the estimates.** Difficulty is S (days), M (one to two weeks), L (weeks), XL (a month or more, or
open-ended). CPU is judged against the minimum confirmed spec, a Raspberry Pi 2 (900 MHz quad Cortex-A7, 1 GB), as a
share of ONE core: *trivial* < 2 %, *light* 2–10 %, *medium* 10–30 %, *heavy* 30–100 %, *beyond* more than a core.
These are estimates from the published algorithms and from how the reference programs behave on small ARM boards.
None of them has been measured on our code. Each decoder must get a row in `vibe_benchmark_decoders.h` before it ships,
as the existing ones have.

**Not legal advice.** Every licence, patent and interception point below is the best reading of the public sources,
with the uncertain ones marked. Before shipping anything flagged ⚖, it needs a proper check.

---

## 1. Executive summary

We already have a solid HF core: RTTY with AUTO, NAVTEX with station naming, WEFAX, SSTV, FT8/FT4, five time
stations, SYNOP/SHIP translation, RDS/Advanced RDS and DAB/DAB+. All of it decodes on the server, and the idle-decoding
engine and the gallery now exist to carry more. The gaps are:

- the rest of the amateur keyboard modes (CW, PSK, the MFSK family);
- WSPR;
- VHF data (APRS, AIS, ACARS, VDL2, ADS-B);
- maritime DSC;
- the SAM/ECSS/AM-stereo demodulators;
- a way to tell a listener what a signal is before they have guessed the decoder.

**Principles carried through every recommendation:**

1. **Decode on the server.** Clients display. The only exceptions are things that are not decoding, such as a static
   band-plan table drawn on the spectrum.
2. **Build our own from the published standard.** GPL programs are references for behaviour and black-box test
   oracles. They are never copied into the tree (the App Store exception cannot be granted over third-party GPL code).
3. **Feed the gallery and the background engine.** A decoder that can run unattended on a fixed frequency (NAVTEX, DSC,
   WSPR, APRS, AIS, ACARS) gets far more value per line of code than one a listener must sit with.
4. **No dead controls.** A decoder appears only where this server, this radio and this capture bandwidth can run it.

### Recommended build order

| Tier | What | Why first |
|---|---|---|
| **1: next** (cheap, popular, standards public, mostly background-capable) | **CW (our own)**, **DSC MF/HF + VHF**, **WSPR**, **PSK31/63**, **APRS/AX.25 1200 + 300**, **AIS**, **SAM → ECSS → AM stereo** (already planned), **CTCSS/DCS tone ID**, **more time stations** (JJY, BPC, CHU, TDF), **signal tags phase 1** (band-plan layer + the FSK/RTTY detector already approved) | Each is S/M, light on a Pi 2, and either fills a gap users hit today (CW currently needs an UberSDR) or runs unattended into the gallery. |
| **2: after** | **ACARS**, **VDL2**, **ADS-B 1090** (as a radio role), **HFDL (our own)**, **radiosondes (RS41)**, **FT8 multi-pass + JS8**, **FT4/FST4/FST4W/Q65/MSK144**, **MFSK family (Olivia, Contestia, MFSK16, Thor) + Hellschreiber**, **FreeDV + M17** (server-side Codec2), **amateur DMR/D-STAR/YSF signalling (no voice)**, **ALE 2G**, **Meteor LRPT** (already briefed), **DRM** (OS codec only), **signal tags phase 2** (IQ classifier), **aviation SELCAL**, **EAS/SAME** | Larger, or needs the channelizer to run several channels at once, or needs a whole radio. |
| **3: later or by request** | STANAG 4285 / MIL-STD-188-110 (identify, plus demod for the unencrypted minority), ALE 3G, PACTOR/VARA (identify only), EPIRB 406, UAT 978 (US), Mode S beyond ADS-B, HRPT, Inmarsat EGC SafetyNET, a curated rtl_433 subset, LoRa (identify only) | Niche, heavy, needs special hardware, or legally awkward. |
| **4: nice-to-have, app only** | **TV DX on the Android app's local hardware** (SDRplay / Airspy R2 / HackRF): "a portable TV of old, with advanced signal analysis", meaning watching plus MER/BER, constellation, echo profile, multiplex ID and a DX log (§4.5). Never on servers. | Stuart's final TV scope. |
| **⚖ Admin opt-in only, off by default** | POCSAG, FLEX, non-amateur trunking control channels (P25/DMR Tier III/TETRA: system identity only) | Content is someone else's private message in the UK and elsewhere. |
| **✗ Excluded** | HD Radio, AMBE/AMBE+2 voice, TETRA voice, anything encrypted, pay-TV, Iridium content, mobile phone content, **TV on servers / over the internet** (declined: §4.5) | §4. |

### Top 10 recommendations

1. **Our own CW decoder.** Morse is currently only available through an UberSDR's `morse` extension
   (`DecoderClient.ts` speaks it). A VibeServer listener has none. S/M, trivial CPU, background-capable as a CW
   skimmer later.
2. **DSC (MF/HF and VHF Ch 70).** It reuses the FSK decoder at 100 Bd/170 Hz, sits beside NAVTEX in the same
   background profile, and has a free ITU standard. S/M.
3. **WSPR.** The background decoder an antenna tester wants: one VFO per band, a two-minute cycle, and summary cards
   exactly like the FT8 ones. M.
4. **APRS/AX.25** (1200 Bd VHF, 300 Bd HF). It has public specs, is light, fills the gallery with a map, and covers the
   ISS digipeater (already in `DECODERS-GALLERY.md`). M.
5. **AIS.** Two GMSK channels near 162 MHz, a free ITU standard, very popular, and ideal in the background. M.
6. **SAM → ECSS → AM stereo (C-QUAM compatible).** Already approved and planned. It is a demodulator, not a decoder, but
   DXers notice it most. M.
7. **PSK31/63.** The most-used amateur keyboard mode after FT8 and CW. S/M, trivial CPU.
8. **CTCSS/DCS tone identification on NFM.** Tiny, and answers "what tone is this repeater?". S.
9. **Signal tags, phase 1.** A band-plan layer plus the approved "looks like RTTY" detector, drawn in a dedicated
   no-overlap lane (§5). M.
10. **The decoder menu with pins** (§6). It is needed before the list doubles in length. S/M.

---

## 2. Inventory: what we have today

All of VibeServer's decoders live in the shared native core (`android/app/src/main/cpp/`). That core is compiled into:

- the Linux/macOS daemon (`vibeserver/`);
- the Android server (VibeServer Lite and the app's own server);
- the apps, where the same `DecoderHost` runs on the phone for the Kiwi path's native sidecar (`KiwiAdapter.ts`
  `feedDecoderPcm`).

The decoder slot limit is box-wide (`FileSlots`) and set by the benchmark (`vibe_benchmark_decoders.h`).

### 2.1 VibeServer's own decoders

| Decoder | Code | Input | Platforms | Background (idle) decoding | Notes |
|---|---|---|---|---|---|
| **RTTY**: manual (ITA2/ASCII, any baud, shift, parity, stops, USOS) and **AUTO** (shift, centre, baud, polarity found from the signal) | `decoders/fsk_decoder.*`, `decoders/rtty_auto.*`, `src/utils/rttySpec.ts` | audio 48 kHz | server: Linux, macOS, Android; app sidecar on Kiwi | ✓ Linux/macOS (not Android yet) | LSB inverts mark/space automatically. |
| **SYNOP / SHIP** plain-English weather reports | `vibe_synop.h` (branch `v12-synop`, awaiting Stuart) | RTTY text | server | ✓ via RTTY | Frame `0x08`; WMO FM 12/FM 13; station list from NOAA ISD (public domain). |
| **NAVTEX / SITOR-B** (CCIR 476, FEC, message cutter, station naming) | `fsk_decoder.*` (`NavtexRx`), `vibe_navtex_stations.h`, `src/utils/navtex.ts` | audio | server, app sidecar | ✓ | 490/518/4209.5 kHz; station table from NGA Pub. 117 plus a supplement. |
| **WEFAX** 120 LPM (IOC 576), phasing, start/stop tones, auto-align | `decoders/wefax_decoder.*`, `src/utils/wefaxAlign.ts` | audio | server, app sidecar | ✓ (PNG per chart) | "Gold standard" after the October audit; 60/90/240 LPM are not offered. |
| **SSTV**: Martin M1–M4, Scottie S1/S2/DX, Robot 8/12/24/36/72 and B/W, PD-50…PD-290, Wraase SC-2 120/180, Pasokon P3/P5/P7 | `decoders/sstv_decoder.*` | audio 12 kHz | server, app sidecar | ✓ USB/NFM (not LSB) | The audit's slant and sync bugs are listed in `DECODER-AUDIT-2026-10-04.md`. |
| **FT8 / FT4** spots | `decoders/ft8_decoder.*` + `ft8_lib/` (kgoba, **MIT**) | audio | server, app sidecar | ✓ (last-10 and hourly summary cards) | Single pass, about 2–4 dB short of WSJT-X (audit). |
| **Time signals**: MSF, DCF77, WWVB (envelope), WWV/WWVH (100 Hz subcarrier), RWM (markers + Morse ID, no timecode) | `decoders/time_decoder.*` | audio | server, app sidecar | — | JJY, BPC, CHU and TDF are missing (§3). |
| **RDS / RBDS** and **Advanced RDS** | `vibedsp/rds.cpp` (clean-room EN 50067 / IEC 62106) | MPX | server | — | Not a decoder slot; has its own switch. |
| **DAB / DAB+** (ensemble, MP2 ours, AAC by the server OS's codec, TII, EPG, MOT slideshow) | `vibe_dab_*.h` | IQ 2.048 MS/s | server (not on Airspy HF+) | — | A whole-profile mode. |
| **Audio NR, auto-notch** | `decoders/audio_nr.*`, `auto_notch.*` | audio | server | — | DSP, not decoders. |

**Demodulators** (`vibedsp.h` `RxPipeline::Mode`): AM (envelope only), USB, LSB, CW, NFM, WFM (stereo + RDS).
**There is no carrier PLL**, so no SAM, ECSS or AM stereo yet (the plan is in memory `sam_ecss_cquam_plan`; nothing has
been built). The `sam` id in `ModeSelector`'s `COMMON_IDS` only lights for backends that have it, such as OWRX and Kiwi.

### 2.2 Decoders we show, but another server does the decoding

| What | Backend | How we show it |
|---|---|---|
| Morse/CW decoder, Whisper transcription, FT8/digital spots, CW spots (skimmer), HFDL map | **UberSDR** (its add-on decoders) | `DecoderClient.ts` `morse`/`whisper`; server maps (`onServerMap('hfdl'|'digi'|'cw')`) |
| Everything OpenWebRX+ offers: ADS-B, ACARS, VDL2, HFDL, AIS, APRS/packet, POCSAG/FLEX ("page"), ISM (rtl_433), radiosondes ("sonde"), wM-Bus, Meshtastic/LoRa, DMR/YSF/D-STAR/NXDN (through its codecserver), FreeDV, M17, WSPR/JT/FST4/Q65/MSK144, BPSK, CW, SSTV, FAX, DRM, DAB, and more | **OpenWebRX+** | its DIGITAL / DECODERS dropdown (`ModeSelector` `others`), whole-profile modes in `dataModes.ts` |
| SAM, and the KiwiSDR extensions (FT8, WSPR, FAX, NAVTEX/DSC/Selcall FSK, ALE, DRM, HFDL, timecode) | **KiwiSDR** | Kiwi modes; extensions are not driven by us |

★ The ADS-B shown on the watch and in App Store shots is OpenWebRX's decoder. **We have no ADS-B, AIS, APRS, ACARS,
HFDL, POCSAG, CW or WSPR decoder of our own.** (memory `decoders_where_they_run`)

### 2.3 Related pieces that already exist

- **Background decoding and the gallery** (`vibe_bgdecode.h`, `vibe_gallery.h`): VFOs cut from one forward FFT by the
  `Channelizer`, one `RxPipeline` + `DecoderHost` each, priority tiers, and listeners always win a slot. Not yet on the
  Android server.
- **"Anyone know what this is?" canned answers** (`local_sdr_shim.cpp` `is_*` ids, `src/services/dialChat.ts`): 27 answers
  that link to sigidwiki.com and offer DECODE where we have the decoder. The tag classifier (§5) should use the same ids.
- **EiBi** schedule (`vibeserver/eibi.*`, fetched by the server, used by search and the VTS).
- **The approved FSK hint** (memory `v12_signal_suggest_decoder`): "looks like RTTY: open the decoder?". This is the
  seed of §5.

---

## 3. Per-decoder survey

**Key.** Std: ✓ = free public standard; $ = published but paywalled; P = proprietary/undocumented; RE = only
reverse-engineered. **Where:** S = server (all of these are server-side by our rule; "S+R" means it needs a whole
radio or a dedicated radio role). **BG** = suitable for background decoding. Priority is the tier from §1.

### 3.1 HF utility

| Decoder | Where heard | Standard | Open references (licence) | Diff. | Pi 2 CPU | BG | Popularity | Tier |
|---|---|---|---|---|---|---|---|---|
| NAVTEX / SITOR-B | 490, 518, 4209.5 kHz; SITOR-B also on HF maritime MSI | ITU-R M.540, M.476/M.625 ✓ | fldigi (GPL-3), multimon-ng? no; KiwiSDR FSK ext (GPL) | **have** | trivial | ✓ | high (Europe) | have; FEC items in audit |
| **DSC MF/HF** | 2187.5, 4207.5, 6312, 8414.5, 12577, 16804.5 kHz; 100 Bd, 170 Hz FSK | ITU-R M.493 (+M.541) ✓ | KiwiSDR FSK ext (GPL-3), SDRangel DSC demod (GPL-3), YaDDNet community | S/M (FSK exists; add the 10-bit code + expansion, MMSI decode) | trivial | ✓ | medium-high with maritime listeners | **1** |
| **DSC VHF** | 156.525 MHz (Ch 70); 1200 Bd AFSK 1300/2100 Hz | ITU-R M.493 ✓ | SDRangel (GPL-3) | S (same message layer, Bell-202-like modem shared with APRS) | trivial | ✓ | medium (coastal) | **1** |
| SITOR-A / ARQ | HF maritime, mostly gone | M.476 ✓ | fldigi | S | trivial | — | very low | 3 (identify only) |
| **WEFAX** | 2.6–16.9 MHz (Northwood, DWD, Kyodo, NWS…) | WMO/ITU-T T.4-era, IOC 576/288 ✓ | fldigi, HamFax (GPL) | **have** | light | ✓ | high | have; add 60/90/240 LPM + IOC 288 (S) |
| SYNOP/SHIP/TEMP over RTTY | DWD 4583/7646/10100.8 kHz | WMO-No. 306 ✓ | — | **have** (branch) | trivial | ✓ | niche but loved | have |
| **ALE 2G** | 2–30 MHz military, government, amateur ALE (HFLink) | MIL-STD-188-141 App. A ✓ (DoD ASSIST, free) | KiwiSDR ALE_2G ext (GPL), PC-ALE (freeware), LinuxALE | M (8-FSK 125 Bd, Golay, word parser) | light | ✓ (scan list) | medium among utility DXers | **2** |
| ALE 3G | military | MIL-STD-188-141 App. C / STANAG 4538 ✓ (141) | few open | L | medium | — | low | 3 |
| STANAG 4285 / 4529 / MIL-STD-188-110 A/B/C | 2–30 MHz military/naval, 2400 Bd PSK at 1800 Hz | 110: ✓ (ASSIST); STANAG 4285 public copies circulate, NATO release status uncertain | SigDigger (GPL-3) for analysis; fldigi has none; RFSA/Sorcerer (closed) | L (equaliser, probes, interleavers) | medium | — | medium (identify), content usually encrypted | **3** (identify first) |
| PACTOR I/II/III/IV, VARA, CLOVER | HF data (Winlink, maritime e-mail) | PACTOR I documented; II–IV and VARA proprietary (SCS, EA5HVK) P | none for II–IV | identify only: S/M | trivial | — | high on the bands, people ask "what is it?" | **3: identify only** |
| **HFDL** | 2.9–22 MHz aircraft data, 1800 Bd PSK | ICAO Doc 9741 / ARINC 635 $ | dumphfdl (GPL-3), KiwiSDR ext | L (multi-channel; we show UberSDR's today) | medium per channel | ✓ | high (aviation fans) | **2** |
| **CW (Morse)** | everywhere on HF; beacons, NDBs, amateur | ITU-R M.1677 ✓ | fldigi (GPL), CW Skimmer (closed), csdr/OWRX (GPL/AGPL) | S/M (adaptive WPM, matched filter, confidence like UberSDR's) | trivial | ✓ (skimmer later) | very high | **1** |
| CW skimmer (whole passband) | contests, RBN | — | CW Skimmer (closed), RBN, csdr skimmer | M/L (needs channelizer + many CW decoders) | medium on Pi 2 for ~20 channels | ✓ | high | 2 |
| Aviation **SELCAL** | HF aero voice channels (e.g. Shanwick 8879, 5649) | ARINC 714 (tone table well known) | multimon-ng? (no); simple | S (two-tone pair detection, 16 tones) | trivial | ✓ | medium (aviation) | **2** |
| Maritime/land Selcall (CCIR 493-4, ZVEI, CCIR, EEA) | HF/VHF | ITU-R M.493 / M.257; tone tables ✓ | multimon-ng (GPL-2) ZVEI/CCIR | S | trivial | — | low | 3 |
| CODAR / OTH radar | HF sweeps | — | — | identify only (§5) | — | — | people ask | tags only |

### 3.2 Amateur

| Decoder | Where | Standard | References (licence) | Diff. | Pi 2 CPU | BG | Popularity | Tier |
|---|---|---|---|---|---|---|---|---|
| FT8 / FT4 | 1.840, 3.573, 7.074, 10.136, 14.074, 18.100, 21.074, 24.915, 28.074, 50.313 MHz (FT4: 3.575, 7.0475, 10.140, 14.080, 18.104, 21.140, 24.919, 28.180) | QEX paper "The FT4 and FT8 Communication Protocols" ✓ | WSJT-X (GPL-3), ft8_lib (MIT, in use) | **have**; multi-pass + subtraction is M | heavy burst every 15 s | ✓ | very high | have; improve (2) |
| **WSPR** (WSPR-2) | 136 kHz, 474.2 kHz, 1.8366, 3.5686, 5.2872, 7.0386, 10.1387, 14.0956, 18.1046, 21.0946, 24.9246, 28.1246, 50.293 MHz | protocol documented by K1JT (K1JT/G4JNT notes) ✓ | WSJT-X `wsprd` (GPL-3), wsprdaemon (GPL), KiwiSDR WSPR ext | M (4-FSK, Fano/sequential decoder, K=32 r=½) | light (bursty, every 2 min) | ✓✓ | high with antenna testers | **1** |
| FST4 / FST4W | LF/MF and HF | QEX 2021 paper ✓ | WSJT-X (GPL-3) | M (shares the LDPC machinery) | light | ✓ | low-medium (LF fans) | 2 |
| Q65 | 6 m EME/scatter, microwaves | Q65 paper (Nico Palermo, K1JT) ✓ | WSJT-X (GPL-3) | M/L (q-ary RA code) | medium | — | low | 2 |
| MSK144 | 6 m / 2 m meteor scatter (50.280, 144.360) | WSJT-X docs ✓ | WSJT-X | M | light | ✓ during showers | low | 2 |
| JT65 / JT9 | historic, still on 160/LF and EME | documented ✓ | WSJT-X | M (RS soft-decision is the hard bit) | light | ✓ | low and falling | 3 |
| **JS8** | 3.578, 7.078, 10.130, 14.078, 18.104, 21.078, 28.078 MHz | an FT8 derivative; JS8Call source (GPL-3) is the de-facto spec | JS8Call (GPL-3) | M (ft8_lib-like core, different costas/timing, Normal/Fast/Turbo/Slow) | heavy burst like FT8 | ✓ | medium (EmComm) | **2** |
| **PSK31 / PSK63 / QPSK31** | 14.070, 7.040 (R1), 3.580, 21.070, 28.120 MHz | G3PLX's published description ✓ (varicode) | fldigi (GPL-3) | S/M | trivial | ✓ (multi-channel later) | high | **1** |
| Olivia, Contestia | 14.072–14.076 etc.; 8–64 tones, 125–2000 Hz | Olivia described by SP9VRC (papers) ✓-ish | fldigi (GPL-3) | M (Walsh/FHT; many submodes → AUTO needed) | light | ✓ | medium | 2 |
| MFSK16 / MFSK32 (+ pictures), Thor, Domino | scattered | published by authors (IZ8BLY, ZL1BPU) ✓-ish | fldigi (GPL-3) | M (share an MFSK core with Olivia) | light | ✓ | low-medium | 2 |
| Hellschreiber (Feld-Hell, FM-Hell) | 3.580, 7.077, 14.063 MHz | historic Hell spec ✓ | fldigi (GPL-3) | S (it is a picture: draw columns) | trivial | — | low, but delightful and visual | 2 |
| **SSTV** | 3.735/3.845, 7.165/7.171, 14.230, 21.340, 28.680, 145.800 (ISS) | Dayton/JL Barber "Proposal for SSTV mode specifications" ✓ | slowrx (ISC), QSSTV (GPL), MMSSTV (closed) | **have**; add MMSSTV MP/MR/ML, AVT, FAX480, PD no-VIS start (M) | light | ✓ | high | have; extend (2) |
| **FreeDV** (700D/700E/1600/2020/RADE) | 14.236, 7.177, 3.625 MHz | Codec2 + modem specs open | Codec2 (**LGPL-2.1**), FreeDV GUI (LGPL), RADE (BSD-style, check) | M (link Codec2 dynamically on the server; send PCM as Opus) | medium (700D OFDM + LDPC); RADE is heavy (ML) | — | medium | **2** (700D/700E/1600 first) |
| **M17** | 70 cm / 2 m reflectors | M17 spec (open) ✓; voice is Codec2 3200 | libm17, m17-cxx-demod (GPL-3), SDR++ m17_decoder (GPL-3) | M (4-FSK 4800 Bd + Codec2) | light | — | small but growing; legal voice! | **2** |
| **APRS / AX.25 1200 Bd** | 144.800 (EU), 144.390 (US), 145.825 (ISS), 144.575 (NZ) | AX.25 2.2 (TAPR) ✓, APRS 1.0.1 ✓ | direwolf (GPL-2), multimon-ng (GPL-2) | M (Bell 202 AFSK; direwolf-class multiple demodulators per channel for weak signals) | light | ✓✓ (map, gallery) | high | **1** |
| APRS / packet 300 Bd HF | 10.1476/10.149, 14.105 MHz | same ✓ | direwolf | S once 1200 exists | trivial | ✓ | low-medium | **1** (with 1200) |
| 9600 Bd G3RUH packet | satellites, 70 cm | ✓ | direwolf | S/M | light | ✓ | low | 2 |
| Amateur DMR / D-STAR / YSF / NXDN **signalling, no voice** | 2 m / 70 cm repeaters and hotspots | DMR ETSI TS 102 361 ✓; D-STAR JARL spec ✓; YSF partly documented; NXDN forum spec ✓ | dsd-fme (mixed GPL/ISC), DSDcc (GPL-3), digiham (GPL) | M–L each | light | ✓ (who is on the repeater) | high interest | **2** (§4.2) |

### 3.3 Aviation

| Decoder | Where | Standard | References (licence) | Diff. | Pi 2 CPU | BG | Popularity | Tier |
|---|---|---|---|---|---|---|---|---|
| **ACARS** | 131.550/131.725 (EU), 131.525, 130.025, 130.450, 131.125, 129.125 (US) MHz; 2400 bps MSK on AM | ARINC 618 $ (SAE ITC); widely documented | acarsdec (GPL-2), SDRangel (GPL-3) | M (several channels through the channelizer) | light per channel | ✓✓ | high | **2** |
| **VDL Mode 2** | 136.975 (CSC), 136.725/136.775/136.875 (EU) MHz; D8PSK 10.5 ksym/s | ICAO Annex 10 Vol III / Doc 9776 $, ARINC 631 $ | dumpvdl2 (GPL-3) | L (D8PSK, RS, AVLC, X.25/CLNP, then ACARS-over-AVLC) | medium | ✓ | high in Europe (more traffic than ACARS) | **2** |
| **ADS-B 1090 ES / Mode S** | 1090 MHz, PPM 1 Mb/s | ICAO Annex 10 Vol IV; RTCA DO-260B $ | dump1090 (GPL-2+), readsb (GPL-3), SDRangel | M (preamble, CRC-24 + error repair, CPR position); a **whole radio** at ≥ 2 MS/s | light-medium (readsb runs on a Pi Zero) | ✓ (radio role) | very high | **2** (needs `RADIO-ROLES.md` dedicated radio) |
| UAT 978 | 978 MHz (US only) | DO-282B $ | dump978-fa (FlightAware; licence BSD-style, check) | M | medium | ✓ | US only | 3 |
| Mode A/C, Mode S surveillance replies | 1090 / 1030 | Annex 10 Vol IV | readsb | S on top of ADS-B | — | — | low | 3 |
| HFDL | see §3.1 | | | | | | | 2 |
| VOR / ILS identification | 108–118 MHz | ICAO Annex 10 Vol I | SDRangel VOR (GPL-3) | M (30 Hz AM vs FM bearing, Morse ID) | light | — | medium curiosity | 3 |

### 3.4 Maritime

| Decoder | Where | Standard | References | Diff. | Pi 2 CPU | BG | Popularity | Tier |
|---|---|---|---|---|---|---|---|---|
| **AIS** (Class A/B) | 161.975, 162.025 MHz; GMSK 9600 | ITU-R M.1371-5 ✓ | AIS-catcher (GPL-3), rtl-ais (GPL-2), gnuais | M (two channels at once; NMEA out; message types 1–27) | light | ✓✓ (map) | very high on coasts | **1** |
| DSC VHF / MF-HF | §3.1 | | | | | | | 1 |
| NAVTEX | have | | | | | | | — |
| Inmarsat-C EGC SafetyNET | 1537–1545 MHz (L-band, needs patch antenna + LNA) | partly documented $ | SDR-based "Scytale-C" (unclear licence), JAERO (MIT) for Aero | L | medium | ✓ | low; needs hardware | 3 |

### 3.5 Paging ⚖

| Decoder | Where | Standard | References | Diff. | CPU | Tier |
|---|---|---|---|---|---|---|
| POCSAG 512/1200/2400 | UK 153 MHz band and 466 MHz; US 152/157/929–931 MHz | ITU-R M.584-2 ✓ | multimon-ng (GPL-2), PDW (closed) | S | trivial | **⚖ opt-in** |
| FLEX 1600/3200/6400 | US 929–931 MHz, parts of Europe | Motorola proprietary P (the protocol has been documented by reverse engineering; underlying patents date from the early 1990s and have probably expired: not verified) | multimon-ng (GPL-2) | M | light | **⚖ opt-in** |

Pager content is private messaging (medical and fire pagers carry names, addresses and conditions). See §4.3.

### 3.6 Broadcast

| Decoder | Where | Standard | References | Diff. | CPU | Tier |
|---|---|---|---|---|---|---|
| **DRM30** (and DRM+) | SW/MW broadcasters (AIR, Radio Romania, KTWR, BBC tests, China), DRM+ VHF trials | ETSI ES 201 980 ✓ | Dream (GPL-2) with FAAD2/fdk-aac; KiwiSDR DRM ext (Dream port) | XL (OFDM modes A–D, MLC, SDC/FAC, then AAC/xHE-AAC framing for the OS decoder) | medium for the OFDM; audio by the OS | **2** (§4.4) |
| AM stereo (C-QUAM compatible) | a handful of MW stations (some in Japan, Australia, the US, Brazil; few in Europe) | Motorola C-QUAM, patents expired ~1997–2000 (memory) | SoDiRa (closed), KiwiSDR? (check) | M (on top of SAM) | light | **1** (planned) |
| SAM / SAM-U/L, ECSS | all AM | — (it is DSP) | csdr, KiwiSDR (GPL) | M (PLL) | light | **1** (planned) |
| RDS/RBDS, DAB/DAB+ | have | | | | | have |
| HD Radio (NRSC-5) | — | ✗ excluded | | | | ✗ |
| **CTCSS / DCS tone ID** | every NFM repeater, PMR446 | EIA/TIA-603 tone tables ✓ | multimon-ng (no CTCSS), many simple | S (Goertzel bank 67–254.1 Hz; DCS 134.4 bps Golay-23 code words) | trivial | **1** |
| EAS / SAME | NOAA Weather Radio 162.400–162.550 MHz (US/Canada) | 47 CFR §11.31 ✓ | multimon-ng EAS (GPL-2) | S (AFSK 520.83 Bd) | trivial | **2** (US listeners) |
| Teletext / WSS / VPS | analogue TV (largely gone) | ETSI EN 300 706 ✓ | — | M | — | with TV, app only (§4.5) |

### 3.7 Satellite

| Decoder | Where | Standard | References | Diff. | CPU | BG | Tier |
|---|---|---|---|---|---|---|---|
| NOAA APT | 137.1/137.62/137.9125 MHz | NOAA KLM User's Guide ✓ | WXtoImg (closed), SatDump (GPL-3), noaa-apt (GPL-3) | S | light | — | ✗ **service ended** (NOAA-18 June 2025, -15 and -19 August 2025). Archive-file decoder at most. |
| **Meteor-M LRPT** | 137.1 / 137.9 MHz, 72k QPSK or 80k OQPSK | CCSDS 131.0-B ✓ + Planeta format notes | SatDump (GPL-3), meteor_demod/decode (MIT, check) | L | heavy but deferrable | ✓ (pass scheduler) | **2** (`DECODERS-GALLERY.md`) |
| HRPT / AHRPT | 1.7 GHz, needs a tracking dish | ✓ | SatDump | XL | beyond Pi 2 | — | 3 |
| ISS / amateur sats: SSTV, APRS, 9600 packet, CW/BPSK telemetry | 145.800, 145.825, 435–438 MHz | ✓ | gr-satellites (GPL-3) | reuse + per-sat framing (M) | light | ✓ (pass-triggered) | via 1/2; needs the satellite engine's Doppler (§3.7A) |
| Orbcomm | 137–138 MHz | P (undocumented) | gr-orbcomm? (unclear) | M | light | — | 3, ⚖ commercial messaging: identify/telemetry only |
| Iridium | 1616–1626.5 MHz | P | gr-iridium (GPL-3), iridium-toolkit | L | heavy | — | ✗ content; §4.3 |
| Radiosondes (RS41, DFM, M10/M20) | 400–406 MHz (403/404/405 in Europe) | RE (Vaisala does not publish) | rs1729/RS (licence check), radiosonde_auto_rx (GPL-3) | M per family (RS41 first: GFSK 4800, RS(255,231), Vaisala XOR) | light | ✓✓ | **2** (very popular, legal weather data) |

#### 3.7A Satellite decoders and the satellite engine

Doppler tracking and pass prediction are designed in **`SATELLITE-ENGINE.md`**:
- SGP4 from CelesTrak OMM, `sats.json` mirrored on vibesdr.net;
- an open-loop Doppler prediction plus a closed-loop trim, applied in the channel's DDC/NCO;
- the `loop` types `fll` / `none` / `costas`.

The satellite decoders, the gallery and client-assisted decode are in **`DECODERS-GALLERY.md`**, and the pass-window
scheduler and dedicated radios are in **`RADIO-ROLES.md` §3.1**. Nothing here redesigns them. This table only says what
each satellite decoder needs from them.

| Decoder | Doppler from the engine | Pass predictions | Engine `loop` | Covered by the satellite briefs? |
|---|---|---|---|---|
| NOAA APT | — (service ended 2025; archive files only) | — | — | ✓ (`DECODERS-GALLERY.md` trap: do not build) |
| **Meteor-M LRPT** | ✓ feed-forward (±3.1 kHz at 137 MHz) | ✓ capture window, max-elevation gate | `costas` | ✓ both briefs, plus RADIO-ROLES |
| HRPT / AHRPT (1.7 GHz) | ✓ about 12 times the 137 MHz figures (roughly ±38 kHz, several hundred Hz/s; scaled, not measured) | ✓ and **antenna tracking** (a dish on a rotator) | `costas` | ✗ gap |
| Orbcomm (137–138 MHz) | ✓ (±3 kHz class) | ✓ | demod-tracked (`costas`-like) | ✗ gap (and ⚖, §3.7) |
| **CubeSat telemetry**: AFSK 1200 and **GMSK/FSK 9600 AX.25** (435–438 MHz), CW beacons | ✓ essential: ±10 kHz, ~215 Hz/s at 70 cm, more than a 9k6 demodulator's own AFC should chase | ✓ | `fll` for AFSK on FM; demod-tracked for GMSK | partly: ISS AFSK 1200 only; ✗ 9k6 and per-satellite framing |
| **FUNcube-1 (AO-73) / FUNcube-family BPSK 1200 telemetry** (145.935 MHz) | ✓ (±3.3 kHz) | ✓ | demod-tracked (BPSK carrier recovery) | ✗ gap |
| FUNcube/linear transponders (SSB/CW) | model only (each uplinker has their own error) | ✓ | `none` | ✓ as the engine's phase 3 (v2) |
| **ISS SSTV** (145.800 MHz NFM) | ✓ | ✓ (and event dates) | `fll` | ✓ (engine first consumer; gallery phase 2) |
| **ISS APRS digipeater** (145.825 MHz) | ✓ | ✓ | `fll` | ✓ (gallery phase 3) |
| LoRa satellites (TinyGS-class, 400–437 MHz) | ✓ | ✓ | demod-tracked | ✗ gap, and **not recommended**: LoRa is identify-only (§3.8, patent uncertainty) |
| Geostationary (GOES HRIT, GK-2A LRIT, Inmarsat EGC) | — (fixed) | — | — | ✗ not covered; tier 3, needs a dish or patch antenna |

**Gaps in the satellite briefs (to add there, not here):**
1. **The `sats.json` `mode` values.** Only `NFM`, `AFSK1200` and `LRPT` appear. It needs at least `GMSK9600`/`FSK9600`,
   `BPSK1200` (FUNcube), `BPSK`/`QPSK` with a symbol rate, `CW` and `HRPT`, plus a **framing** field (AX.25, CCSDS,
   FUNcube, per-satellite custom). SatNOGS DB supplies mode and baud, but not a framing our decoder can act on.
2. **Generic CubeSat telemetry decoding.** No brief covers per-satellite frame formats or turning telemetry into
   readable values. References: gr-satellites (GPL-3, black box only) and SatNOGS DB's decoders/Kaitai structs
   (licence to check).
3. **"Demod-tracked" as an explicit `loop` kind.** `costas` is written for LRPT. GMSK, BPSK and LoRa demodulators also
   do their own fine tracking and want feed-forward only. The behaviour is the same, but it should be named generally.
4. **Background decoding is clock-scheduled, not pass-triggered.** `BACKGROUND-DECODING.md` profiles are UTC time windows;
   RADIO-ROLES' scheduler has pass windows. A **pass-triggered background VFO** (an ISS APRS/SSTV pass on a radio that is
   otherwise idle, inside the in-range rule) is the missing link between the two engines.
5. **Rotator-driven tracking for dish passes** (HRPT, and later high-gain Yagis). `ANTENNA-SWITCH-ROTATOR.md` drives
   `rotctld` by azimuth for UberSDR and VibeServer, but has no elevation and no pass track.
6. **Wide captures:** HRPT needs about 3 MS/s and an L-band front end; nothing gates it on capture bandwidth yet (the same
   rule as DAB/ADS-B in §6.4).
7. **9k6 on the shared dial:** a 9600 Bd GMSK channel is about 15–20 kHz wide plus ±10 kHz of Doppler. Check it fits the
   channel's IF on a narrow (Airspy HF+ 660 kHz-class) capture before offering it.

### 3.8 Utility VHF/UHF

| Decoder | Where | Standard | References | Diff. | CPU | Tier |
|---|---|---|---|---|---|---|
| EPIRB / PLB / ELT 406 | 406.025–406.076 MHz, 400 bps BPSK, 520 ms bursts every ~50 s | Cospas-Sarsat T.001 ✓ (free) | dec406 (F4EHY, GPL?), SDRangel? | M | trivial | 3 (rarely heard; worth having as a background watch) |
| 121.5 MHz homing tone | 121.5 / 243 MHz | swept-tone AM | — | S (identify) | trivial | tags |
| rtl_433 ISM sensors | 433.92, 868, 315, 915 MHz | RE, 200+ device protocols | rtl_433 (GPL-2) | XL to match; S/M per family | light | 3: curated subset (weather stations only), §4.3 for TPMS |
| LoRa / Meshtastic | 433/868/915 MHz | Semtech CSS, patented; Meshtastic protocol open (GPL-3) | gr-lora, gr-lora_sdr (GPL-3) | L | medium | identify only ⚖ (patent status of a software receiver uncertain) |
| wM-Bus (meters) | 868.95 MHz | EN 13757-4 $ | rtl-wmbus (BSD-2?) | M | light | 3, ⚖ privacy (consumption reveals occupancy) |
| P25 / DMR Tier III / TETRA / NXDN trunking control channels | 380–470, 800 MHz | TIA-102 $, ETSI TS 102 361 ✓, EN 300 392 ✓ | OP25 (GPL-3), dsd-fme, sdrtrunk (GPL-3), osmo-tetra (AGPL) | L | light-medium | ⚖ system identity only, admin opt-in (§4.2) |
| GSM / LTE / 5G | — | — | gr-gsm etc. | — | — | ✗ excluded |
| Tyre pressure (TPMS), car keys | 315/433 MHz | RE | rtl_433 | — | — | ✗ (vehicle tracking, key capture) |

### 3.9 Time signals

| Station | Where | Format | Have | Diff. | Tier |
|---|---|---|---|---|---|
| MSF, DCF77, WWVB, WWV/WWVH, RWM | — | — | ✓ | — | have (audit bugs 6–9 open) |
| **JJY** | 40 kHz (Ohtakadoya-yama), 60 kHz (Hagane-yama) | NICT published ✓; PWM 0.2/0.5/0.8 s, like WWVB's shape | — | S (reuse the WWVB envelope branch, own bit map) | **1** |
| **BPC** | 68.5 kHz (Shangqiu) | China NTSC; format documented by third parties (four-level, 20 s frames); official document availability uncertain | — | S/M | **1** (big audience in Asia) |
| **CHU** | 3.330, 7.850, 14.670 MHz | NRC Canada ✓; Bell 103 FSK 300 Bd time code in seconds 31–39 | — | S (300 Bd FSK on top of the FSK decoder) | **1** |
| **TDF** (ALS162) | 162 kHz (Allouis) | ANFR/LNE published ✓; phase modulation of the carrier | — | M (needs a carrier PLL: lands with SAM) | **1** after SAM |
| DCF77 phase code | 77.5 kHz | PTB published ✓ | — | M | 3 |
| HBG, Teleswitch | gone / 198 kHz BBC R4 LW data | — | — | — | — |

---

## 3A. Short notes on each tier-1 and tier-2 candidate

### Tier 1

- **CW.** Build the decoder our own way: a narrow matched filter at the peak, adaptive threshold, dit/dah timing by
  k-means on the mark lengths, a Farnsworth-tolerant decision, and a per-character confidence. That gives the same
  high/medium/low/poor grading our UberSDR path already shows (`MorseQuality`), so one client UI serves both. Later,
  put N of these on the `Channelizer` for a skimmer, sending spots, not text, to the CW spots map we already draw.
  ITU-R M.1677 defines the code.
- **DSC.** The FSK decoder already does 100 Bd/170 Hz. DSC adds the 10-bit symbols (7 information bits plus a 3-bit
  count of B-bits), the DX/RX time diversity (each symbol sent twice, 4 symbols apart), the format/category/MMSI/
  nature-of-distress fields from M.493, and the ECC check. Show distress alerts plainly, with position (if sent),
  MMSI → country from the MID table (ITU MARS, free), and "This is a real distress alert. You cannot help by radio;
  coastguards have it." Background profile: put DSC beside NAVTEX (2187.5 sits inside a 500 kHz-centred MF capture only
  on wideband radios; on HF the 6 frequencies want one VFO each). VHF DSC shares the 1200 Bd AFSK modem with APRS.
- **WSPR.** A 110.6 s transmission, 4-FSK at 1.4648 Bd, 1.4648 Hz tone spacing, starting 1 s after an even minute.
  The convolutional code is K=32, r=½, with sequential (Fano) decoding. The callsign/locator/power packing is in K1JT's
  notes. The output shape matches FT8's summary cards (spots, countries, furthest, by band), so the gallery code is
  reused nearly whole. ★ It is also the best antenna-comparison tool we can give an owner: a multi-band background
  profile on a locked HF radio.
- **PSK31/63.** BPSK with raised-cosine shaping, varicode, and an AFC. The idle signal (continuous phase reversals) makes
  two tones 31.25 Hz apart, which is also its classifier signature (§5). QPSK31 adds a K=5 Viterbi. Later work:
  multi-channel "PSK browser" on the channelizer, as fldigi's signal browser does.
- **APRS/AX.25.** Bell 202 AFSK (1200/2200 Hz) with NRZI, bit-stuffing, HDLC flags and the FCS-16. The weak-signal gain
  comes from running several demodulators at different tone-balance settings and keeping any frame that passes the
  FCS (direwolf's approach; describe and reimplement, never copy). Parse APRS positions, weather and messages; plot on
  the existing MapLibre map; gallery as a daily station log. HF 300 Bd is the same stack at 1600/1800 Hz (200 Hz
  shift).
- **AIS.** Two 25 kHz channels at 161.975/162.025 MHz: cut both from one capture with the `Channelizer`. GMSK
  BT 0.4, 9600 Bd, NRZI, HDLC, CRC-16, then the M.1371 message table. The AIS slot structure is open data. Publish
  ships on the map with MMSI → flag. It is ideal in the background on a coastal server.
- **SAM / ECSS / AM stereo.** As planned in memory `sam_ecss_cquam_plan`: a carrier PLL in `RxPipeline` (DSB, U, L),
  ECSS presented as "tune the carrier, choose the sideband", and C-QUAM-compatible stereo with the 25 Hz pilot. The
  label must say "AM Stereo (C-QUAM compatible)": the name is a Motorola trademark. TDF time code needs the same PLL.
- **CTCSS/DCS.** A Goertzel bank on NFM audio below 300 Hz (50 standard tones, ±1 % discrimination). DCS: 134.4 bps NRZ
  with a 23-bit Golay code word, normal or inverted. Show "CTCSS 88.5 Hz" in the VTS line as a quiet suffix. This also
  helps the squelch (tone squelch is a natural follow-on).
- **More time stations.** JJY and BPC reuse the LF envelope path. CHU needs the 300 Bd FSK decoder. Fix audit items 6–9
  first, because WWV/WWVB report a minute late today.

### Tier 2

- **ACARS.** MSK 2400 bps on an AM carrier; several channels at once on one capture (Europe 131.525/131.725/131.825,
  US 129.125–131.550). Channelizer plus N light demodulators. Message labels/sublabels are well documented publicly
  (ARINC 618 itself is paid). Aircraft registration → type from public registries only.
- **VDL2.** Harder: D8PSK at 10.5 ksym/s, a training sequence, Reed–Solomon, then the AVLC and ACARS-over-AVLC layers.
  In Europe it carries more traffic than plain ACARS. ICAO Doc 9776 is the spec (paid). dumpvdl2 is the black-box
  reference.
- **ADS-B 1090.** Needs ≥ 2 MS/s at 1090 MHz, so it takes a whole radio. It is a **radio role** (`RADIO-ROLES.md`
  "Dedicated"), not a VFO. RTL-SDR and SDRplay can; the Airspy HF+ cannot reach 1090 MHz at all, so the role is offered
  only where the radio's tuning range and rate allow it (gate on capability, not driver name). Output: aircraft table +
  map, a Beast/SBS feed (optional, so owners can feed tar1090 or FlightAware), and gallery daily stats ("412 aircraft,
  furthest 380 km").
- **HFDL (our own).** Replaces our reliance on an UberSDR. 1800 Bd PSK (BPSK/QPSK/8PSK by data rate), 32 s frames of
  13 slots, a known preamble and an adaptive equaliser. It wants many channels: the ground stations' frequency
  list ("systable") is published by the stations themselves in the squitters. A Pi 2 can probably manage only a few
  channels; a modern box can do dozens. Measure.
- **Radiosondes.** Hugely popular (sonde hunting), legal (weather balloons), and background-perfect: launches are twice
  a day at fixed times, at known sites. RS41 first.
- **FT8 multi-pass and JS8.** The audit's FT8 items (subtraction, 2–3 passes, capture-timestamped slots) come first.
  JS8 then reuses the same core.
- **FST4/FST4W, Q65, MSK144.** The algorithms are in the WSJT-X team's published papers. Implement from those, test
  against WSJT-X as a black box. Low audience; do them once FT8's core is shared.
- **MFSK family + Hell.** One MFSK core (FFT tone detector, Gray code, interleaver, convolutional/Walsh codes) serves
  MFSK16/32, Olivia, Contestia, Thor and Domino. An AUTO detector (tone count and spacing from the spectrum) matters
  more here than for RTTY, because there are dozens of submodes. Hellschreiber is drawn, not decoded: render columns to
  an image strip; no character recognition needed.
- **FreeDV + M17.** Codec2 is LGPL-2.1, so link it **dynamically on the server** (Linux .deb, macOS .app, Android
  .so); the server decodes to PCM and sends ordinary Opus. No codec reaches the iOS app, so the LGPL relinking clause
  never bites in the App Store. FreeDV 700D/700E/1600 first; RADE is ML-based and probably too heavy for a Pi 2
  (measure). M17 voice is legal, open and growing, the one digital-voice system we can fully offer.
- **Amateur DMR/D-STAR/YSF/NXDN signalling.** See §4.2: callsigns/IDs, talkgroups, slot activity, GPS and SMS data,
  D-STAR slow data (text, D-PRS positions), YSF callsign headers. No voice. Present it as "who is on this repeater".
- **ALE 2G.** 8-FSK 125 Bd tones 750–2500 Hz, Golay (24,12) and triple redundancy, a word parser (TO/TIS/TWS/DATA…),
  and a 2G scan list in the background. Popular with utility listeners; the KiwiSDR ALE extension shows the demand.
- **Meteor LRPT.** As already briefed in `DECODERS-GALLERY.md`.
- **DRM.** See §4.4.
- **SELCAL and EAS/SAME.** Both S; SELCAL makes HF aero voice more fun (name the aircraft from public SELCAL
  registries), and SAME alerts matter to US owners near NOAA Weather Radio.

---

## 4. Exclusions and legal notes

### 4.1 Hard exclusions (do not propose)

| What | Why |
|---|---|
| **HD Radio / NRSC-5** | Xperi-owned, proprietary HDC codec, licensed receivers. Declined (memory `hd_radio_declined`). |
| **AMBE / AMBE+2 voice** (DMR, D-STAR, YSF DN/VW, NXDN, dPMR, P25 Phase 2, Iridium voice) | DVSI's codecs. AMBE+2 is still patented: one source gives US 8,359,197 to 2028-05-20 (not verified against USPTO). mbelib-style decoders exist (ISC licence) but the patent risk is the user's, and a public server decoding for others is the worst position to be in. |
| **TETRA voice** | ACELP is licensed technology (originally Thomson-CSF, now Thales) on FRAND terms (ETSI EN 300 395-2, 1997 text). ETSI publishes reference C code with the standard, but that does not grant a patent licence. Most TETRA networks are encrypted anyway (TEA1–4). |
| **Anything encrypted** | Never attempt decryption. A signal flagged as encrypted shows "encrypted" and nothing more. |
| **Pay-TV, conditional access** | Never. |
| **Iridium content, mobile phone content** (GSM/LTE/5G), cordless phones | Private communications; illegal to intercept content in the UK and the US. |
| **Vehicle TPMS, car key fobs** | Vehicle tracking and key capture. |
| **TV on servers, TV over the internet** | Declined by Stuart, 2026-10-23 (§4.5): broadcaster retransmission rights, codecs, bandwidth. TV is Android-local only. |

### 4.2 Digital voice systems: what we may offer without the codec

| System | Voice codec | Patent status of the codec | What we can offer |
|---|---|---|---|
| **M17** | Codec2 3200 | Open; Codec2 was designed to be patent-free (LGPL-2.1) | **everything, including voice** |
| **FreeDV** | Codec2 / LPCNet / RADE | Open (LGPL / BSD-style) | **everything, including voice** |
| **P25 Phase 1** | IMBE | Secondary sources say the IMBE patents (early-to-mid 1990s) have expired; not verified against the patent records | Technically decodable. **Recommendation: still do not offer P25 voice.** P25 is public-safety radio; the interception law (§4.3), not the patent, is the blocker. Signalling: ⚖ system identity only, opt-in. |
| **P25 Phase 2, DMR, D-STAR, YSF, NXDN, dPMR** | AMBE / AMBE+2 | Patented (✗) | **Signalling and data only:** for amateur use — callsigns/radio IDs, talkgroups, colour code, slot activity, GPS (DMR LRRP, D-PRS), SMS/short data, D-STAR slow-data text. For non-amateur systems — only the system type, colour code, network/site identity, encrypted or clear, and nothing addressed to individuals; ⚖ opt-in. |
| **TETRA** | ACELP | Licensed (✗) | ⚖ opt-in: network identity (MCC/MNC, location area, colour code), encryption class, from the broadcast channels only. |
| **DRM** | AAC, xHE-AAC, CELP, HVXC | §4.4 | §4.4 |

★ **Why the amateur/non-amateur split.** Amateur transmissions are, by licence condition, meant for anyone to hear and
must not be encrypted, so their callsigns and talkgroups are fair to show. A professional system's unit IDs and
talkgroups are "the sender or addressee of a message" in UK law (§4.3) even when no voice is decoded. Decide amateur
vs non-amateur from the frequency (inside the amateur bands, from the band plan) and from the content (a callsign in
the header), never from a user's claim.

### 4.3 Interception law: who may decode what ⚖

- **UK.** Wireless Telegraphy Act 2006, s.48: it is an offence to use apparatus "with intent to obtain information as
  to the contents, sender or addressee of any message" that neither you nor anyone you act for is authorised to receive,
  or to disclose such information. Broadcasts, amateur traffic, safety broadcasts (NAVTEX, WEFAX, weather RTTY, DSC
  distress) and transmissions intended for general reception are the accepted safe ground. Pagers, professional PMR,
  trunked radio, Iridium, ACARS and AIS are not "intended for you", although ACARS/ADS-B/AIS reception is widely
  practised and openly published (Ofcom has historically focused on disclosure and use). ⚖ That is a grey area; take
  advice before the gallery publishes ACARS message bodies.
- **US.** ECPA, 18 U.S.C. §2511. §2511(2)(g)(ii) permits intercepting radio "readily accessible to the general public":
  amateur, broadcast, marine and aeronautical mobile, public-safety radio that is not scrambled or encrypted. Display
  pagers, cellular and cordless telephony are protected (tone-only pagers are excluded from the definition in §2510(12)).
  Some states restrict scanners in vehicles.
- **Elsewhere.** Many EU countries have laws close to the UK's (for example Germany's TKG on the secrecy of
  communications, which also bans sharing what was heard). Some countries forbid owning scanners at all.
- **Our recommended handling:**
  1. **Default on:** broadcast, amateur, time, weather, maritime safety (NAVTEX, DSC, WEFAX), aviation weather and
     aircraft position (ADS-B is a broadcast by design, ICAO), AIS (a broadcast collision-avoidance system), radiosondes.
  2. **Admin opt-in, off by default, with a jurisdiction notice:** POCSAG/FLEX, non-amateur trunking identity, ACARS/
     VDL2/HFDL **message bodies** (positions and flight IDs on by default; free-text bodies behind the opt-in), wM-Bus.
     The setup page text: "Decoding pager messages is illegal in the UK and many other countries, and restricted in
     the US. Only switch this on if the law where this server is allows it. Messages are shown only to the admin." An
     opted-in decoder is admin-only by default, never public, and never in the public gallery.
  3. **"Format, not content"** where the format is interesting and the content is not ours: identify the signal (tag:
     "POCSAG 1200", "TETRA, encrypted", "PACTOR-III", "STANAG 4285") without decoding it. This is the default for every ⚖
     item while the opt-in is off.
  4. **Never:** §4.1.

### 4.4 DRM and its codecs ⚖

- The OFDM layer (ETSI ES 201 980) is free and we can build it ourselves.
- **Audio:** DRM30 uses AAC (HE-AAC v2) on older services and **xHE-AAC (USAC)** on newer ones; CELP and HVXC are rare.
  The SDC declares the codec before any audio is needed.
- **The position already decided by Stuart:** we ship no AAC decoder. As with DAB+ (`vibe_dab_aacdec.h`), the
  **server OS's** decoder does the decoding (AudioToolbox, MediaCodec; on Linux whatever FFmpeg the system provides). DRM
  is offered only where that server can decode the station's codec, and a station whose codec it cannot decode shows
  its metadata with "This station uses xHE-AAC. This server's system has no decoder for it, so the audio cannot be
  played; the signal is being received and decoded." xHE-AAC: Apple has `kAudioFormatMPEGD_USAC` (iOS 13+/macOS 10.15+);
  Android has an xHE-AAC decoder from API 28/29 (check per device); Linux FFmpeg gained a native USAC decoder in 2024
  (7.1). Whether the FFmpeg build on the box is licensed is the distributor's concern, not ours, because we link none
  (as for DAB+ today). ⚖ Check that the Linux route really keeps us out of it; Via LA administers an xHE-AAC pool.
- **Reference:** Dream (GPL-2), which uses FAAD2/fdk-aac. Black box only.
- CPU: the OFDM demodulation of a 10 kHz DRM30 signal is light-to-medium on a Pi 2 (FFT of 288/256/176/112 bins per
  symbol, channel estimation, MLC with Viterbi). The audio decode is the OS's.

### 4.5 Television: Android local hardware only (nice-to-have, lowest tier)

> **"An Android phone with an SDR attached functions like a portable TV of old, but with advanced signal analysis."**
> (Stuart, 2026-10-23)

In short, there are two halves, side by side on one screen:
- **watching** a channel: analogue where it still exists, and DVB-T/T2;
- **the analysis**: MER; BER before and after Viterbi (DVB-T) or LDPC (T2); the constellation; the channel impulse
  response, an echo profile showing multipath and SFN echoes; the TPS or L1 parameters; the multiplex and service list;
  the transmitter and its distance; and a reception log for TV DX.

**Final scope (Stuart, 2026-10-23; supersedes every earlier TV note):**
- TV is a **nice-to-have**, and it stays **off the servers**. It is a feature of the **Android app's own local hardware**
  only: an **SDRplay RSP (10 MS/s), Airspy R2 (10 MS/s) or HackRF (up to 20 MS/s, 8-bit)** on USB OTG, demodulated and
  shown on the phone. The pitch is **TV DX on the phone**: portable hunting for distant multiplexes.
- **Removed:**
  - server-side TV of any kind;
  - LAN streaming from a server (the "Sky Q main box → Mini" model; precedents such as Sky Q, HDHomeRun, PVR home
    streaming and SAT>IP (CENELEC EN 50585) exist, but it is not our scope);
  - a server background "TV band scan";
  - RTL-SDR multiplex identification on servers.
- **Not supported radios:** RTL-SDR (including its native DVB-T mode through the Linux DVB drivers), Airspy Mini (6 MS/s,
  too narrow), Airspy HF+.
- ✗ **Internet streaming of TV: declined, not deferred.** Retransmitting broadcast TV to remote viewers is a
  broadcaster rights and licensing issue: the BBC would not accept a radio server sending BBC One to viewers in the US,
  and the UK CDPA 1988 s.20 (communication to the public) makes unlicensed internet retransmission an infringement. It
  would also mean making H.264 for others, and it costs 13–23 times a listener's stream (below).
- **Parked (one line):** sharing *multiplex identification* (metadata, not programme content) from a server over the
  internet.
- Radio audio relaying is a separate, existing question and out of scope here.
- Encrypted or pay-TV: never.

**How it works: the analysis (identification and measurement) first, because it also works on signals too weak to watch; then watching.**

1. **What the phone can measure from the air (EN 300 744, DVB-T):**
   - **Mode and guard interval** from cyclic-prefix autocorrelation. The peak lag is the useful symbol length: 224 µs for
     2K, 896 µs for 8K. The guard (1/4…1/32) comes from the correlation's period.
   - **TPS.** Every OFDM frame (68 symbols) carries the same 68-bit DBPSK TPS block on every TPS carrier: 17 carriers in
     2K, 68 in 8K, spread across the band. It holds:
     - sync and length;
     - frame number;
     - constellation;
     - hierarchy;
     - HP/LP code rates;
     - guard and mode;
     - **the 16-bit cell_id** (8 bits per frame, MSB in odd frames and LSB in even);
     - DVB-H signalling;
     - BCH protection.

     Because TPS is **differential** across symbols on the same carrier, no channel estimate is needed. TPS lock is fast
     (two frames, about 0.15–0.25 s in 8K), so a UHF sweep of channels 21–48 is quick.
   - **MER, pre/post-Viterbi BER, uncorrected RS packets, the constellation**, and the **channel impulse response from the
     scattered pilots**. DXers value that last one: in an SFN it shows each transmitter's echo, and the delay converts
     to a path difference.
   - **Whether a transmitter sets cell_id:** not established. No public list of UK or German values was found (UK
     scan-file evidence shows network_id, e.g. Waltham 12323, and ONID 9018 for the UK, not cell_id). Measure on air;
     treat cell_id as a bonus where present.
   - **DVB-T2.** The P1 symbol (1K FFT with a C-A-B time structure) is detectable, and S1/S2 give SISO/MISO, T2 or
     T2-Lite, and the FFT size. The L1-pre (cell_id, network_id, T2_system_id) is LDPC-coded across P2's carriers and
     needs a full-band demodulation, which all three supported radios can do.
   - **Partial-band note (for the record, since RTL-SDR is out of scope).** At 2.4 MS/s, resampled to 2.2857 MS/s
     (= 9.142857/4), a 2048-point FFT lands exactly on the 8K carrier spacing. The slice then holds about 20 TPS carriers
     and about 56 continual pilots, so mode, guard and TPS are readable from a partial band. T2's L1-pre is probably not
     (unverified).

2. **What the phone can read with a full demodulation (all three radios):**
   - PAT/PMT;
   - **NIT**: network name, network_id, the terrestrial delivery descriptor, the cell list, LCNs;
   - **SDT**: service and provider names;
   - EIT now/next;
   - TDT/TOT: time and the local offset, a country hint.

   ONID → country comes from ETSI TS 101 162 (the DVB Services registry, dvbservices.com).

3. **What is looked up, not measured.** The transmitter comes from the channel, the phone's own location and the
   identifiers above, matched against a transmitter database. Each source needs its own licence check, as was done for DAB
   (`docs/DAB-SITES-SOURCES.md`):
   - US: FCC LMS/CDBS (public domain);
   - Australia: the ACMA register (open data, check the terms);
   - Netherlands: Agentschap Telecom's antenna register;
   - UK: Ofcom's published DTT data (check whether it is under the Open Government Licence);
   - ukfree.tv, mb21 and fmscan (copyright; ask for permission; facts only);
   - Wikipedia (facts only).

   The UI marks each field: **measured** (mode, constellation, code rate, guard, MER, BER, cell_id; network and service
   names from SI) vs **looked up** (the transmitter, its distance and bearing, "carries …"). For example:
   "UHF 40 · DVB-T 8K 64QAM 2/3 GI 1/32 (measured) · PSB1 (BBC A) (SI) · Sutton Coldfield, 38 km (looked up)".

4. **The DX log**, kept on the phone: time, channel, measured parameters, SI names, transmitter and distance, MER, and
   one frame grab where video is decoded. It can be exported as CSV. Analogue extras: teletext header and 8/30 CNI or VPS
   network codes, and test-card frame grabs.

5. **Watching.** The phone demultiplexes one service and hands the elementary streams to **Android
   `MediaCodec`** (H.264 everywhere; HEVC on most; MPEG-2 on many but not all SoCs). The device's licensed decoder does
   the codec work, the same posture as DAB+ AAC. MPEG-2 video's patents have expired (unlike H.264, still in Via LA's
   pool), so where `MediaCodec` lacks MPEG-2 we *could* decode UK SD ourselves, as we do MP2 for DAB. Audio: MP2 is
   ours; HE-AAC goes through MediaCodec. Analogue PAL/NTSC/SECAM (AM-VSB vision, FM sound, NICAM: a 1980s BBC design,
   with no live patents found) has no codec issue at all; at 10 MS/s it fits vision plus sound. Amateur DATV (DVB-S/S2
   reduced-bandwidth TV via QO-100 or terrestrial) is light and comes almost free with the DVB-S demodulator.

**Which standards fit the three radios:**

| Standard | Needs | RSP 10 MS/s | Airspy R2 10 MS/s | HackRF ≤ 20 MS/s |
|---|---|---|---|---|
| DVB-T / DVB-T2 8 MHz (7.61 MHz occupied; T2 extended 7.77) | ≥ 9.14 MS/s | ✓ (tight margin) | ✓ (tight) | ✓ (comfortable at 12–16 MS/s) |
| DVB-T 7 MHz, ATSC 1.0, ISDB-T, NTSC (6 MHz) | ≥ 6.5–8 MS/s | ✓ | ✓ | ✓ |
| Analogue PAL I/BG (vision + FM sound + NICAM spans ~7.5 MHz) | ~8–10 MS/s | ✓ | ✓ | ✓ |
| DVB-S/S2 RB-TV (125 kS/s–2 MS/s) | ≤ 3 MS/s | ✓ | ✓ | ✓ |

**Android cost and floor:**
- **USB.** These figures are from the radios' documentation, not measured:
  - HackRF at 10 MS/s is 20 MB/s (8-bit IQ); at 20 MS/s it is 40 MB/s, which many phones drop.
  - Airspy R2 at 10 MS/s is about 30 MB/s packed.
  - RSP at 10 MS/s: about 20–40 MB/s depending on bit depth (unverified).

  USB 2.0 OTG realistically gives 30–40 MB/s on a good phone. Measure with a throughput probe before the TV key is shown.
- **CPU (estimated, mid-range arm64 phone with Cortex-A78-class big cores, DVB-T 8K 64QAM 2/3).** Ingest and conversion
  take about 5 % of a big core. The 8K FFT, channel estimation and equaliser take about 15–25 %. A NEON Viterbi for
  ~24 Mb/s output takes 15–30 %. RS(204,188) is cheap. In total that is **about 1 to 1.5 big cores**. DVB-T2's LDPC is
  another 1–2 cores (less certain). With video decoded by `MediaCodec`, the screen costs almost nothing. Battery and heat
  are the real limits, with the radio drawing from OTG as well.
- **Floor:** arm64, four Cortex-A76-class or better cores, Android 10+, 4 GB RAM. **The 32-bit 1 GB phones cannot do it**
  (neither the USB rate nor the CPU). Show no TV key there, and none on a radio that is not one of the three
  (AGENTS.md).
- **For comparison:**
  - A listener's stream today is about 150–270 kb/s: Opus at 64 kb/s mono or 96 kb/s stereo (`g_vsOpusBitrate`,
    `opusBitrateFor`), the spectrum at ~80 kb/s at 10 fps up to ~160 kb/s at 20 fps (`docs/WIRE-AUDIT-2026-10-10.md`),
    and ~10 kb/s of meters.
  - Twitch's 720p H.264 is ~3.5 Mb/s, 13–23 times that.
  - So a 10–20 Mb/s home upload could carry 3–5 viewers and a 4G holiday server about one. That is part of why internet
    TV was declined.

**HackRF status (a correction to "not supported").** HackRF is **already in the tree as EXPERIMENTAL**:
- `android/app/src/main/cpp/hackrf_source.{h,cpp}`;
- a vendored, patched `libhackrf` with an Android fd entry point (commits fa77237d, eb86ade9);
- manual gain only, no VibeAGC.

It **has been tested on real hardware by Saber** (Stuart, 2026-10-23); the "never run" note in the code comments is out of date. `hackrf.c` itself carries a BSD-3-Clause header, though the directory's
`COPYING` file is GPL-2 (upstream's repository-wide file). Worth tidying, so the vendored library's licence is stated
unambiguously. What TV needs from it: a 10–16 MS/s path through the DSP thread, and the Android
throughput probe.

**Effort:**
- the analysis (TPS/CP, MER/BER, constellation, echo profile, SI read, log): L;
- watching DVB-T/T2: XL;
- analogue: M/L;
- DATV: L.

Open-source references only: SDRangel ATV/DATV (GPL-3), SDR++ `atv_decoder` (GPL-3), leansdr/leandvb (GPL-3),
gr-dvbt and GNU Radio gr-dtv (GPL-3), the BATC Portsdown/MiniTiouner software. MadPsy's browser SDR decoding TV from an
SDRplay is a driver test bench, not a product, but it shows that the bandwidth is enough.

---

## 5. Signal identification and tags

### 5.1 What users want, and what must not happen

- A Discord request asked for OpenWebRX+-style digital-mode markings on the top bar (WSPR, SSTV, FT8 spots).
- Stuart said no to bookmarks on the top row because, on his OpenWebRX, they overlapped and became unreadable.
- So: tags must tell a listener "that is RTTY / FT8 / DRM / NAVTEX" at a glance, **never overlap**, never cover the VTS
  or the search box, and a tap must open the right decoder (or the sigidwiki page where we have none).

### 5.2 Two layers, merged into one tag per signal

**Layer A: frequency-based (cheap, static, always available).**
- **Sources:**
  - IARU Region 1/2/3 band plans for the sub-bands;
  - the WSJT-X/JS8/WSPR dial frequencies listed in §3.2 (facts, typed in ourselves, not copied from GPL tables);
  - NAVTEX 490/518/4209.5; DSC; the aeronautical and maritime allocations from the ITU Radio Regulations Appendix 17/18
    and public channel lists;
  - HFDL ground-station frequencies (public squitter data);
  - WEFAX/RTTY station schedules (`docs/WEFAX-WORLD-STATIONS.md`, DWD/NWS published schedules);
  - time stations;
  - **our EiBi data** for broadcasters (already on the server).
- **Shape:** a generated table (`scripts/gen-*`, one rule for both readers, like the NAVTEX stations) of
  `{lo, hi, kind, label, decoderId, region, sigidwiki}`.
- **It says "this is where X lives", not "X is here".** It is drawn as a faint segment under the spectrum, not as a pill
  (§5.4), unless layer B confirms it.

**Layer B: content-based classification (on the server, from the spectrum and IQ).** Two stages, so the Pi 2 pays
almost nothing:

1. **Stage 1, the spectrum features (always on while anyone is watching, ~1 Hz).** It reuses the spectrum governor's
   FFT. **No new FFT**: the server already computes it for the waterfall. Per frame: noise floor, peak list, and for
   each peak its occupied bandwidth, edge steepness, flatness, the number of discrete tones and their spacing, and a
   few seconds' history of on/off and tone alternation. Cheap rules give a candidate class and a confidence:

   | Signature | Rule | Candidate |
   |---|---|---|
   | Two tones a fixed 170/200/425/450/850 Hz apart, alternating | the approved FSK detector; element length 22 ms → 45.45 Bd, 20 ms → 50 Bd, 10 ms → 100 Bd | RTTY / NAVTEX / SITOR / DSC (frequency layer decides) |
   | 8 tones at 6.25 Hz spacing, 50 Hz wide, starting at :00/:15/:30/:45 ±1 s | time-slot cadence is decisive | FT8 (7.5 s slots, 83 Hz wide: FT4) |
   | ~6 Hz wide, two-minute cadence on an even minute | | WSPR |
   | Narrow carrier, on/off keyed, 20–200 ms elements | | CW (also beacons, RWM, NDBs) |
   | Two lines 31.25 Hz apart (idle PSK31), ~60 Hz wide busy | | PSK31 (62.5 Hz: PSK63) |
   | Flat-topped block, steep edges, 4.5/5/9/10/18/20 kHz wide, on a 5 kHz/9 kHz raster | | DRM (confirmed in stage 2) |
   | Flat block ~2.4–3 kHz centred near 1800 Hz audio, continuous | | STANAG 4285 / MIL-110 / HFDL (frequency layer splits) |
   | 8 tones at 250 Hz spacing, 750–2500 Hz, short bursts | | ALE 2G |
   | Tone-count/spacing of 8–64 tones, 125–2000 Hz | | Olivia/Contestia/MFSK |
   | 1500–2300 Hz sweep with 1200 Hz sync pulses | | SSTV (VIS confirms) |
   | 1500/2300 Hz with a long 300 Hz-wide continuous line, start tone 300/450 Hz | | WEFAX |
   | Linear sweeps repeating every 0.5–1 s across 10–100 kHz | | CODAR / OTH radar |
   | Linear chirps in 125/250/500 kHz on 433/868/915 MHz | | LoRa (identify only) |
   | 25 kHz channel, 2 × FSK tones ±2.4 kHz, slot timing 26.67 ms | | AIS (frequency confirms) |
   | 12.5 kHz, 4-FSK, 30 ms TDMA bursts | | DMR (TDMA bursts), dPMR/NXDN (continuous) |

2. **Stage 2, IQ confirmation (only for the top-K candidates, K = 1 on a Pi 2, 4+ on a fast box).** Cut each candidate
   with the existing `Channelizer` and run a short, specific test:
   - **OFDM:** cyclic-prefix autocorrelation gives the useful symbol length, and that names the DRM robustness mode
     (A 24 ms, B 21.33 ms, C 14.66 ms, D 9.33 ms). DAB, DVB-T and Wi-Fi are recognised the same way.
   - **PSK order:** the M-th power spectrum (a line at M × carrier) gives BPSK/QPSK/8PSK and the symbol rate from the
     envelope's cyclic frequency (1800 Bd HFDL vs 2400 Bd STANAG 4285).
   - **FSK:** the shift and baud precisely (RttyAuto already does this).
   - **Known preambles:** the FT8 Costas array, the SSTV VIS, the STANAG 4285 probe period, the VDL2 training sequence,
     the ALE word sync.

   A stage-2 pass is a fraction of a second of IQ and runs on the decoder-priority thread (Network > Audio > Spectrum
   > Decoders). If the box is busy, it waits; it never delays audio.

**The strongest evidence is a decoder that decoded.** When a decoder is running (a listener's, or a background VFO),
its output overrides both layers: "FT8 · 23 decodes", "NAVTEX · B Oostende". So tag confidence has three grades:

| Grade | Source | Drawn as |
|---|---|---|
| **Confirmed** | a decoder produced valid output (CRC/parity passed) | solid pill |
| **Detected** | stage 2 confirmed, or stage 1 ≥ 0.85 with the frequency layer agreeing | solid pill, no "?" |
| **Likely** | stage 1 only, 0.6–0.85, or the frequency layer alone with a signal present | pill with "?" ("RTTY?") |
| below 0.6 | — | not drawn |

**Hysteresis:** a tag needs 3 consecutive agreeing stage-1 frames to appear and is held for one full cycle of its mode
after the signal goes (FT8 30 s, WSPR 4 min, otherwise 10 s), so slotted modes do not blink.

**Open classifiers, as references only:**
- sigidwiki.com is the signature catalogue (we link to it, never copy it; memory `chat_answers_sigid_wiki`);
- Artemis (AresValley, GPL-3) packages the sigidwiki data for offline identification;
- TorchSig (Apache-2.0) and DeepSig's RadioML datasets (CC BY-NC-SA, non-commercial) are ML references.

An ML classifier is not proposed for the Pi 2. Rules plus stage-2 tests are explainable, cheap and testable on our
recorded library (`~/VibeSDR-test-audio`, the UberSDR live source). A small model could come later for fast boxes
only, gated by the benchmark.

**CPU on a Pi 2.** Stage 1 is a pass over peaks already found (tens of microseconds a frame) plus a small per-peak
history: < 1 % of a core at 1 Hz. Stage 2 at K = 1, a 0.5 s window every few seconds, is a few percent while a candidate
is unresolved and nothing once it is. Budget: **≤ 3 % of one core on a Pi 2**, enforced by a governor that drops
stage 2 first. Measure with a new `vibe_benchmark_decoders.h` row.

**Where it runs, and the wire.** On the server, per radio (one classifier per capture, not per listener, so ten
listeners cost the same as one). It is sent on the spectrum socket as a small `sigtags` message at most once a second,
and only when something changed:

```
{"type":"sigtags","v":1,"tags":[{"id":17,"hz":14080350,"bw":2400,"k":"rtty","lbl":"RTTY 45/170","g":"detected","c":0.91,
  "dec":"rtty","p":{"baud":45.45,"shift":170}}, …],"gone":[12]}
```

That is ~100–400 bytes, against the 10 KB/s spectrum frame. `dec` is present only if this server offers that decoder
on this radio, so the tap never lands on a dead control. `p` carries what stage 2 measured, so tapping an RTTY tag
opens RTTY already set to 45.45/170. A client too old for `sigtags` ignores it.

**Other backends.** On OpenWebRX/Kiwi/UberSDR we have no server classifier. The app may draw **layer A only** (a
static band-plan table is not decoding), plus OWRX's own spot data where it sends it. Layer B is a VibeServer feature.

### 5.3 The "Anyone know what this is?" link

The tag kinds should be the same ids as the canned chat answers (`is_rtty`, `is_drm`, `is_stanag`…), so a tapped tag
and a chat answer open the same sigidwiki page and the same decoder. A detected tag could even pre-fill the "It's …"
row for the person answering.

### 5.4 Drawing without clutter

**Where.** Tags get their **own lane**: a 16–18 pt strip at the bottom edge of the spectrum, just above the waterfall.
- They are not on the top row (Stuart's OWRX experience).
- They are not inside the waterfall (which must never be obscured or frozen).
- They are not over the VTS or the search box.

The lane is part of the layout, so the spectrum's height accounts for it. It is hidden when there are no tags, and it can
be switched off (Display → Signal tags: Off / Detected only / All).

**Placement, with no overlap (one deterministic rule, shared by web and app as `src/utils/tagLayout.ts`, like
`galleryPager.ts`):**
1. Build the candidates: layer-B tags as pills; layer-A allocations as **underline segments** (a 2 px coloured bar the
   width of the sub-band along the lane's top edge, no text) unless a layer-B tag sits inside them.
2. Sort by priority:
   - the tag at the current dial;
   - confirmed, then detected, then likely;
   - a decoder this server offers ahead of an identify-only kind;
   - then signal strength.
3. Place each pill centred on its signal, clamped inside the lane and **outside the exclusion rectangles** the layout
   passes in (the VTS, the search box, the zoom drum, the passband handles). If it collides with an already-placed
   pill, try a shorter label ("RTTY 45/170" → "RTTY" → "R"), then shifting up to ±½ pill width, then **collapse**.
4. **Collapse:** pills that cannot fit merge into a counted cluster "◆ 3" at their centroid. Tapping the cluster opens a
   small list ("RTTY 45/170 · 14.0803", "PSK31 · 14.0705", "FT8 · 14.074 · 23 decodes"), each row with DECODE or WIKI.
   At low zoom (a whole HF band on screen), whole sub-bands collapse to one cluster per sub-band ("FT8 ◆ 12").
5. The labels use the standard font (memory `feedback_display_font_everywhere`: decoder pills are not display-font
   chrome). Colour comes from the theme tokens, checked on default, silver and black, with transparency on and off,
   phone and wide (memory `feedback_no_visible_polish_gaps`).
6. **Stability:** a placed pill keeps its slot while it lives. Placement is recomputed only when the view changes or a
   tag appears or goes, so labels do not dance as the waterfall scrolls.

**Tap behaviour.**
- A tap on a pill tunes to it, in the right mode, with the decoder opened and pre-set from `p`. Under the shared-dial
  contract this is a user action, so the client sends it like any tune; tuning etiquette applies on a shared dial.
- A long press (or the pad's secondary button) shows the info card: what it is, how sure we are, the sigidwiki link,
  "no decoder on this server" where that is true.
- An identify-only kind (PACTOR, encrypted DMR, STANAG) offers WIKI only.

**Pad and keyboard.** The tag lane joins the spectrum's focus group: ◀/▶ steps between pills and clusters in screen
order, enter = the tap, the secondary button = the info card. The keyboard uses `T` to cycle tags at the dial's side
(add it to `KeyboardShortcuts.tsx` and the controller brief).

**Watch and Jr:** no tags on the watch spectrum. At most, the tag at the dial appears as a word in the readout.

### 5.5 Phasing

1. **Phase 1 (tier 1):**
   - the layer-A table and the underline segments;
   - the approved FSK detector as the first layer-B kind;
   - "confirmed" tags from running decoders (FT8, NAVTEX, RTTY, WEFAX, SSTV already exist);
   - the lane, the layout rule, tap-to-decode.
2. **Phase 2 (tier 2):** the stage-1 rule set for the table in §5.2, and stage-2 OFDM/PSK/preamble tests.
3. **Phase 3:** background tags. Idle decoding's VFOs report into the same table, so a new listener lands and already
   sees "NAVTEX · 6 messages tonight" on 518.

**Tests:** a labelled corpus from `~/VibeSDR-test-audio` and the UberSDR live recordings (memory
`ubersdr_live_audio_test_source`); a precision/recall table per kind; **a false tag is worse than none**, so tune for
precision.

---

## 6. Decoder menu design

### 6.1 Today

- The app's Mode sheet (`ModeSelector.tsx`) has a CLIENT DECODERS grid (RTTY, NAVTEX, WEFAX, SSTV, TIME, filtered by
  what the server's extensions include and what the admin blocks), ADVANCED RDS, DAB, TRANSCRIBE, and the spots keys.
- On OWRX it shows the server's DIGITAL / DECODERS dropdown.
- The web client's DECODERS menu mirrors that.

With tier 1 alone the grid would grow from 5 keys to about 12, so it needs the OWRX-style split now.

### 6.2 The model: pins on the sheet, the rest in one list

```
DECODERS
[ FT8 ] [ RTTY ] [ NAVTEX ] [ WEFAX ] [ CW ] [ SSTV ]          ← pinned keys (max 8, two rows)
[ MORE DECODERS (14) ▾ ]                                       ← the rest, one scrolling list
    ☆ APRS            VHF packet · positions
    ☆ DSC             ship calls & distress
    ★ PSK31           (pinned; shown above)
    ☆ WSPR …
    GALLERY ›
```

- **Default pins (when the user has never pinned anything):** the decoders this server offers on this radio, in this
  order, up to 6: **FT8, RTTY, CW, NAVTEX, WEFAX, SSTV** on HF; **APRS, AIS, ADS-B, ACARS** on VHF/UHF where offered.
  Above 30 MHz the HF defaults that are pointless there drop out of the *default* set (not out of the list). As soon as
  the user pins anything, the user's pins replace the defaults entirely: no mixing, so nothing appears they did not
  choose.
- **The dropdown** lists everything else this server and radio can run, alphabetically, in groups (Amateur · Utility ·
  Aviation · Maritime · Broadcast · Satellite · Time), each row with a one-line description and a ☆/★ pin toggle. A
  running decoder that lives in the dropdown lights the dropdown's head, as `activeDecInOthers` does now.
- **GALLERY** stays at the bottom of the decoder section (VibeServer only), as now.
- **The order of pins:** the order pinned (see §6.5: pinning is done in EDIT mode). Max 8 pins; pinning a ninth asks to
  replace the last.
- The same model is used for OWRX's list, so a VibeSDR user's habits carry between backends: pins are by **decoder
  id**, mapped per backend (our `rtty` is OWRX's `rtty`/UberSDR's `fsk`; ids are mapped in one table, as
  `ModeSelector`'s `slug` mapping does today).

### 6.3 Persistence

- **Per user, across servers.** The pin list is the user's habit, not the server's, so it is stored once per user and
  **intersected** with what each server offers.
  - App: the existing settings store with `cloudSync.ts` (iCloud KVS / Android backup) so pins follow the user between
    their phone, iPad and Mac.
  - Web: `localStorage` keyed `vibe.decoderPins.v1`, every read and write in try/catch, the page correct without it.
- **Optional per-server override** (later, only if asked): a long-press "pin on this server only".

### 6.4 Hidden, never dead

Following AGENTS.md ("a control that only works in one scenario should not be there"):

- **The server decides what exists.** hwinfo lists the decoders available **on this radio, now**. It drops a decoder
  when any of these applies:
  - the admin disabled it;
  - the radio cannot reach its frequency;
  - the capture bandwidth is too small (ADS-B, DAB, TV);
  - the radio is an Icom (no IQ for the channelizer);
  - the box's benchmark puts it out of reach (LRPT on a Red-band host shows in the list with "slow on this server",
    per `DECODERS-GALLERY.md`, because it still works, only slowly);
  - it is ⚖ and not opted in (or the user is not the admin).
- **The client never shows a dead key.** A pinned decoder the current server does not offer is simply **not drawn**.
  One quiet line at the bottom of the dropdown says "2 of your pinned decoders aren't on this server", so the user knows
  why a pin vanished without being offered a control that does nothing. The pin is kept for the next server.
- **Band-awareness is advice, not hiding.** A decoder that works anywhere technically (RTTY at 147 MHz) stays in the
  list. Only the default pins are band-aware.
- A decoder that runs on this server but is **busy** (no free slot) stays drawn and says so when tapped ("all 4 decoder
  slots are in use; one will free when …"). That is a real, temporary state, not a dead control.

### 6.5 Pinning: a small EDIT button, and NO new controls (Stuart, 2026-10-23)

★★ Stuart's decision, which replaces the earlier proposal (secondary face button to pin, `D`/`P` shortcuts, drag/▲▼
reorder): **the keyboard and controller need nothing more than they have now.**

- **Selecting a decoder stays exactly as today:** highlight the DIGITAL / DECODERS dropdown, then select to open it;
  the D-pad (or arrow keys) scrolls; select runs the decoder; back closes the menu.
- **Pinning is a small EDIT button at the top of the decoder section.** EDIT expands the whole decoder list into
  ordinary buttons in the sheet. The user scrolls the sheet down and selects a button to pin or unpin it (★/☆ on the
  button, with a VoiceOver/TalkBack announcement such as "PSK31 pinned"). EDIT again, or back, returns the sheet to its
  normal layout.
- Because EDIT mode is just a list of buttons, it works by touch, mouse, keyboard and pad with the existing navigation
  (`NavRow`/`NavItem`). There is no new binding to learn, document or tour.
- **Pin order** is the order pinned. Reordering is left out unless someone asks for it.

- **Focus memory:** reopening the sheet puts focus on the running decoder, or the last one used.
- **Copy to update when this moves (AGENTS.md):**
  - the `sdrTour` card that names CLIENT DECODERS;
  - `AboutOverlay` release notes and feature lists;
  - the website;
  - the watch tutorial if it mentions decoders.

### 6.6 Tags and menu together

The tag lane is the discovery path: a listener sees "RTTY?" and taps it. The menu is the habit path. Both open the
same decoder with the same settings, through the same id table.

---

## 7. Sources

Standards (free unless marked $):
- ITU-R M.540 (NAVTEX): https://www.itu.int/rec/R-REC-M.540/en · M.476: https://www.itu.int/rec/R-REC-M.476/en ·
  M.625: https://www.itu.int/rec/R-REC-M.625/en
- ITU-R M.493 (DSC): https://www.itu.int/rec/R-REC-M.493/en · M.541: https://www.itu.int/rec/R-REC-M.541/en
- ITU-R M.1371 (AIS): https://www.itu.int/rec/R-REC-M.1371/en
- ITU-R M.584 (POCSAG): https://www.itu.int/rec/R-REC-M.584/en
- ITU-R M.1677 (Morse): https://www.itu.int/rec/R-REC-M.1677/en
- ITU-R BT.470 (analogue TV): https://www.itu.int/rec/R-REC-BT.470/en
- ETSI ES 201 980 (DRM): https://www.etsi.org/deliver/etsi_es/201900_201999/201980/
- ETSI EN 300 744 (DVB-T): https://www.etsi.org/deliver/etsi_en/300700_300799/300744/ · EN 302 755 (DVB-T2):
  https://www.etsi.org/deliver/etsi_en/302700_302799/302755/ · EN 300 421 (DVB-S):
  https://www.etsi.org/deliver/etsi_en/300400_300499/300421/ · EN 302 307 (DVB-S2):
  https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/
- ETSI EN 300 163 (NICAM 728): https://www.etsi.org/deliver/etsi_en/300100_300199/300163/
- ETSI TS 102 361 (DMR): https://www.etsi.org/deliver/etsi_ts/102300_102399/10236101/ · TS 102 490 (dPMR):
  https://www.etsi.org/deliver/etsi_ts/102400_102499/102490/
- ETSI EN 300 392 (TETRA air interface): https://www.etsi.org/deliver/etsi_en/300300_300399/30039202/ ·
  EN 300 395-2 (TETRA codec, Thomson-CSF FRAND note):
  https://www.etsi.org/deliver/etsi_i_ets/300300_300399/30039502/01_60/ets_30039502e01p.pdf
- MIL-STD-188-141 / -110 (ALE, HF modems), DoD ASSIST: https://quicksearch.dla.mil/
- Cospas-Sarsat T.001 (406 MHz beacons): https://www.cospas-sarsat.int/en/documents-pro/system-documents
- AX.25 v2.2: https://www.tapr.org/pdf/AX25.2.2.pdf · APRS 1.0.1: http://www.aprs.org/doc/APRS101.PDF
- "The FT4 and FT8 Communication Protocols" (QEX 2020): https://wsjt.sourceforge.io/FT4_FT8_QEX.pdf · WSJT-X
  documentation (WSPR, FST4, Q65, MSK144): https://wsjt.sourceforge.io/wsjtx.html
- CCSDS 131.0-B (TM coding, used by LRPT): https://public.ccsds.org/Pubs/131x0b4.pdf
- 47 CFR Part 11 (EAS/SAME): https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-11
- IARU Region 1 band plans: https://www.iaru-r1.org/on-the-air/band-plans/
- NIST WWV/WWVB: https://www.nist.gov/pml/time-and-frequency-division/time-distribution · JJY (NICT):
  https://jjy.nict.go.jp/jjy/trans/index-e.html · CHU (NRC):
  https://nrc.canada.ca/en/certifications-evaluations-standards/canadas-official-time/nrc-shortwave-station-broadcasts-chu
- ICAO Annex 10, Doc 9741 (HFDL), Doc 9776 (VDL2) $: https://store.icao.int/ · RTCA DO-260B, DO-282B $:
  https://www.rtca.org/ · ARINC 618/631/635/714 $: https://www.sae-itc.com/
- M17 specification: https://spec.m17project.org/ · Codec2: https://github.com/drowe67/codec2 · FreeDV:
  https://freedv.org/

Open-source software (licences as stated by each project; check before relying):
- OpenWebRX+ (AGPL-3.0): https://github.com/luarvique/openwebrx, https://fms.komkon.org/OWRX/
- SDR++ (GPL-3.0): https://github.com/AlexandreRouma/SDRPlusPlus · SDRangel (GPL-3.0):
  https://github.com/f4exb/sdrangel · SigDigger (GPL-3.0): https://github.com/BatchDrake/SigDigger
- multimon-ng (GPL-2.0): https://github.com/EliasOenal/multimon-ng · dsd-fme: https://github.com/lwvmobile/dsd-fme ·
  OP25 (GPL-3.0): https://github.com/boatbod/op25 · mbelib (ISC): https://github.com/szechyjs/mbelib
- rtl_433 (GPL-2.0): https://github.com/merbanan/rtl_433 · readsb (GPL-3.0): https://github.com/wiedehopf/readsb ·
  dump1090-fa (GPL-2.0+): https://github.com/flightaware/dump1090 · dump978-fa: https://github.com/flightaware/dump978
- acarsdec (GPL-2.0): https://github.com/TLeconte/acarsdec · dumpvdl2 (GPL-3.0): https://github.com/szpajder/dumpvdl2 ·
  dumphfdl (GPL-3.0): https://github.com/szpajder/dumphfdl
- direwolf (GPL-2.0): https://github.com/wb2osz/direwolf · AIS-catcher (GPL-3.0): https://github.com/jvde-github/AIS-catcher
- SatDump (GPL-3.0): https://github.com/SatDump/SatDump · radiosonde_auto_rx (GPL-3.0):
  https://github.com/projecthorus/radiosonde_auto_rx · rs1729/RS: https://github.com/rs1729/RS
- WSJT-X (GPL-3.0): https://sourceforge.net/projects/wsjt/ · ft8_lib (MIT, in use): https://github.com/kgoba/ft8_lib ·
  JS8Call (GPL-3.0): https://js8call.com/
- fldigi (GPL-3.0): https://sourceforge.net/projects/fldigi/ · slowrx (ISC): https://github.com/windytan/slowrx ·
  QSSTV (GPL-3.0): https://github.com/ON4QZ/QSSTV
- Dream (GPL-2.0): https://sourceforge.net/projects/drm/ · welle.io (GPL-2.0): https://github.com/AlbrechtL/welle.io
- KiwiSDR and extensions (GPL-3.0): https://github.com/jks-prv/Beagle_SDR_GPS · UberSDR (MadPsy): https://github.com/madpsy
- leansdr / leandvb (GPL-3.0): https://github.com/pabr/leansdr · BATC DATV wiki (Portsdown, MiniTiouner):
  https://wiki.batc.org.uk/ · gr-dvbt: https://github.com/BogdanDIA/gr-dvbt
- osmo-tetra / TETRA codec notes: https://osmocom.org/projects/tetra/wiki/Speech_Codec
- gr-lora_sdr (GPL-3.0): https://github.com/tapparelj/gr-lora_sdr · gr-iridium: https://github.com/muccc/gr-iridium
- Signal Identification Wiki: https://www.sigidwiki.com/ · Artemis (GPL-3.0): https://github.com/AresValley/Artemis ·
  TorchSig (Apache-2.0): https://github.com/TorchDSP/torchsig

Licensing, patents, law:
- IMBE patent status (secondary sources): https://en.wikipedia.org/wiki/Multi-Band_Excitation
- AMBE+2 US 8,359,197 to 2028-05-20 (as stated by a third-party package; unverified): https://pypi.org/project/blip25-mbe/0.2.1/
- Via LA (AVC/H.264, xHE-AAC pools): https://www.via-la.com/ · MPEG-4 Visual's last pool patent expired July 2026:
  https://itsfoss.com/news/mpeg-4-visual-patent-expiry/ · H.264 patent count, May 2026:
  https://ipkitten.blogspot.com/2026/07/guestpost-sep-licensing-when-patent.html · MPEG-2 pool:
  https://en.wikipedia.org/wiki/MPEG-2
- NICAM background: https://en.wikipedia.org/wiki/NICAM
- Analogue TV switch-off status: https://en.wikipedia.org/wiki/Digital_television_transition ·
  https://en.wikipedia.org/wiki/Terrestrial_television · Cuba 2026:
  https://www.cubanet.org/gobierno-cubano-pretende-apagar-la-television-analogica-en-todo-el-pais-en-2026/
- UK Wireless Telegraphy Act 2006 s.48: https://www.legislation.gov.uk/ukpga/2006/36/section/48
- UK CDPA 1988 s.20: https://www.legislation.gov.uk/ukpga/1988/48/section/20 · TV Licensing:
  https://www.tvlicensing.co.uk/check-if-you-need-one
- US 18 U.S.C. §2511 / §2510: https://www.law.cornell.edu/uscode/text/18/2511 ·
  https://www.law.cornell.edu/uscode/text/18/2510
- Raspberry Pi 5 (no H.264 hardware codec; HEVC decode): https://www.raspberrypi.com/products/raspberry-pi-5/

Internal: `docs/v12/BACKGROUND-DECODING.md`, `docs/v12/DECODERS-GALLERY.md`, `docs/DECODER-AUDIT-2026-10-04.md`,
`docs/WIRE-AUDIT-2026-10-10.md`, `android/app/src/main/cpp/vibe_decoder_host.h`, `vibe_benchmark_decoders.h`,
`src/components/ModeSelector.tsx`, `src/services/DecoderClient.ts`, `src/services/dataModes.ts`; memory notes
`decoders_where_they_run`, `v12_signal_suggest_decoder`, `sam_ecss_cquam_plan`, `hd_radio_declined`,
`dab_drm_licence_free_plan`, `chat_answers_sigid_wiki`.

### Unsure, needing a check before anyone relies on it

- IMBE patents expired: secondary sources only.
- AMBE+2 expiry 2028: one third-party claim.
- Current TETRA ACELP licence holder and terms; the licence of ETSI's reference C code.
- FLEX patents (probably expired).
- LoRa: whether a software receiver needs a Semtech licence.
- NICAM: no patent evidence found either way.
- STANAG 4285's public-release status.
- dump978-fa and rs1729 licences.
- Android's xHE-AAC availability per device.
- Linux FFmpeg USAC as an xHE-AAC route.
- The UK position on ACARS message bodies.
- BPC's official format document.
- Every CPU figure in this brief: estimated, not measured.
