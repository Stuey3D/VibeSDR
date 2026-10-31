# Decoder survey: every decoder we do not have yet (V12)

**Status:** research only, 2026-10-31. Nothing here is built, and no code was read except where a licence is GPL-3
compatible and only to confirm how a mode behaves. Every finding is in our own words.
**Asked by Stuart:** "Research all the other decoders we do not have yet. Same principles apply: compatible with our
licence, inspired by, never copy, improve where possible and make as efficient as possible with SIMD NEON SSE2 etc."
**Builds on:** `DECODER-RESEARCH.md` (2026-10-23: the first sweep, the legal framework in its §4, signal tags, the
decoder menu). That document is still the reference for the interception law (§4.3) and the digital-voice split
(§4.2). This one goes deeper on what is left, corrects a few of its licence calls, and adds modes it did not list.
**Satellites:** the tracking engine, Doppler lock, `sats.json`, pass scheduler, gallery and client-assisted decode are
already designed in `SATELLITE-ENGINE.md`, `DECODERS-GALLERY.md` and `RADIO-ROLES.md`. §6 below covers only the
decoders that ride on them, and does not re-propose tracking or Doppler.
**Out of scope here:** ACARS, VDL2, HFDL and radiosondes (another agent's survey). HD Radio and TV from servers
(declined).

**Not legal advice.** Licences were checked on 2026-10-31 against the repositories themselves (LICENSE files and source
headers); patent and interception points are the best reading of public sources, marked ⚖ where a proper check is
needed before shipping.

---

## 0. How to read this

**Cost class on a Raspberry Pi 2** (900 MHz quad Cortex-A7, NEON, 1 GB), as a share of ONE core, the same scale as
`DECODER-RESEARCH.md`: *trivial* < 2 %, *light* 2–10 %, *medium* 10–30 %, *heavy* 30–100 %, *beyond* > 1 core.
Anchored on our measured Pi 2 figures (`PI2-REAL-CPU.md`): SELCAL 2.3 %, MSK144 2.5 %, FT8/Q65 dial ~11 %,
CW skimmer 29 %, ISM ~47 % per thread, AIS 53 %. These are estimates, not measurements; each decoder still needs its
`vibe_benchmark_decoders.h` row before it ships.

**Effort:** S = days, M = one to two weeks, L = weeks, XL = a month or more.

**Licence compatibility with our GPL-3.** ✓ = GPL-3, GPL-2-or-later, LGPL, AGPL-3 (combinable under GPL-3 §13),
MIT, BSD, ISC, Apache-2.0. ✗ = GPL-2-only, proprietary, no licence. **Reading rule (unchanged):** compatible sources
may be read for inspiration and run as black-box test oracles; nothing is copied, because the App Store exception
cannot be granted over third-party code.

### 0.1 Licence corrections to the first sweep

The 2026-10-23 sweep marked several references "GPL-2" and treated them as off-limits. The file headers say otherwise:

| Project | What the headers actually say (checked 2026-10-31) | Compatible? |
|---|---|---|
| **direwolf** (wb2osz) | "either version 2 of the License, or (at your option) any later version" | ✓ GPL-2+ (GitHub's "GPL-2.0" tag is misleading) |
| **multimon-ng** | GPL-2 "or (at your option) any later version" | ✓ GPL-2+ |
| **rtl_433** | GPL-2 "or (at your option) any later version" | ✓ GPL-2+ (we still build ISM clean-room, by choice) |
| **libm17** (M17 Project) | LICENSE is the GPL-2 text; no "or later" seen in the LICENSE | ⚠ treat as GPL-2-only until a header says otherwise; m17-cxx-demod is GPL-3 ✓ |
| **dsd-fme** | mixed: most code ISC ("DSD Author"); `dstar_header`, `descramble.h`, `fcs.h` GPL-2-only; README says it borrows snippets from OP25 and sdrtrunk | ⚠ do not read: provenance is mixed and partly GPL-2-only |
| **mbelib** | ISC | licence ✓, **but see §4: the patents are the problem, not the copyright** |
| **ardop** (pflarue's ardopcf) | MIT | ✓ |
| **JAERO** | MIT | ✓ |
| **dump978** (FlightAware) | BSD-2-Clause | ✓ |
| **codec2** | LGPL-2.1 | ✓ (dynamic link on the server, as already planned) |
| **radae** (FreeDV RADE) | BSD-2-Clause | ✓ |
| **LPCNet** | BSD-3-Clause | ✓ |
| **horusdemodlib** | LGPL-2.1 | ✓ |
| **SatDump, gr-satellites, SDRangel, SDR++, dumpvdl2, sdrtrunk, pyModeS, DSDcc, digiham, codecserver, radiosonde_auto_rx, DJ2LS FreeDATA, ka9q-radio** | GPL-3 | ✓ |
| **osmo-tetra, rtlamr, OpenWebRX+** | AGPL-3 | ✓ for reading |
| **meteor_demod / meteor_decode** (dbdexter), **MeteorDemod** (Digitelektro) | MIT | ✓ |
| **KiwiSDR** (Beagle_SDR_GPS, now archived) | "a mix of GPL v2.1/3 and MIT", per-directory `_LICENSE` files | ⚠ per file; check each extension before reading |
| **OP25** (boatbod), **gr-iridium**, **iridium-toolkit**, **SSDV** | no licence metadata found on GitHub; GPL-3 is believed for OP25/gr-iridium/SSDV from their headers in the past | ⚠ check headers before reading |

### 0.2 Shared machinery that most of this survey reuses

Most candidates are cheap *because* of pieces we already have or must build once. Naming them here avoids repeating
them in every entry:

- **Channeliser** (the `Channelizer` the CW skimmer and AIS use): a polyphase filter bank that cuts N narrow channels
  from one capture for the cost of one FFT per block. Every "many channels" decoder below (ALE scan list, DGPS skimmer,
  beacon tracker, DMR/NXDN on several repeaters, 406 MHz EPIRB band) is a channeliser consumer, not N separate DDCs.
- **FSK/MFSK cores** (RTTY, NAVTEX, DSC, THOR, Olivia, MFSK16/32): tone filterbank or sliding DFT, Gray mapping,
  soft-decision Viterbi (K=7 r=½ is in THOR/MFSK), interleavers. DominoEX, FSQ, ALE and MT63 reuse these.
- **PSK core** (PSK31/63): matched filter, Costas/decision-directed carrier loop, Gardner timing. PSK125–1000, PSK-R,
  8PSK and FUNcube BPSK reuse it.
- **HDLC/AX.25 stack** (APRS, AIS): NRZI, bit de-stuffing, FCS-16. 9600 G3RUH, FX.25, IL2P and cubesat AX.25 reuse it.
- **4-FSK symbol slicer with soft output** (from FT8/JS8/LoRa work): DMR, dPMR, NXDN, P25, YSF and M17 are all 4-FSK
  (C4FM) at 2400–4800 sym/s, so one SIMD discriminator + RRC filter + timing loop serves the lot.
- **CCSDS chain** (to be built for Meteor LRPT): K=7 r=½ Viterbi, ASM sync, derandomiser, RS(255,223). GOES/GK-2A
  xRIT, many cubesats and Inmarsat-C all reuse parts of it.
- **SIMD policy** (`DECODERS-WAVE2.md` "SIMD — hard requirement"): every hot loop NEON + SSE2 with a scalar fallback,
  bit-exact tests between them. The usual hot loops here are FIR/RRC filters, complex mixers, the Viterbi
  add-compare-select (16- or 64-state butterflies vectorise very well: 8 or 16 lanes of int16), FFT bins and
  correlators.

---

## 1. Utility HF

### 1.1 ALE 2G (MIL-STD-188-141 Appendix A) — **build later (first of the utility modes)**
1. **Standard:** MIL-STD-188-141 (B/C/D), free from the DoD ASSIST quick-search site (quicksearch.dla.mil); FED-STD-1045
   is the civil twin. Amateur practice (HFN, HFLink) documented at hflink.com.
2. **Open implementations:** LinuxALE (old, GPL; version not checked: don't read until checked); PC-ALE and MARS-ALE are
   freeware (closed); KiwiSDR's ALE_2G extension (per-file licence, check first). No clean GPL-3 reference known, so
   write from the standard alone.
3. **How it works:** 8-FSK, 125 Bd, tones 750–2500 Hz in 250 Hz steps (3 bits per tone, 375 bit/s). 24-bit words
   (3-bit preamble + three 7-bit ASCII-64 characters) coded with extended Golay (24,12), each word sent three times
   interleaved, with majority vote. Word types TO, TIS, TWAS, THIS, DATA, REP, CMD; calls, soundings, AMD text. Weak
   spots of existing decoders: hard-decision Golay, no combining of the triple redundancy below the bit level, and one
   channel at a time (a scanning network hops over 10–20 channels).
4. **How we'd do better:** soft-decision tone energies → soft Golay decode (Chase or full ML over 4096 code words is
   cheap at 125 Bd) and soft combining of the three copies; the channeliser to watch a whole ALE net's channel list at
   once from a wide HF capture (RX888/UberSDR, or a VibeServer HF capture) rather than following one. NEON: the 8-tone
   detector is one small sliding DFT per channel. **Pi 2: light per channel; medium for a 20-channel net.**
5. **Legal/privacy ⚖:** ALE calls are addressing: "sender or addressee" under WTA 2006 s.48 for non-amateur nets.
   Amateur ALE (HFN, in the amateur bands) default on. Non-amateur: show "ALE 2G call / sounding" by default;
   station addresses behind the admin opt-in (same tier as pager content in `DECODER-RESEARCH.md` §4.3), never in a
   public gallery. Note that utility listeners publish ALE logs widely (UDXF); we follow the stricter line anyway.
6. **Activity:** very active in 2026: government, military, NGO and amateur nets heard every hour in Europe.
7. **Effort M.** Test signals: RX888 on UberSDR (2–30 MHz), sigidwiki samples, HFN soundings on 14.109/7.102 MHz.

### 1.2 ALE 3G (STANAG 4538 / 188-141 App. C) and 4G (Wideband ALE, 188-141D App. G) — **skip**
Standard partly free (141 App. C; 4538 is NATO, release status unclear). Burst PSK, very short, encrypted linking
traffic in most real use. Little to show beyond "3G ALE burst". Identify only via the signal classifier (§1.4).

### 1.3 STANAG 4285 / 4529 / 4539 and MIL-STD-188-110 A/B/C serial — **identify only (later)**
1. **Standard:** MIL-STD-188-110 A/B/C free on ASSIST ✓. STANAG 4285/4529 public copies circulate; their NATO release
   status is unclear, so prefer 110A, which shares the structure.
2. **Open implementations:** few and not clean; SigDigger (GPL-3) is an analysis tool. Commercial: Sorcerer, Code300,
   Wavecom (closed). Write from 110A.
3. **How it works:** 8-PSK at 2400 Bd on an 1800 Hz carrier, scrambled; known probe blocks for channel estimation;
   decision-feedback equaliser; interleaver sizes signalled in the preamble (110A: a 3-symbol mode descriptor repeated
   in the sync preamble tells data rate and interleave). Real traffic is mostly encrypted crypto-streams.
4. **Better:** do the cheap, useful half: correlate the preamble/probe structure to say *which* waveform it is, the data
   rate and interleave ("STANAG 4285, 600 bps long"), and stop there. A preamble correlator on the PSK core is
   **light**; a full equalised demod would be **medium** and buys almost nothing (content encrypted).
5. **Legal ⚖:** military traffic; format, not content, by policy.
6. **Activity:** heavy on HF in Europe (naval broadcasts). People ask "what is this?" constantly: identification has real
   value.
7. **Effort S/M** (identify), L (full). Test: RX888 on UberSDR; sigidwiki recordings.

### 1.4 A generic utility signal classifier (baud, shift, PSK order) — **build later, with signal tags phase 2**
Not a decoder, but the single biggest win for utility listeners: estimate symbol rate (cyclostationary/squared-envelope
line), FSK shift and tone count, PSK order (M-th power spectrum lines), and match against a table (sigidwiki-style).
That labels ARQ-E, CIS-36-50, PACTOR, VARA, STANAG, 3G ALE, Link 11, CODAR sweeps and so on, without decoding any of
them. Already scoped as "signal tags phase 2" in `DECODER-RESEARCH.md` §5; this survey just confirms it is where most
of the "identify only" rows below should land. **Pi 2: light (a few FFTs per second per signal).**

### 1.5 CODAR / OTH radar — **tags only**
HF ocean radars (SeaSonde, 4.4–5.4, 12–13.5, 24–27 MHz FMCW sweeps); some send a CW ID. Identify from sweep rate and
span via §1.4; any CW ID goes through our CW decoder. No spec needed. S, trivial.

### 1.6 ARQ-E/E3, ARQ-M2/M4 (TDM), Piccolo, Coquelet, Twinplex, FEC-A, CIS-36-50 — **skip (identify only)**
Legacy diplomatic, military and press-agency modes. Mostly gone from air by 2026; what survives is encrypted or
weather. Specs are only in ITU-R/CCIR old recommendations (ARQ-E: ITU-R F.342, F.519 free) and Klingenfuss handbooks
(paid). Identify only via §1.4.

### 1.7 DGPS (RTCM SC-104 on MF) — **later, as an NDB-skimmer add-on; mostly dead in north-west Europe**
1. **Standard:** RTCM SC-104 v2.x is **paid** (rtcm.org). ITU-R M.823 (free ✓) describes the radiobeacon broadcast:
   MSK at 100/200 bps on 283.5–325 kHz, 30-bit words with GPS-style parity, message types 1/3/7/9/16. M.823 plus the
   free GPS ICD-200 parity algorithm is enough to frame, check parity and read station ID, health and the type 16 text.
2. **Open:** gpsd's RTCM2 decoder (BSD-2 ✓) as a black-box oracle.
3. **How it works:** MSK demod, word sync from the preamble 0x66, parity per word, then header (type, station ID,
   Z-count, health).
4. **Better:** our NDB skimmer already sees the DGPS "humps" at 284–291 kHz (`NDB-SKIMMER.md`) and deliberately ignores
   them; the same channeliser can demodulate every hump in the band at once and name the station from its ID. MSK
   200 bps is **trivial** per channel.
5. **Legal:** navigation broadcast for general reception. Fine.
6. **Activity:** UK and Ireland GLAs **closed DGPS in March 2022** (Trinity House notice 2/2022); **Norway planned
   closure from 27 January 2026** (Kystverket); Hong Kong ends 1 Jan 2027. Some continental, Baltic (R-Mode trials),
   Russian, Mediterranean and Asian stations remain: the IALA DGNSS station table (October 2024) is the list to check.
   From the UK, night-time reception of continental beacons is still possible.
7. **Effort S** once the skimmer channel path is reused. Test: RX888 at night on 283–325 kHz.

### 1.8 NAVDAT (ITU-R M.2010) — **skip for now, watch**
500 kHz (and HF) OFDM successor to NAVTEX; ITU-R M.2010-1 is free ✓; no open implementation known. Only trials (France,
China, Korea, a few others) by 2026. Revisit if an operational station appears in Europe. M/L, light.

### 1.9 SITOR-B variants, SYNOP/weather RTTY variants — **have**
NAVTEX/SITOR-B, SITOR-A and DWD-style weather RTTY with SYNOP translation are built. One small check, not a new
decoder: make sure the SITOR-B decoder runs at any dial (HF MSI broadcasts, Russian/Greek coast stations), not only on
the NAVTEX channels.

### 1.10 PACTOR I (and II/III/IV), VARA, WINMOR, ARDOP, CLOVER — **identify; ARDOP maybe later**
- **PACTOR I:** FSK 100/200 Bd, memory-ARQ, CRC-16; the protocol was published by SCS (free description). No GPL-3
  receive-only implementation known. Almost nobody uses PACTOR I in 2026 (Winlink stations are PACTOR III/IV or VARA).
  **Skip** beyond identification.
- **PACTOR II/III/IV:** proprietary (SCS), compressed. Identify only.
- **VARA (HF/FM/SAT):** closed, proprietary (EA5HVK). Identify only; never attempt a decode.
- **WINMOR:** dead (Winlink retired it). Skip.
- **ARDOP:** spec published (ardop.org); **ardopcf is MIT** ✓. 4FSK/PSK/QAM frames with RS. Receiving one side of
  someone else's ARQ session shows little; ARDOP FEC mode is broadcast-style. Low activity. **Later, low value.**
- **CLOVER:** dead.

### 1.11 HF time/frequency extras — see §8 (DCF77 and WWVB phase codes).

---

## 2. Amateur

### 2.1 AX.25 9600 Bd G3RUH, plus FX.25 and IL2P — **build now**
1. **Standards:** G3RUH's 9600 modem paper (free ✓: scrambler 1 + x¹² + x¹⁷, NRZI, baseband FSK); AX.25 2.2 (TAPR ✓);
   **FX.25** (Stensat Group spec, free ✓: Reed–Solomon wrapper round an unchanged AX.25 frame, correlation tags select
   the RS code); **IL2P** (Nino Carrillo KK4HEJ, free spec ✓: a compact header, RS parity, no bit-stuffing, used by
   newer TNCs and direwolf).
2. **Open:** direwolf (GPL-2+ ✓, readable for inspiration), multimon-ng FSK9600 (GPL-2+ ✓), gr-satellites (GPL-3 ✓).
3. **How it works:** 9600: matched filter, clock recovery (Gardner or zero-crossing PLL), descramble, NRZI, HDLC.
   Weak spots: 9600 is unforgiving of a radio's audio filtering and of mistuning; most software decoders use a single
   hard slicer; Pi users report direwolf at 9600 being fine but sensitive to the sample rate/decimation chain.
4. **Better:** we take 9600 from **IQ**, not from a radio's discriminator audio, so there is no de-emphasis or audio
   filter in the way; run two or three slicer thresholds/timing phases in parallel and keep any frame that passes the
   FCS (the same "several demodulators" trick as our APRS); FX.25/IL2P give real RS error correction; soft bits into an
   erasure-aware RS decoder. NEON: FIR + discriminator over 4 lanes. **Pi 2: light** (~5 %).
5. **Legal:** amateur. Fine.
6. **Activity:** terrestrial 9600 packet is small in the UK; the real value is **cubesats** (§6.3) and IL2P/FX.25 on
   1200 APRS (direwolf users increasingly turn FX.25 on: decoding it costs nothing and rescues frames).
7. **Effort M** (9600 + FX.25 + IL2P together). Test: SatNOGS public observation recordings (9k6 GMSK cubesats),
   our RSP/RTL on 145.825/435–438 MHz during passes.

### 2.2 FreeDV (700D, 700E, 1600, 2020, RADE) — **build later (700D/700E/1600 first)**
1. **Standards:** all open: the codec2 repo documents every mode (README_freedv, the OFDM modem description);
   RADE papers and code are open.
2. **Open:** codec2 (**LGPL-2.1** ✓), freedv-gui (licence metadata unclear, mostly LGPL; don't need it), radae
   (**BSD-2** ✓), LPCNet (**BSD-3** ✓).
3. **How it works:** 700D/700E: OFDM (17/21 carriers), pilots, LDPC, Codec2 700C voice; 1600: FDMDV 16-carrier DQPSK +
   Codec2 1300; 2020: OFDM + LPCNet neural vocoder; RADE: an ML "radio autoencoder" sending vocoder features over
   OFDM. Weak spots: 2020 and RADE are CPU-heavy (neural networks); 700D sync can take a few seconds.
4. **Better:** link codec2 **dynamically on the server only** (already decided in `DECODER-RESEARCH.md` §3A); decode to
   PCM, send Opus as for every mode; offer a mode only where the benchmark says it runs. We'd add an **AUTO** that
   detects 700D/700E/1600/RADE from the spectrum shape (carrier counts and spacing differ), which freedv-gui does not
   do well. **Pi 2: 1600 light, 700D/E medium (~15–25 %), 2020 beyond, RADE beyond** (benchmark-gated: desktop-class
   servers only).
5. **Legal:** amateur voice. Fine. Codec2 is designed to be free of patents.
6. **Activity:** modest but real; RADE (2024–) has revived interest; 14.236, 7.177, 3.643 MHz in Europe. The FreeDV
   Reporter website shows who is on.
7. **Effort M** (700D/E/1600), L (RADE; and only for strong servers). Test: freedv test files in codec2's repo; RX888.

### 2.3 M17 — **build later (soon): the one digital-voice system we can fully offer**
1. **Standard:** the M17 specification (spec.m17project.org), free ✓.
2. **Open:** m17-cxx-demod (**GPL-3** ✓), SDR++'s m17 decoder (GPL-3 ✓); libm17 is GPL-2 (treat as GPL-2-only: ✗ do
   not read).
3. **How it works:** 4-FSK at 4800 sym/s (9600 bit/s), RRC α = 0.5; sync words; LSF (link setup frame: source and
   destination callsigns encoded base-40, type, META) protected by K=5 convolutional code punctured, Golay(24,12) on
   the LICH chunks; Codec2 3200 voice. Packet mode and SMS, GNSS in META.
4. **Better:** share the 4-FSK SIMD slicer with DMR/NXDN/YSF (§3); soft Viterbi; combine LICH fragments across
   superframes to recover a callsign from a weak stream. **Pi 2: light** (~8–12 % with Codec2 3200 decode).
5. **Legal:** amateur, unencrypted by licence condition. Callsigns are fine to show.
6. **Activity:** small but growing; several UK M17 repeaters and many hotspots; reflectors via MMDVM.
7. **Effort M.** Test: M17 project sample files; any M17-capable hotspot on 70 cm; our RTL/RSP.

### 2.4 fldigi-family completions — **build now (cheap, rides on existing cores)**
- **DominoEX** (4/5/8/11/16/22/44/88): IFK+ (incremental frequency keying), 18 tones, varicode, no FEC. Spec published
  by ZL1BPU ✓. Our **THOR is DominoEX plus FEC**, so DominoEX is THOR with the FEC removed: **S, trivial**.
- **FSQ (Fast Simple QSO):** IFK+ with 33 tones, 2–6 Bd, directed messaging, designed for NVIS (UK 60 m and 80 m
  use it). Published by ZL1BPU/ZL2AFP ✓. Shares the IFK core: **S, trivial**. Display FSQ "heard" lists and
  directed messages (amateur; fine).
- **PSK125/250/500/1000, PSK-R (robust, with K=7 FEC + interleaver), multi-carrier PSK (12×PSK125R etc.):** extend
  the PSK core; NBEMS emcomm nets use PSK125R/PSK250R. **S, light.**
- **8PSK125/250/500/1000 (+ FL variants):** fldigi's 8PSK with Viterbi; NBEMS. **S/M, light.**
- **MT63 (500/1000/2000, short/long interleave):** 64 carriers, differential BPSK, Walsh-function FEC, a long
  interleaver (1 s or 2 s). Published by SP9VRC (Pawel Jalocha) ✓; fldigi GPL-3 ✓ as oracle. Very tolerant of
  QSB and interference. Used for emcomm on VHF FM (MT63-2000L), historically by VOA Radiogram. **M, light** (a
  64-bin FFT per symbol plus FHT).
- **Hell variants (Slow Hell, Hell x5/x9, FSK-Hell, Hell 80):** extend our Feld Hell renderer. **S, trivial.**
- **Throb/ThrobX:** effectively dead. **Skip.**
- **IFKP, SCAMP:** tiny user bases. Skip.

Legal: amateur, fine. Activity: modest; NBEMS/RAYNET-style emcomm use is the real audience (no 2025–26 UK usage figures
found). Test: fldigi's own test file generator is GPL-3 and can be run as a black box to make audio; RX888 for real.

### 2.5 JT65, JT9, JT4 — **later**
QEX/WSJT-X documentation ✓; WSJT-X GPL-3 ✓ oracle. JT65's strength is the Franke-Taylor soft-decision RS decoder
(published in QEX ✓) and its weakness is CPU in bursts. JT9 shares FT8-like LDPC-free convolutional coding (K=32
Fano). Activity in 2026: 160 m, LF/MF, and EME (JT65B/C, JT4 on microwaves); falling. Fits **Band spots**' mode registry.
**M each, Pi 2: medium burst per minute.** Low audience: after everything in "build now".

### 2.6 QRSS / DFCW "grabber" — **build now (display tool)**
Slow CW (3–120 s dots) and dual-frequency CW beacons, 10.140 MHz, 7.0398, 136/475 kHz and others. No decoding
standard needed: a long-integration narrow waterfall (0.1–0.5 Hz bins) rendered as an image strip, uploaded to the
gallery every 10 minutes like the "QRSS Plus" grabbers. Optional: WSPR-like auto-annotation is not needed. **S, trivial.**
Legal fine; a lively UK/European community (Knights QRSS). Test: RX888 on 10.1398–10.1402 MHz.

### 2.7 NCDXF/IARU beacon tracker — **build now**
18 beacons on 14.100, 18.110, 21.150, 24.930, 28.200 MHz, each 10 s per band in a fixed 3-minute cycle: callsign in CW
then four dashes at 100 W, 10 W, 1 W, 0.1 W. Schedule published by NCDXF ✓. We already have CW: the tracker only needs
the UTC slot table, the CW ID check, and SNR of each dash, then shows a "propagation now" grid (beacon × band × power
level heard). Channeliser across the five frequencies on a wide HF capture. **S, Pi 2 light.** Activity: on air in 2026,
though a few beacons are off at any time (check ncdxf.org/beacon status before a release note claims all 18). Very high
value for HF listeners; a natural background-decoding profile. Test: RX888 on UberSDR.

### 2.8 SSTV extras and digital SSTV — **later**
- Analogue SSTV gaps (MMSSTV MP/MR/ML, AVT, FAX480, PD without VIS) are already listed as an extension in
  `DECODER-RESEARCH.md`.
- **HamDRM / EasyPal digital SSTV:** DRM mode-B-like OFDM in 2.5 kHz, carrying files (JPEG/JP2) with RS and a
  segmented file transfer. EasyPal is closed; QSSTV (GPL-2+, check) implements HamDRM. We have a DRM OFDM layer
  (`DRM.md`), so HamDRM is mostly the narrow mode table and the file layer. Activity: 14.233 MHz, small but steady.
  **M, light.** Into the gallery like SSTV.
- KG-STV: closed, Japan only. Skip.

### 2.9 Packet extras on the AX.25 stack — **S, with 2.1**
NET/ROM node broadcasts, BBS beacons, AX.25 "heard" lists; plain parsers. **Trivial.**

### 2.10 FreeDATA and other codec2 data modes — **later, low**
DJ2LS FreeDATA (GPL-3 ✓) uses codec2's OFDM data modes for ARQ; only beacons and broadcasts are worth showing to a
third party. Tiny user base. Later.

### 2.11 UKHAS balloon telemetry, Horus Binary and SSDV images — **build now (small, UK-flavoured, legal, gallery-friendly)**
1. **Standards:** UKHAS telemetry sentence format (`$$CALL,…*CRC`) and RTTY practice (ukhas.org.uk ✓); **Horus Binary
   v2** (Project Horus docs ✓: 4-FSK 100 Bd with Golay/LDPC-style coding); **SSDV** (packetised JPEG with RS, used by
   balloons and some satellites; format description ✓).
2. **Open:** horusdemodlib (**LGPL-2.1** ✓); SSDV (fsphil, GPL-3 believed; repository archived, check headers);
   radiosonde_auto_rx (GPL-3 ✓) for the SondeHub side.
3. **How it works:** UKHAS: 50/300 Bd RTTY (we have RTTY) → parse and CRC-16 the sentence. Horus: 4-FSK 100 Bd with
   a soft decoder. SSDV: reassemble JPEG packets, RS(255,223) per packet, draw the image as it arrives.
4. **Better:** our RTTY AUTO already finds the shift; add the sentence parser, a Horus 4-FSK mode on the shared
   4-FSK slicer, and SSDV into the gallery as live-growing pictures. **Pi 2: trivial–light.**
5. **Legal:** licence-exempt/amateur telemetry intended for public tracking (SondeHub amateur map). Fine. Position of a
   balloon is not a person's.
6. **Activity:** UK school and club launches through the year on 434 MHz; worldwide Horus flights. Bursty but loved.
7. **Effort S/M.** Test: horusdemodlib sample files; UKHAS flight recordings; our RTL on 434.
   (Coordinate with the radiosonde agent: SondeHub upload and the map are shared.)

### 2.12 DTMF — **build now (tiny)**
ITU-T Q.23/Q.24 ✓ tone pairs. Goertzel bank, twist and duration checks; the same structure as SELCAL (2.3 % on Pi 2).
Use: repeater/EchoLink/AllStar control codes on amateur NFM. **S, trivial.** ⚖ Show it on amateur frequencies only
(band plan, as in `DECODER-RESEARCH.md` §4.2); on PMR/telephone-patch frequencies DTMF can be a dialled number:
format only ("DTMF heard").

---

## 3. Digital voice: signalling, data and the codecs

### 3.1 Shared receiver design
DMR, dPMR, NXDN, P25 Phase 1, YSF and M17 are all C4FM/4-FSK (2400–4800 sym/s) in 6.25 or 12.5 kHz; D-STAR is
GMSK 4800 bit/s; TETRA is π/4-DQPSK at 18 ksym/s in 25 kHz; P25 Phase 2 is H-CPM/H-DQPSK TDMA.

**One decoder, many protocols:** a single NEON/SSE2 front end (channel filter → RRC → FM discriminator or
differential detector → symbol timing → soft 4-level slicer) feeds a protocol detector that looks for each system's
sync words in parallel (they are distinct 24–48-bit patterns), so the user never chooses "DMR or NXDN": we say what it
is. Existing decoders (DSD and its forks) make the user pick or try sync words serially on hard symbols, and their
issue trackers are full of "wrong modulation selected" and Pi CPU complaints from running several instances. Through
the channeliser, a server can watch several repeater outputs at once. **Pi 2: light per channel (~5–10 %).**

### 3.2 Per system: what is free to show

| System | Free standard | Open references (licence) | What we may show without voice | Activity (UK/EU 2026) |
|---|---|---|---|---|
| **DMR** Tier I/II/III | ETSI TS 102 361-1…4 ✓ (free) | DSDcc (GPL-3 ✓), sdrtrunk (GPL-3 ✓); dsd-fme ⚠ mixed | amateur: radio ID → callsign (RadioID.net database), talkgroup, colour code, slot activity, **talker alias**, embedded GPS (LRRP), SMS/short data | **very high**: hundreds of UK amateur repeaters (BrandMeister/Phoenix/FreeDMR); also commercial PMR everywhere (⚖) |
| **dPMR** (incl. dPMR446) | ETSI TS 102 490 / 102 658 ✓ | DSDcc (GPL-3 ✓) | colour code, IDs, status | dPMR446 licence-free handhelds; some business use (⚖) |
| **NXDN** (48/96) | NXDN Technical Specifications, free from the NXDN Forum ✓ | DSDcc (GPL-3 ✓), sdrtrunk (GPL-3 ✓) | RAN, unit IDs, talkgroup, site info | some UK business/utility; small amateur use (⚖ outside ham bands) |
| **D-STAR** | JARL D-STAR specification (English translation free) ✓ | DSDcc (GPL-3 ✓) (dsd-fme's D-STAR header files are GPL-2-only ✗) | callsigns (MY/UR/RPT1/RPT2), **slow-data text**, D-PRS positions → our APRS map | moderate amateur use; ICOM repeaters |
| **YSF (System Fusion C4FM)** | not formally published by Yaesu; structure documented in open MMDVM-family code (GPL-2, ✗ for reading) and DSDcc (GPL-3 ✓) | DSDcc (GPL-3 ✓) | callsigns (FICH/header), DN/VW mode, GPS in some radios | **high** amateur use in the UK (Fusion repeaters common) |
| **P25 Phase 1** | TIA-102 suite is **paid** ($); some older documents public | OP25 (licence ⚠ check), sdrtrunk (GPL-3 ✓) | ⚖ NAC, system/site IDs, encrypted-or-clear | almost none in the UK (some US-linked bases); big in North America |
| **P25 Phase 2** | TIA-102 paid | sdrtrunk (GPL-3 ✓) | ⚖ same as Phase 1 | North America |
| **TETRA** | ETSI EN 300 392-2 ✓ (free) | osmo-tetra (AGPL-3 ✓ for reading) | ⚖ MCC/MNC, location area, colour code, encryption class from broadcast channels only | UK Airwave (police/ambulance/fire, TEA2-encrypted) until ESN; private TETRA at airports/transport |
| **MPT1327** (UK analogue trunking control) | MPT 1327/1343 (UK government specifications, historically freely published) ✓ | none clean known | ⚖ system identity only; channel activity | declining; some UK utilities/transport left |

**Amateur first, everything else format-only.** As in `DECODER-RESEARCH.md` §4.2: in the amateur bands (decided from
the band plan and a callsign in the header) show callsigns, talkgroups, talker alias, GPS and text; outside them show
system type, colour code / NAC / RAN, "encrypted" or "clear", and nothing addressed to an individual, unless an admin
opt-in for their jurisdiction, never public.

### 3.3 The voice codecs: honest legal and patent status ⚖

| Codec | Used by | Patent status (best reading, **not verified against patent registers**) | Copyright/implementation status | Our position |
|---|---|---|---|---|
| **Codec2** (700C/1300/1600/2400/3200) | FreeDV, M17 | designed to avoid patents; none known | LGPL-2.1 | ✓ **ship** (server-side dynamic link) |
| **LPCNet / RADE** | FreeDV 2020, RADE | none asserted; BSD | BSD-2/3 | ✓ ship where the CPU allows |
| **IMBE** (full rate, 7200 bit/s with FEC) | P25 Phase 1, Inmarsat-M | DVSI's IMBE patents were filed around 1989–1995; with 20-year terms they would all have expired by about 2015. Secondary sources say so; we have not checked each patent. | the IMBE description is in TIA-102.BABA (paid). mbelib (ISC) implements it from public descriptions. | **Do not offer.** Patent is not the blocker: P25 is public-safety radio and not intended for general reception in the UK (WTA s.48) and is restricted in places elsewhere. A public server decoding police voice for anyone on the internet is the worst position. |
| **AMBE** (original, "AMBE 2020"-era 3600×2400) | D-STAR | the core AMBE patents date from the mid-1990s and are **plausibly expired** (late 2010s); DVSI may hold later improvement patents. Unverified. | mbelib (ISC) implements it. DVSI considers its vocoders proprietary. | **Not now.** If a proper patent search confirms that every patent reading on D-STAR's AMBE has expired, D-STAR *amateur* voice could be reconsidered. Until then, signalling only. |
| **AMBE+2** (half rate 3600×2450) | DMR, dPMR, NXDN, YSF DN, P25 Phase 2 | filed in the 2000s; at least one family member is believed to run to **2028** (`DECODER-RESEARCH.md` §4.1 cites US 8,359,197 to 2028-05-20: unverified) | mbelib-style software exists | ✗ **No software AMBE+2 decoder.** Re-examine in 2029 at the earliest. |
| **ACELP (TETRA)** | TETRA | licensed on FRAND terms via ETSI; early-1990s ACELP patents may well have expired, but ETSI's reference code carries its own terms | ETSI reference C code with the standard | ✗ never: TETRA networks in the UK are encrypted public-safety systems. |

**The licensed-hardware route (later, amateur only).** OpenWebRX gives amateur DMR/D-STAR/YSF voice through a
*hardware* DVSI vocoder chip the owner bought (ThumbDV, DV3000, AMBE-3000 boards), driven via codecserver
(GPL-3 ✓). The chip is licensed by DVSI, so no patent is ours to worry about. A VibeServer could offer the same: if
the owner plugs in a DVSI chip, amateur-band digital voice plays (server-side, sent as Opus); without one, signalling
only, and the control does not appear at all (our "no dead controls" rule). **Effort M** after the signalling decoder.
Never outside amateur frequencies.

**Effort for the signalling decoder:** L for the shared front end plus DMR and D-STAR/YSF; then S/M per extra protocol.
**Test signals:** our RTL/RSP on local 70 cm/2 m amateur repeaters (many in the UK); DSDcc sample files; sigidwiki.

---

## 4. Aviation (beyond ADS-B; ACARS/VDL2/HFDL are the other agent's)

### 4.1 Mode S beyond ADS-B: Comm-B / EHS (BDS 4,0 / 5,0 / 6,0), DF4/5/20/21, ACAS — **build now (parser only)**
1. **Standards:** ICAO Annex 10 Vol IV and Doc 9871 (paid $); **Junzi Sun, "The 1090 Megahertz Riddle"** (TU Delft,
   open-access book, free ✓) explains every BDS register and how to infer which one a reply carries.
2. **Open:** pyModeS (**GPL-3** ✓), readsb (GPL-3 ✓).
3. **How it works:** Comm-B replies (DF20/21) carry a 56-bit MB field with no register number; the receiver must infer
   the register from field validity and plausibility against the aircraft's ADS-B state. EHS (mandatory in UK/EU
   airspace) gives selected altitude, roll, track rate, ground speed, true airspeed, heading, IAS and Mach
   (BDS 4,0/5,0/6,0); MRAR BDS 4,4 gives wind and temperature on some aircraft. Address/parity is overlaid on the CRC,
   so the ICAO address comes from the CRC remainder and must be cross-checked against known aircraft.
4. **Better:** checked 2026-10-31: `vibe_adsb.h` already accepts DF20/21 and takes their altitude and squawk, but
   **ignores the 56-bit MB field**, so all the Comm-B content is thrown away today. The rest is a parser plus a plausibility scorer; deriving **winds aloft** per
   aircraft and drawing them on the map is a feature most front ends lack. **Pi 2: trivial on top of ADS-B.**
5. **Legal:** surveillance replies from aircraft, same footing as ADS-B. Fine.
6. **Activity:** Comm-B is constant over the UK (interrogated by NATS radars); it is roughly half the 1090 traffic.
7. **Effort S/M.** Test: our existing ADS-B captures.

### 4.2 UAT 978 MHz — **later (Americas only)**
DO-282B is paid ($); FAA public material describes FIS-B products. **dump978-fa is BSD-2** ✓. 1.041667 Mbit/s
binary CPFSK, RS(30,18)/(48,30) for ADS-B messages, ground uplinks with RS(92,72) and FIS-B weather (NEXRAD, METAR,
TAF, NOTAMs). Needs a whole radio at ≥ 2 MS/s (a radio role, like ADS-B). **Pi 2: medium–heavy.** US only; none in
Europe. M. Test: public US recordings.

### 4.3 ADS-L (EASA SRD-860) — **later (soon): an open, growing European conspicuity broadcast**
1. **Standard:** EASA "ADS-L 4 SRD860" technical specification, published under ED Decision 2022/024/R, free ✓
   (Issue 2 on easa.europa.eu; an Issue 4 PDF also exists: confirm the current issue first).
2. **Open:** none clean known; FLARM's own protocol is obfuscated/encrypted and must **never** be decrypted. OGN
   (Open Glider Network) is the community; its software licences vary (check before reading).
3. **How it works (from the spec, summarised):** GFSK in the 868 MHz SRD band, short packets with an address,
   position, altitude, velocity and integrity, plus a privacy (random address) option. It is to light aviation, drones
   and gliders what ADS-B is to airliners, and newer FLARM-family devices transmit it alongside FLARM.
4. **Better:** it sits naturally inside our **ISM whole-radio mode** on 868 MHz (the same capture as wM-Bus and LoRa),
   so it costs one more packet family, and it puts gliders and paragliders on our existing aircraft map.
   **Pi 2: light on top of ISM.**
5. **Legal:** a conspicuity broadcast intended for everyone (that is its purpose). Fine; honour the privacy-address bit
   (never try to link random addresses).
6. **Activity:** growing in 2025–26 in Europe, notably gliding sites; still small in the UK, where PilotAware (P3I,
   proprietary) and FLARM dominate GA.
7. **Effort M.** Test: our RTL/RSP at 868 MHz near a gliding club; EASA spec test vectors if published.

### 4.4 VOR (bearing + ident) and ILS — **later**
VOR: 30 Hz AM variable vs 30 Hz FM on a 9960 Hz subcarrier reference; bearing = phase difference; Morse ident through
our CW decoder. ILS: 90/150 Hz depth difference. Described in FAA/ICAO training material ✓ (Annex 10 Vol I is paid);
SDRangel's VOR localiser and SDR++'s VOR receiver are GPL-3 ✓. **S/M, trivial.** Fun "you are on the 247° radial of
DTY" readout for a server; on a fixed server it is a curiosity, on the app's local hardware it is a real toy.
UK VOR network is being rationalised but many remain in 2026. Legal: navigation aids. Fine.

### 4.5 Mode A/C and other 1090/1030 — **skip**
Mode A/C replies carry squawk/altitude without address; matching them to aircraft is guesswork. 1030 MHz
interrogations: low value. Skip.

### 4.6 Inmarsat Aero (Classic Aero, L-band) — **later (needs L-band hardware)**
1.5455–1.5590 GHz; P-channel 600/1200/10500 bit/s OQPSK carrying ACARS-like messages and position reports.
**JAERO is MIT** ✓ (with libacars, MIT). No free spec (ARINC 741 paid; JAERO documents behaviour). C-channel voice is
AMBE: ✗. Needs a patch antenna + LNA facing the satellite (Inmarsat 4F2 at 64°E or 3F2/4F3 elsewhere; check
current satellite positions). **Pi 2: light (600/1200), medium (10500).** ⚖ Same message-body rule as ACARS.
**M.** Coordinate with the ACARS agent's message layer.

### 4.7 FLARM, PilotAware, drone Remote ID, DJI DroneID — **skip**
FLARM requires decrypting an obfuscated protocol: never. PilotAware is proprietary. Drone Remote ID (ASTM F3411 over
BLE/Wi-Fi) and DJI DroneID **locate the pilot, a person**: excluded under our "never track people" rule, and mostly at
2.4 GHz anyway.

---

## 5. Maritime and distress

### 5.1 COSPAS-SARSAT 406 MHz (EPIRB, PLB, ELT) — **build later (background watch)**
1. **Standard:** C/S T.001 "Specification for Cospas-Sarsat 406 MHz distress beacons" and T.018 (second-generation
   beacons), free from cospas-sarsat.int ✓. The country code table is in T.001 annexes and ITU MID tables.
2. **Open:** dec406 (F4EHY; licence unclear, don't read until checked); scan406 (closed). Write from T.001.
3. **How it works (first generation):** 400 bit/s biphase-L BPSK (±1.1 rad), 160 ms unmodulated carrier, 15-bit bit
   sync, 9-bit frame sync (**000101111** for a real alert, **011010000** for self-test), short (112-bit) or long
   (144-bit) messages with BCH(82,61) and BCH(26,14) codes; country, protocol, beacon ID, optional GNSS position.
   Bursts of 440/520 ms every ~50 s, anywhere in 406.0–406.1 MHz. Second generation (T.018): DSSS-OQPSK, 300 bit/s,
   BCH(250,202); being phased in.
4. **Better:** a burst detector over the whole 100 kHz (one FFT per 10 ms block, trivial with the channeliser), then
   a coherent BPSK demod with BCH error correction (most hobby decoders are hard-decision with no correction). Watch
   unattended alongside AIS/DSC. **Pi 2: trivial–light.**
5. **Legal/privacy ⚖:** distress signals are for SAR; a real alert can carry a person's position. Policy: show
   "406 MHz beacon · TEST (self-test)" or "406 MHz beacon · REAL ALERT · country", beacon type, **position only to the
   admin**, never in a public gallery, with "Coastguards already have this alert: you cannot help by radio." Self-tests
   are what people actually hear (marinas, airfields, and the beacon sellers' test bursts).
6. **Activity:** real alerts are rare locally; self-test bursts are common near harbours and airfields.
7. **Effort M** (first gen); +M for second gen. Test: public self-test recordings; T.001 example messages; our RTL/RSP
   near a marina. Never transmit test signals ourselves (it would be a false alert).

### 5.2 AIS extras: binary messages (6/8, ASM), SART/MOB/EPIRB-AIS — **build now (parser only)**
`AIS-APRS.md` already shows 970/972/974 SART/MOB/EPIRB as DISTRESS BEACON. Still missing (no mention found):
**binary messages 6 and 8 and the application-specific messages** (IMO SN.1/Circ.289 ✓ free: meteorological and
hydrographic data from AtoN and port stations: wind, tide, visibility, water level; area notices). Many UK/EU AtoN
stations send met/hydro data. Parser only: **S, trivial.** Also message 14 safety text from SARTs ("SART ACTIVE"/
"SART TEST") if not already shown, and message 21 AtoN virtual/real flags.

### 5.3 VDES (ITU-R M.2092) — **skip for now, watch**
AIS's successor (ASM channels 2027/2028, VDE terrestrial and satellite); free ITU spec ✓; operational use still
patchy in 2026. Revisit when ASM traffic is common in Europe. M.

### 5.4 Inmarsat-C EGC SafetyNET — **later (needs L-band hardware)**
1.5370–1.5450 GHz, 1200 sym/s BPSK, K=7 r=½ convolutional code, interleaved frames, unique word sync; the NCS common
channel carries **EGC SafetyNET** maritime safety broadcasts (NAVAREA warnings, met forecasts): a satellite NAVTEX.
No free full spec (IMO SafetyNET manual describes the service ✓; the air interface is Inmarsat's). Open references:
SDRangel's Inmarsat demodulator (GPL-3 ✓); Scytale-C (repository gone; licence unknown ✗). **Pi 2: light.**
⚖ SafetyNET broadcasts are for general reception (show); private Inmarsat-C messages to ships are not (format only).
Needs a patch antenna + LNA. **M/L.** Activity: constant worldwide. Value: medium (maritime listeners), the content
complements our NAVTEX.

### 5.5 SITOR-B HF MSI, NAVTEX — **have** (§1.9 check only).

---

## 6. Satellites (decoders on the designed engine)

**What the engine already gives us** (`SATELLITE-ENGINE.md`): SGP4 from **OMM JSON** (the 5-digit TLE catalogue ran
out in July 2026), CelesTrak via the **vibesdr.net mirror only** (`sats.json`), an open-loop Doppler prediction plus
a closed-loop trim applied in the channel's DDC/NCO, `loop` kinds `fll` / `none` / `costas`, and the tracker moving the
channel as a **server move under the shared-dial contract** (never through `vibe_tune_pace.h`, never a user tune, never
re-arming band defaults). `RADIO-ROLES.md` gives pass windows on Dedicated radios; `DECODERS-GALLERY.md` gives
capture → deferred decode → gallery → client-assisted decode. **VibeServer Lite has no Dedicated role and no capture**,
so every pass-scheduled or capture-based decoder below is full-VibeServer only; live decoders (SSTV, APRS, cubesat
AFSK) can still run on Lite when a listener tunes them by hand.

Each decoder below says only what it needs from that design.

### 6.1 Meteor-M LRPT — **already briefed; build as designed, with a 2026 status correction**
- **Spec:** CCSDS 131.0-B (TM synchronisation and channel coding) ✓, CCSDS 132.0-B ✓, plus the Meteor JPEG/MCU
  packet layout (described by the Russian operator's format notes and community write-ups).
- **References:** SatDump (**GPL-3** ✓, confirmed 2026-10-31: inspiration/oracle only, as the brief says);
  **meteor_demod and meteor_decode (dbdexter, MIT ✓)** and **MeteorDemod (Digitelektro, MIT ✓)** are smaller,
  permissive and readable: better references for understanding than SatDump's large tree.
- **Better:** the brief's capture + deferred decode is right for a Pi 2 (capture **light**; Viterbi + RS offline
  **heavy** but deferrable). SIMD: the K=7 Viterbi ACS is the hot loop (64 states → 4 NEON q-registers of int16);
  soft symbols stored as int8 halve the brief's 180 MB raw-IQ figure (the brief already has a soft-symbol mode).
- **Engine plug-in:** `loop: costas`, feed-forward Doppler, pass window with the max-elevation gate (default 15°).
- **⚠ Brief outdated in places (2026 status):**
  - **Meteor-M N2-3** (NORAD 57166): operational, but its LRPT antenna did not fully deploy, so the signal is weak and
    direction-dependent (Dartcom rated reception "poor" in 2025). Expect partial images on modest antennas; the
    "don't blame the decoder" max-elevation hint matters more than the brief assumes.
  - **Meteor-M N2-4** (NORAD 59051), the example in the brief's `sats.json`: decoded by SatNOGS observers in June 2026,
    but **no decoded data in the 30 days to 2 August 2026**. Its LRPT may be off or moved. The brief's first test
    ("capture a real Meteor M2-4 pass") may need to become N2-3. Keep everything per-satellite in `sats.json` (the
    brief's trap already covers mode/frequency changes); add an "LRPT not heard recently" state fed from the
    vibesdr.net mirror (it can read SatNOGS's last-decoded date) so the app greys a dead satellite with a reason.
  - NOAA APT: the brief is right: dead since August 2025. Nothing to add.

### 6.2 ISS APRS and ISS SSTV — **already briefed (gallery phases 2–3)**
Ride on our existing APRS and SSTV decoders with `loop: fll`. Nothing new except: ISS APRS is intermittent (the
digipeater is switched off for crew activity and hardware swaps); the mirror should carry ARISS status where it can,
rather than letting users conclude the decoder is broken.

### 6.3 Cubesat telemetry: AFSK 1200, GMSK/FSK 9600 AX.25, BPSK 1200 (FUNcube) — **build later (after 2.1)**
1. **Standards:** AX.25 ✓, G3RUH ✓ (§2.1); **FUNcube**: AMSAT-UK publishes the downlink format (BPSK 1200 bit/s with
   K=7 r=½ convolutional code, interleaving, RS, a 256-byte frame) ✓; per-satellite payload formats in SatNOGS DB
   and its Kaitai struct decoders (licence to check before reading; believed AGPL-3, which is fine for reading).
2. **Open:** gr-satellites (**GPL-3** ✓) is the reference for 100+ satellites; FUNcube Dashboard (closed).
3. **How it works:** satellites mostly use 1200 AFSK, 9600 GMSK/G3RUH AX.25, a few BPSK 1200/2400 with CCSDS-style
   coding, and many custom framings (CSP, Mobitex-like, AX100). Weak spots of ground stations: 70 cm Doppler of ±10 kHz
   at ~215 Hz/s is more than a 9600 demod's own AFC wants to chase; this is precisely what the engine fixes.
4. **Better:** the engine supplies feed-forward Doppler so the demod sees a near-stationary carrier; `loop`
   should be a **"demod-tracked"** kind for GMSK/BPSK (the gap already noted in `DECODER-RESEARCH.md` §3.7A item 3);
   decode telemetry into readable values (battery V, temperatures, mode) for a curated few satellites rather than raw
   hex for hundreds. **Pi 2: light per pass.**
5. **Engine plug-in:** `sats.json` needs `mode` values `GMSK9600`/`FSK9600`/`BPSK1200` with symbol rate and a
   **`framing`** field (`ax25`, `fx25`, `funcube`, `ccsds`, `csp`, …): the gap already listed in
   `DECODER-RESEARCH.md` §3.7A item 1. A **pass-triggered background VFO** (item 4 there) is how this runs unattended.
6. **Legal:** amateur satellite telemetry is for everyone. Fine.
7. **Activity:** dozens of active amateur cubesats in 2026; **FUNcube-1 (AO-73)** still listed operational (SatNOGS,
   elements March 2026; AMSAT 12th-birthday note, November 2025): a UK-built satellite, a good first target on
   145.935 MHz. **Effort M** (framework + 3–5 satellites), then S per satellite. Test: SatNOGS public observation
   audio/IQ (free downloads), our RTL/RSP passes.

### 6.4 SSDV from satellites — **with 2.11**
Some cubesats send SSDV images; the same SSDV layer as balloons. Gallery.

### 6.5 GOES HRIT/LRIT, GK-2A LRIT, Elektro-L — **later, and not for UK hosts**
1. **Standards:** CCSDS ✓; NOAA "GOES-R HRIT/EMWIN" specifications free ✓ (HRIT 400 kbit/s BPSK at 1694.1 MHz with
   EMWIN text); KMA publishes GK-2A LRIT/HRIT documents ✓ (64 kbit/s LRIT at 1692.14 MHz).
2. **Open:** SatDump (GPL-3 ✓), goestools (BSD-2, believed: check), xrit-rx (GK-2A; licence to check).
3. **How it works:** BPSK, K=7 Viterbi, RS(255,223), CCSDS virtual channels, xRIT file headers, Rice (CCSDS 121.0)
   compressed or JPEG images, EMWIN text bulletins.
4. **Better:** reuses the LRPT CCSDS chain almost entirely; geostationary means **no Doppler and no pass**: the engine
   only needs a "geostationary" satellite kind (fixed azimuth/elevation for aiming, no `loop`), and RADIO-ROLES needs a
   **continuous** Dedicated task rather than a windowed one. **Pi 2: LRIT light–medium; HRIT heavy** (927 ksym/s
   Viterbi in real time; a Pi 2 would need capture-and-defer like LRPT, which at HRIT rates is a lot of disk).
5. **Legal:** public meteorological broadcasts. GK-2A: some file types are encrypted with keys KMA issues on request:
   decode only the open products unless the owner supplies their own key.
6. **Activity/geometry ⚠:** **from the UK neither GOES-East (75.2°W) nor Elektro-L at 76°E is usable** (both near 0°
   elevation from London); GK-2A (128.2°E) is invisible. Meteosat's own L-band LRIT ended; Europe gets EUMETCast via
   Ku-band DVB-S2. So: of value to hosts in the Americas (GOES; our Brazilian hosts) and Asia-Pacific (GK-2A), with a
   ~1 m dish, a 1.7 GHz LNA/filter and an RTL or better.
7. **Effort L** (after LRPT). Test: SatDump-community public recordings.

### 6.6 HRPT/AHRPT (Metop, FengYun-3, Meteor HRPT) — **skip for now**
1.7 GHz, 665 kbit/s–3 Mbit/s, needs a tracking dish with azimuth **and elevation** rotator (`ANTENNA-SWITCH-ROTATOR.md`
has azimuth only). Beyond a Pi 2 in real time. Already tier 3 in `DECODER-RESEARCH.md`.

### 6.7 Iridium, Orbcomm — **skip**
- **Iridium:** gr-iridium/iridium-toolkit (licences ⚠ unchecked); content is private (pager, SBD, AMBE voice); only
  ring-alert beam positions are harmless and they are of little interest. ⚖ Skip.
- **Orbcomm (137–138 MHz):** commercial messaging, proprietary; the first-generation constellation is largely
  retired. Identify only.

### 6.8 QO-100 (geostationary amateur transponder) — **not a decoder; one DSP note**
Hams in Europe/Africa/Middle East receive QO-100 at 10.489 GHz via an LNB into an RTL/RSP. Our SSB/CW/FT8 decoders
already work there; the useful addition is **LNB drift correction locked to the transponder's beacon** (a fixed-point
NCO trim, like the engine's `trim` but referenced to a beacon instead of an orbit). Note it for the engine's phase 3
(linear transponders) rather than as a decoder.

---

## 7. Paging, trunking and miscellaneous VHF/UHF

| Mode | Standard | Status in 2026 | Recommendation |
|---|---|---|---|
| **ERMES** | ETSI ETS 300 133 ✓ | dead in Europe since the 2000s | **skip** |
| **Golay (GSC)** | Motorola, partly documented; multimon-ng has a GSC demod (GPL-2+) | almost gone; a few US systems | **skip** |
| **Mobitex** | Ericsson (documented by third parties) | data network largely closed; the *framing* survives in some cubesats (covered by §6.3) | **skip** as paging |
| **ReFLEX** | Motorola proprietary | dying US two-way paging | **skip** |
| **5-tone selcall** (ZVEI/CCIR/EEA/EIA) | tone tables public ✓ | still used in European PMR and German fire-brigade alerting | ⚖ format only ("ZVEI-1 5-tone") by default; the digits are addressing (admin opt-in). S, trivial |
| **German FMS / ZVEI BOS** | public | German emergency services; listening is restricted in Germany | **skip** |
| **MPT1327** | §3.2 | declining UK trunking | ⚖ identity-only, later |
| **US train EOT/HOT** (457.9375/452.9375 MHz, 1200 bit/s FSK) | AAR S-9152 (paid); behaviour documented publicly; SDRangel has an End-of-Train demod (GPL-3 ✓) | US/Canada only; none in the UK | **later, low**: railway telemetry (brake pressure, motion), no personal data; receive only (HOT commands brake trains, so never transmit, which we never do) |
| **US ATCS** (railway signalling, 900 MHz) | AAR | US only | **skip** |
| **US Itron ERT meters (SCM/IDM, 900 MHz)** | rtlamr (AGPL-3 ✓) | US/Canada | **later**, same privacy rule as wM-Bus (consumption reveals occupancy) |
| **Zigbee, Z-Wave, Bluetooth LE advertising** | Zigbee spec free with registration; Z-Wave ITU G.9959 ✓; BLE Core Spec ✓ | very common | **skip:** home automation and BLE adverts reveal who is home and let phones, tags (AirTag/Tile) and wearables be **tracked**; BLE/Zigbee are 2.4 GHz anyway. Z-Wave (868.42 MHz) could technically join ISM but adds only privacy risk |
| **LoJack-type vehicle recovery, car/phone tracking** | — | — | **never** (tracks vehicles/people) |
| **GSM-R, LTE, 5G, cell broadcast** | — | — | **excluded** (as before) |
| **GRAVES radar meteor counter** (143.050 MHz) | none needed: the French space-surveillance radar's CW carrier reflected by meteor trails | operating in 2026; very popular with UK/European radio-meteor observers | **build now:** a display/logging tool, not a decoder: narrow FFT around 143.050 MHz, ping detector, hourly counts into the gallery (shower peaks!). **S, trivial.** Legal: we receive reflections of a radar carrier, no message. Same tool works on BRAMS (49.97 MHz, Belgium) |
| **VLF SID monitoring** (sudden ionospheric disturbances) | none needed: log the amplitude of VLF MSK stations (GQD 19.6 kHz Anthorn, DHO38 23.4, NAA 24 kHz, …) | RX888/UberSDR servers reach VLF | **later:** a logger (as SuperSID does), graphs solar flares. **S, trivial.** We log *amplitude only*; the MSK content is encrypted naval traffic, never decoded |
| **Radio Teleswitch (198 kHz phase modulation, UK)** | the RTS format was published by the energy industry | BBC Radio 4 long wave was due to close on **27 June 2026**; whether RTS data continues is unclear | **skip** (dying; check the carrier is still on air before even an archive decoder) |
| **DAB EWF / alarm announcements** | ETSI TS 104 089 ✓ | not deployed in the UK; trials in Germany | a **check on our DAB decoder**, not a new decoder: honour the alarm announcement flag if seen |
| **DARC (FM 76 kHz data)** | ETSI EN 300 751 ✓ | largely gone in Europe | **skip** |
| **RDS2** (extra RDS streams on 66.5/71.25/76 kHz, RFT logo files) | RDS Forum/IEC 62106 (IEC parts paid; RDS Forum guidelines free) | a few stations in Italy and central Europe transmit RDS2 logos | **later, small:** our Advanced RDS could add the extra subcarriers and RFT, which feeds the queued logo cache (memory `logo_cache_plan`) with *on-air* logos. M, light |

---

## 8. Time signals: the phase codes

- **DCF77 pseudo-random phase modulation** (PTB publication ✓): a 512-chip ±13° phase sequence each second carrying
  the same time code; gives far better timing and works when the AM code is unreadable. Needs the carrier PLL coming
  with SAM. **M, trivial.** Later (accuracy fans, not general listeners). **DCF77 Meteotime weather is encrypted and
  proprietary: never.**
- **WWVB phase modulation** (NIST "Enhanced WWVB" format, 2013, free ✓): BPSK time code since 2012, the reason modern
  WWVB clocks work; our WWVB is amplitude-only. **S/M.** Later, US users.
- **JJY is built** (`decoders/time_decoder.h`: MSF, DCF77, WWV/WWVH, RWM, WWVB, JJY). **BPC, CHU and TDF (ALS162)**
  are planned in `DECODER-RESEARCH.md` §3.9 but not in the decoder yet; TDF waits for the SAM carrier PLL.
- **Loran-C / Chayka, eLoran:** UK eLoran at Anthorn ended in 2015; Russian Chayka transmits. Navigation pulse timing,
  no decoding of interest. **Skip.**

---

## 9. What other open-source suites offer that we still lack (modes only)

| Suite | Modes it has that we don't (after this survey's lists) |
|---|---|
| **OpenWebRX+** (AGPL-3) | digital voice via codecserver + hardware AMBE (§3.3), 5-tone selcall/ZVEI/EEA/EIA (§7), JT65/JT9 (§2.5) |
| **SDR++** (GPL-3) | M17 (§2.3), VOR (§4.4), Meteor demod (§6.1), Falcon 9 telemetry (SpaceX now encrypts: skip), weather-sat APT (dead) |
| **SDRangel** (GPL-3) | DSD digital voice (§3), FreeDV (§2.2), M17, VOR/ILS (§4.4), End-of-Train (§7), Inmarsat (§5.4), DATV (TV: declined), radio astronomy (not a decoder), noise figure |
| **fldigi** (GPL-3) | MT63, DominoEX, FSQ, PSK-R/multi-carrier, 8PSK, Hell variants, Throb, IFKP (§2.4) |
| **multimon-ng** (GPL-2+) | DTMF (§2.12), ZVEI/CCIR/EEA/EIA/DZVEI/PZVEI (§7), FMS-FSK (skip), GSC (skip), X10 (skip), CLIP FSK caller ID (telephony: excluded), UFSK1200, HAPN 4800, FSK9600 (§2.1) |
| **direwolf** (GPL-2+) | 9600 G3RUH, FX.25, IL2P (§2.1), 2400 QPSK/4800 8PSK V.26/V.27 packet (rare: skip) |
| **dsd-fme** (mixed) | DMR, dPMR, NXDN, P25 1/2, D-STAR, YSF, ProVoice/EDACS, X2-TDMA (§3; EDACS/ProVoice/X2 are US legacy: skip) |
| **SatDump** (GPL-3) | LRPT (briefed), HRPT/AHRPT (§6.6), GOES/GK-2A/Elektro xRIT (§6.5), Inmarsat STD-C/Aero (§4.6, §5.4), many science satellites in S/X band (beyond our hardware) |
| **gr-satellites** (GPL-3) | 100+ cubesat framings (§6.3) |
| **rtl_433** (GPL-2+) | our ISM decoder is clean-room and covers the families we chose; nothing in rtl_433 outside ISM's scope |

---

## 10. PRIORITISED TABLE

Columns: **Value** to users (H/M/L, UK/EU first, worldwide noted); **Legality** (✓ fine; ⚖ format/opt-in; ✗);
**Spec** (✓ free; $ paid; P proprietary; – none needed); **Ref** (a GPL-3-compatible open reference exists to read or
run as an oracle); **Effort** S/M/L/XL; **Pi 2** class per channel/stream.

| # | Mode | Value | Legal | Spec | Ref | Effort | Pi 2 | Recommendation |
|---|---|---|---|---|---|---|---|---|
| 1 | **Mode S Comm-B / EHS** (DF20/21, BDS 4,0/5,0/6,0/4,4) | H | ✓ | $ + free book | ✓ pyModeS | S/M | trivial | **Build now:** parser on our existing ADS-B demod; adds speeds/headings/winds aloft to every aircraft |
| 2 | **AIS binary/ASM (msgs 6/8, met/hydro), msg 14/21 extras** | M–H coastal | ✓ | ✓ | ✓ | S | trivial | **Build now:** parser only; AtoN weather on the map |
| 3 | **NCDXF/IARU beacon tracker** | H (HF) | ✓ | ✓ | – | S | light | **Build now:** reuses CW; "propagation now" grid; perfect background profile |
| 4 | **fldigi completions: DominoEX, FSQ, PSK125–1000/PSK-R, 8PSK, Hell variants** | M | ✓ | ✓ | ✓ fldigi | S each | trivial–light | **Build now:** each is days on existing cores |
| 5 | **AX.25 9600 G3RUH + FX.25 + IL2P** | M (H for cubesats) | ✓ | ✓ | ✓ direwolf (GPL-2+) | M | light | **Build now:** unlocks cubesat telemetry; FX.25/IL2P rescue APRS frames for free |
| 6 | **UKHAS / Horus Binary / SSDV** | M (UK-flavoured, gallery) | ✓ | ✓ | ✓ horusdemodlib LGPL | S/M | trivial–light | **Build now:** RTTY + 4-FSK we have; live balloon pictures; share SondeHub/map with the sonde agent |
| 7 | **GRAVES meteor counter + QRSS grabber** | M (loved niche) | ✓ | – | – | S each | trivial | **Build now:** display/logging tools into the gallery |
| 8 | **DTMF** | L–M | ✓ amateur / ⚖ else | ✓ | ✓ | S | trivial | **Build now:** tiny; amateur frequencies only |
| 9 | **M17 (with Codec2 voice)** | M, growing | ✓ | ✓ | ✓ m17-cxx-demod GPL-3 | M | light | **Build later (next):** the one fully legal digital voice; shares the 4-FSK front end with #11 |
| 10 | **FreeDV 700D/700E/1600** (RADE/2020 on strong servers only) | M | ✓ | ✓ | ✓ codec2 LGPL, radae BSD | M (+L RADE) | light–medium (RADE beyond) | **Build later (next):** codec2 dynamic link on the server; benchmark-gated modes |
| 11 | **Digital-voice signalling: DMR, D-STAR, YSF, NXDN, dPMR (no voice)** | H (UK amateur DMR/YSF) | ✓ amateur / ⚖ else | ✓ (YSF partial) | ✓ DSDcc/sdrtrunk GPL-3 | L, then S/M each | light | **Build later:** one auto-detecting 4-FSK/GMSK front end; "who's on this repeater", talker alias, GPS |
| 12 | **ALE 2G** | M–H (utility DX) | ✓ amateur / ⚖ else | ✓ ASSIST | – | M | light (medium for a net) | **Build later:** soft Golay + channeliser across a net |
| 13 | **Cubesat telemetry (FUNcube BPSK 1200, 9k6 AX.25)** | M | ✓ | ✓ | ✓ gr-satellites | M | light | **Build later:** after #5 and the engine; `framing` field + pass-triggered VFO |
| 14 | **Meteor-M LRPT** | H (worldwide) | ✓ | ✓ CCSDS | ✓ MIT refs + SatDump | L | heavy, deferred | **Build as briefed;** re-check N2-3/N2-4 status first (§6.1) |
| 15 | **COSPAS-SARSAT 406 (gen 1, then gen 2)** | M | ⚖ position admin-only | ✓ T.001/T.018 | – | M (+M) | trivial–light | **Later:** background watch beside AIS/DSC |
| 16 | **ADS-L (EASA SRD-860)** | M, growing in EU | ✓ | ✓ EASA | – | M | light (inside ISM) | **Later (soon):** a packet family in ISM mode; gliders on the aircraft map |
| 17 | **MT63** | L–M (emcomm) | ✓ | ✓ | ✓ fldigi | M | light | **Later** |
| 18 | **Licensed-hardware AMBE voice (owner's DVSI chip)** | M | ✓ amateur only | – | ✓ codecserver GPL-3 | M | trivial (chip decodes) | **Later:** after #11; control appears only when a chip is present |
| 19 | **Utility classifier (baud/shift/PSK order) → STANAG/MIL-110/PACTOR/VARA/ARQ-E identify** | H (utility "what is it?") | ✓ format only | partly | partial | M | light | **Later:** as signal tags phase 2 (`DECODER-RESEARCH.md` §5) |
| 20 | **VOR bearing / ILS** | L–M | ✓ | ✓ (training docs) | ✓ SDRangel/SDR++ | S/M | trivial | **Later** |
| 21 | **DGPS skimmer** | L (UK/Norway closed) | ✓ | M.823 ✓ / RTCM $ | ✓ gpsd BSD | S | trivial | **Later:** cheap add-on to the NDB skimmer; continental stations at night |
| 22 | **JT65 / JT9 / JT4** | L | ✓ | ✓ | ✓ WSJT-X | M each | medium bursts | **Later:** into Band spots' registry |
| 23 | **HamDRM digital SSTV** | L–M | ✓ | partial | ⚠ QSSTV (check) | M | light | **Later:** reuses our DRM OFDM |
| 24 | **RDS2 + RFT logos** | L–M | ✓ | partial | – | M | light | **Later:** feeds the logo cache plan |
| 25 | **Inmarsat-C EGC SafetyNET / Inmarsat Aero** | M (maritime/aviation), needs L-band kit | ✓ SafetyNET / ⚖ messages | partial | ✓ SDRangel, JAERO MIT | M/L | light | **Later** |
| 26 | **GOES HRIT/LRIT, GK-2A LRIT** | M outside Europe; nil from the UK | ✓ | ✓ | ✓ SatDump | L | medium–heavy | **Later:** after LRPT; Americas/APAC hosts with a dish |
| 27 | **DCF77 / WWVB phase codes** | L | ✓ | ✓ | – | M | trivial | **Later:** after the SAM carrier PLL |
| 28 | **VLF SID logger** | L–M (science) | ✓ amplitude only | – | – | S | trivial | **Later:** RX888 servers |
| 29 | **UAT 978** | M in US, nil in Europe | ✓ | $ | ✓ dump978 BSD-2 | M | medium–heavy | **Later:** US hosts, radio role |
| 30 | **5-tone selcall, MPT1327, non-amateur trunking identity** | L | ⚖ | ✓ | partial | S–M | trivial | **Later, format-only;** addresses behind admin opt-in |
| 31 | **US EOT/HOT, Itron ERT** | L (US only) | ✓ / ⚖ privacy | $ / RE | ✓ SDRangel / rtlamr AGPL | S/M | trivial | **Later, low** |
| 32 | **ARDOP** | L | ✓ | ✓ | ✓ ardopcf MIT | M | light | **Later, low** |
| 33 | **ALE 3G/4G, NAVDAT, VDES** | L in 2026 | ✓ / ⚖ | ✓ | – | M/L | light | **Skip now, watch** |
| 34 | **PACTOR I–IV, VARA, WINMOR, CLOVER, Throb, ARQ-E family** | — | P / dead | P | – | — | — | **Skip:** identify via #19 only |
| 35 | **ERMES, Golay, Mobitex, ReFLEX, DARC, Radio Teleswitch** | — | — | — | — | — | — | **Skip:** dead or dying on air |
| 36 | **HRPT/AHRPT** | L (needs tracking dish) | ✓ | ✓ | ✓ SatDump | XL | beyond | **Skip for now** |
| 37 | **Iridium, Orbcomm** | — | ⚖/✗ content | P | ⚠ | L | heavy | **Skip** |
| 38 | **Zigbee, Z-Wave, BLE, drone ID, LoJack, FLARM decryption** | — | ✗ privacy/tracking/decryption | — | — | — | — | **Never** |
| 39 | **Software AMBE/AMBE+2, IMBE (P25), TETRA ACELP voice** | — | ✗ patents (AMBE+2 to ~2028) / interception law | — | — | — | — | **Never in software;** D-STAR AMBE only if a patent search clears it, and amateur only |

### Notes for whoever builds the "now" list
- Items 1–8 are each **S** (a few days) and almost all ride on code we already have. Together they add more
  visible features than any single L item.
- Items 9–11 share **one SIMD 4-FSK front end** (RRC + discriminator + soft slicer + parallel sync-word search).
  Build that once, M17 first (fully legal, small spec), then DMR/D-STAR/YSF signalling.
- Every new decoder needs: a `vibe_benchmark_decoders.h` row; NEON + SSE2 + scalar with bit-exact tests
  (`DECODERS-WAVE2.md` SIMD rule); background-capable where the table says so; and a no-dead-controls gate (offered
  only where the server, the radio's range and the capture bandwidth can run it).
- **Tour and copy:** each new decoder that appears in the decoder menu must be added to the places that describe the
  menu (`AGENTS.md` grep list).

---

## 11. Sources (checked 2026-10-31 unless noted)

**Licences (GitHub repository metadata and source headers):** github.com/wb2osz/direwolf · github.com/EliasOenal/multimon-ng ·
github.com/merbanan/rtl_433 · github.com/M17-Project/libm17 · github.com/mobilinkd/m17-cxx-demod ·
github.com/lwvmobile/dsd-fme (COPYRIGHT) · github.com/szechyjs/mbelib · github.com/pflarue/ardop ·
github.com/jontio/JAERO · github.com/flightaware/dump978 · github.com/drowe67/codec2 · github.com/drowe67/radae ·
github.com/drowe67/LPCNet · github.com/projecthorus/horusdemodlib · github.com/projecthorus/radiosonde_auto_rx ·
github.com/SatDump/SatDump · github.com/daniestevez/gr-satellites · github.com/f4exb/sdrangel ·
github.com/f4exb/dsdcc · github.com/AlexandreRouma/SDRPlusPlus · github.com/DSheirer/sdrtrunk ·
github.com/junzis/pyModeS · github.com/osmocom/osmo-tetra · github.com/bemasher/rtlamr ·
github.com/jketterl/codecserver · github.com/jketterl/digiham · github.com/DJ2LS/FreeDATA ·
github.com/dbdexter-dev/meteor_demod · github.com/dbdexter-dev/meteor_decode · github.com/Digitelektro/MeteorDemod ·
github.com/jks-prv/Beagle_SDR_GPS (`_LICENSE`) · github.com/luarvique/openwebrx.

**Satellite status:** [Wikipedia: Low-rate picture transmission](https://en.wikipedia.org/wiki/Low-rate_picture_transmission) ·
[SatNOGS DB: Meteor M2-3](https://db.satnogs.org/satellite/57166/) · [SatNOGS DB: Meteor M2-4](https://db.satnogs.org/satellite/59051) ·
[WMO OSCAR: Meteor-M N2-3](https://space.oscar.wmo.int/satellites/view/meteor_m_n2_3) ·
[usradioguy: Meteor M2-2/2-3/2-4](https://usradioguy.com/?p=5849) · [Dartcom support notes](https://ftp.dartcom.co.uk/pub/preddata/winsupp.txt) ·
[SatNOGS DB: AO-73](https://db.satnogs.org/satellite/39444) · [Ham Weekly: AO-73 12th birthday (Nov 2025)](https://daily.hamweekly.com/2025/11/ao-73-funcube-1-celebrates-12th-birthday/) ·
[sigidwiki: FUNcube-1 telemetry](https://www.sigidwiki.com/wiki/FUNcube-1_Telemetry).

**DGPS:** [Trinity House: Tri-GLA DGPS discontinued 2022](https://trinityhouse.co.uk/news/tri-gla-decision-to-discontinue-dgps-service-in-2022) ·
[Trinity House Notice 2/2022](https://www.trinityhouse.co.uk/notice-to-mariners/2/2022-discontinuation-of-dgps) ·
[Kystverket: DGPS discontinued in Norway](https://www.kystverket.no/en/news/2024/discontinuation-of-dgps-service-in-january/) ·
[IALA table of DGNSS stations (Oct 2024)](https://www.iala.int/content/uploads/2025/04/Table-of-DGNSS-stations-Apr-2024-v2.0_clean.docx) ·
[Hong Kong Marine Department notice 16/2026](https://www.mardep.gov.hk/filemanager/tc/share/notices/pdf/mdn26016c.pdf).

**ADS-L:** [EASA news: ADS-L 4 SRD860 Issue 2](https://www.easa.europa.eu/en/newsroom-and-events/news/be-seen-and-be-aware-publication-new-technical-specification-ads-l-4) ·
[EASA download (Issue 2)](https://www.easa.europa.eu/en/downloads/142821/en) · [EASA download (headed Issue 4)](https://www.easa.europa.eu/de/downloads/137503/en).

**Radio 4 LW / Teleswitch:** [RadioToday: June date for Radio 4 LW closedown](https://radiotoday.co.uk/2026/05/bbc-sets-june-date-for-radio-4-long-wave-closedown/) ·
[RadioToday: closure confirmed (April 2026)](https://radiotoday.co.uk/2026/04/bbc-confirms-radio-4-long-wave-service-closure-this-year/) ·
[Hackaday: Radio Teleswitch](https://hackaday.com/tag/radio-teleswitch).

**MT63/NBEMS background:** [Wikipedia: MT63](https://en.wikipedia.org/wiki/MT63) · [ARRL: NBEMS](https://www.arrl.org/NBEMS).

**Standards named in the text (free unless marked):** MIL-STD-188-141 and -110 (DoD ASSIST, quicksearch.dla.mil) ·
ITU-R M.823 (DGPS), M.2010 (NAVDAT), M.2092 (VDES), M.1371 (AIS) at itu.int · IMO SN.1/Circ.289 (AIS ASM) ·
Cospas-Sarsat T.001/T.018 (cospas-sarsat.int) · ETSI TS 102 361 (DMR), TS 102 490/102 658 (dPMR), EN 300 392 (TETRA),
TS 104 089 (DAB EWF), EN 300 751 (DARC) at etsi.org · NXDN Forum technical specifications (nxdn-forum.com) ·
JARL D-STAR specification · M17 specification (spec.m17project.org) · codec2 documentation · CCSDS 131.0-B/132.0-B
(public.ccsds.org) · NOAA GOES-R HRIT/EMWIN specification · KMA GK-2A LRIT/HRIT documents · PTB DCF77 phase-code
publication · NIST enhanced WWVB format · ITU-T Q.23/Q.24 (DTMF) · G3RUH 9600 modem paper · FX.25 (Stensat) and IL2P
(KK4HEJ) specifications · NCDXF beacon schedule (ncdxf.org) · UKHAS telemetry (ukhas.org.uk) · J. Sun, *The 1090
Megahertz Riddle* (TU Delft OPEN). **Paid:** RTCM SC-104, TIA-102 (P25), ICAO Annex 10/Doc 9871, RTCA DO-282B/DO-260B,
ARINC 741, AAR S-9152, IEC 62106.

### Unsure, needing a check before anyone relies on it
- Every patent date in §3.3 (IMBE, AMBE, AMBE+2, ACELP): from secondary sources, not patent registers.
- libm17's "or later" status; OP25, gr-iridium, iridium-toolkit, SSDV, QSSTV, goestools, xrit-rx, LinuxALE, dec406,
  SatNOGS decoders: licences not confirmed today.
- Meteor-M N2-4's LRPT state after August 2026; whether Radio Teleswitch data outlived Radio 4 LW's 27 June 2026
  closure; the current ADS-L issue number; how many NCDXF beacons are on air.
- (Resolved 2026-10-31: DF20/21 MB fields are discarded today; JJY is built, BPC/CHU/TDF are not.)
