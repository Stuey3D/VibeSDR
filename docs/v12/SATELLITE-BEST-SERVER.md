# SATELLITE — beta90, the server half (branch `b90-satsrv`, 2026-11-21)

The plan is `SATELLITE-BEST.md` (phase 1 + "Decided" + "Still open", both recommendations taken as approved). The wire
the clients read is `SATELLITE-BEST.md` → **"Contract additions (beta90)"** (written first, for `b90-satcli`). This file
is what the server does, as built, and how it was tested.

## RESUME HERE
1. **No real signal has been through any of this.** Everything below is synthetic or our own transmitters. Wanted, in
   order (the coordinator captures; no agent touches Stuart's servers):
   - an **AO-73 sunlit pass** (USB audio, or a SatNOGS .ogg of 39444) through `fctlm` — the one unknown that can sink
     1.2: whether FUNcube's 1200 Bd keeps AO-40's **differential code** and which **coder output goes first**. The
     framer tries coherent + differential × both signs and the Viterbi tries both output orders, so either answer
     decodes; if a real pass gives syncs but `failed` climbs and `good` stays 0, suspect the scrambler bit order or the
     unused-slot placement (the last 3 interleaver cells, assumed row 79 cols 62–64) — see vibe_fctlm.h's header.
   - an **RS-44 or FO-29 CW beacon** pass (the `cw` pick), and a tune across RS-44's passband with AUTO DOPPLER on
     (the passband rule).
   - a **SONATE-2 9k6 frame from DP0SNX** (Germany passes only) — the APID 100 decode (vibe_sonate2_tlm.h).
   - an **unattended METEOR pass** on a server with background decoding running (pause → LRPT to the gallery → resume).
2. **iOS** — the prebuilt DSP lib owes a `build_ios.sh` rebuild (the shim, the engine, new headers). Not built here.
3. **Android native lib not built** on this branch (NDK). New headers: vibe_bpsk_rx.h, vibe_fctlm.h, vibe_fctlm_tx.h
   (benchmark only), vibe_sonate2_tlm.h — header-only, no new .cpp, so the Android CMake needs nothing new.
4. **Copy (AGENTS.md grep list) — NOT edited here** (other agents own those files this beta): AboutOverlay release note
   lines to add, suggested: "SATELLITE: CW beacons decoded on a pick; AO-73 (FUNcube) telemetry decoded into values;
   SONATE-2 housekeeping with the mission's own calibrations; transponders labelled by mode (Mode B · U/v); tune
   anywhere inside a transponder's passband without losing the Doppler lock; AO-7 Mode A on HF radios and Icom rigs;
   unattended satellite passes (Setup → beside Background Decoding)". website/index.html likewise.
5. **Unattended passes not on a locked range** (it would need a background channel of its own — say so, as the setup
   page does) and **no recording** (no control offered). Gallery items from a pass are the decoders' ordinary items
   (live origin); the contract's `pass` metadata on gallery entries is **not built**.
6. The **idle-timeout exemption** (Decided 4) is unit-tested (Engine::activeUntil) but not end-to-end (the owner's idle
   setting is minutes; no fast seam was added).
7. `dec_fctlm` benchmark row is built (vibeserver-bench links) but **was not run** (the benchmark is a whole-machine run).

## What was built (server)
- **1.1 CW beacons** — `decoderOf` gives `cw` for a `mode:"cw"` downlink with no consumer; `use` = `packet`. A pick
  attaches the listener's CW decoder through the same DecoderHost (`{"extension_name":"cw"}`). AO-7 Mode A 29.502 and
  Mode B 145.9775, RS-44 435.605, FO-29 435.795.
- **1.2 FUNcube BPSK 1200** (`fctlm`, AO-73 `fc-bpsk`): vibe_bpsk_rx.h (NCO + 96-tap decimator to 9.6 kS/s, coarse AFC
  from the squared signal's FFT line ±900 Hz, 32-tap matched low-pass, Costas with the loop's frequency handed to the
  first NCO past 20 Hz so a Doppler ramp never meets a clamp, Gardner on linear interpolation; NEON / SSE2 / scalar dot
  products, bit-identical), vibe_fctlm.h (the 80×65 de-interleaver and 65-bit sync, four sync forms, both coder orders,
  vibe_lrpt_fec.h's Viterbi + RS(255,223) + PN with the 95-byte shortening — a frame is trusted only when both RS words
  decode AND the padding is still zero). A FUNcube pick tunes **3800 Hz low in USB** (`listenOffsetHz`, beta92 — it
  was 1200 Hz to beta91) in a **100–7600 Hz** passband, and attaches fctlm with `"centre":3800,"afc_range":2600`, so a
  signal ±2.5 kHz off the prediction is found (Stuart's AO-73 pass 11-23 sat +1.3 kHz: 0 frames at the old 1200 Hz /
  ±900 Hz / 2.7 kHz). The lock's nominal carries the same offset. **Drift follow (beta92):** once frames decode, the
  decoder's offset is handed to the lock's trim (`Engine::setDecoderTrim`, slewed at 50 Hz/s, clamped ±2.5 kHz) in
  discrete moves — only when the previous move has arrived, the estimate has been steady for 3 s and is > 120 Hz off
  (`vibefc::TrimFollow`) — so the channel follows the real signal and the two loops never chase each other. A nudge
  inside the lock's reach keeps the whole pick; a tune away or a mode change ends the pick too (`"pickGone":true`).
  Manual fallback on attach: `centre` (300–7000), `afc`, `afc_range` (100–3000), `invert`. Off the audio thread (the host's decode thread). Owner switch `fctlm` (setup page + ServerModeScreen),
  reach entry 145.935 MHz, hwinfo cap, gallery run "FUNcube telemetry", benchmark row `dec_fctlm`.
- **1.3 Values** — FUNcube-1 real-time telemetry (all 58 fields of the as-flown document; the document publishes NO
  calibration, so every analogue channel is `raw:true`; counts / flags / sequence exact; mode Safe / Eclipse /
  Sunlight; fitter-message text). **SONATE-2** — the mission's protocol sheet (CCSDS transfer frame in AX.25 from
  DP0SNX, CRC-16, SCID 0x270; APID 100 STD HK, 212 fields with the mission's own polynomials and enumerations,
  generated from the sheet). **APRS `T#`** heard through a satellite → `aprs-t` (analogue raw, bits exact).
- **1.4** `tx.band` ("U/v"), `tx.modeName`, `tx.decoder` in sat_list; `lo` / `hi` / `band` / `modeName` on `picked`.
  **The passband rule**: on a transponder any tune within lo − |shift| − 3 kHz … hi + |shift| + 3 kHz re-bases the lock
  (nominal clamped to the passband); single channels keep the 15 kHz rule. **Icom by reach** (Decided 1): the Icom
  refusal in `vsSatReachWhy` and in the pick's decoder attach is gone; on an Icom the 25 Hz tracker steps the radio's
  own VFO to nominal + shift (10 Hz steps, ≤ 4 CI-V tunes a second, receive only); the pick's tune carries
  `"modeSet":1` so an Icom applies its mode; a decoder there goes through `icomDecStart` (the radio's mode/filter plan).
  `source.pickPrivate` (= not a shared dial) for REMIND.
- **1.5 Catalogue** (vibe_sat_engine.h AND directory/src/sats-curated.json; fixtures rebuilt through the mirror code):
  AO-7 Mode A / Mode B names + the Mode B CW beacon 145.9775 (drift noted); ISS 437.550 "SSTV events" (SSTV attaches;
  145.800 kept); SONATE-2 SSTV 145.880 (Martin M1, SSTV attaches) + "Germany passes only" on its 9k6; **TO-108 (CAS-6)
  NORAD 44881** U/v 145.915–145.935 (★ CHECK before release: CelesTrak's amateur group lists 44881 as "TIANYAN 01";
  I believe that is the satellite carrying the CAS-6 payload, but did not confirm it against AMSAT / SatNOGS); AO-73 `fc-bpsk`, MO-122 `fox-bpsk` (heard, never offered the FUNcube decoder).
- **1.7** `sat_track` gains `geo` (≤ 120 [lat, lon, altKm], AOS − 10 min … LOS + 10 min), `geoT0` / `geoT1` /
  `geoStep`, `tca`.
- **1.9** APRS stations and packets carry `sat` (the pick's bird, else a callsign we are sure of — the source or a
  digipeater with the H bit); the gallery's APRS line says "via ISS (RS0ISS)". DP0SNX (SONATE-2) added to
  vibe_sat_calls.h from the mission's own page.
- **Unattended passes** (Decided 3): `vibe::bg::SatPasses` inside the background-decoding JSON (`"satPasses"`: its own
  `on`, `minEl`, ≤ 8 downlinks by norad + label). `satPassTick` (housekeeping, before `bgTick`): nobody here, an open
  radio, not Icom / handheld / kept ADS-B / another whole-radio mode → the highest pass above minEl in progress with a
  minute left is picked (tune, the room's Doppler lock, the decoder on the shared host → its ordinary gallery copy);
  background decoding is stopped with its saved dial KEPT and `bgTick` holds off while `satPassBusy_`; at LOS + 5 s the
  decoder and lock end and background decoding's wait is marked served, so the profile it was running starts again at
  once. A listener (or a decoder socket) arriving gives the pass up first (`bgStopForListener` → `satPassGiveUp`) and
  the radio goes back where the pass found it. Its own decoder is not "somebody". Admin status `satPass`. Setup page
  block beside Background Decoding; the Android editor's own section (downlinks from the engine: backgroundCapsJson
  `satPasses`); the page's list is checked against the engine's `passKeepable` (test-sat-engine §13).
- **Idle timeout** (Decided 4): a listener with a SATELLITE pick counts as active from AOS − 2 min to LOS
  (enforceIdleListeners); hard / soft limits untouched.

## Tests (M5 Pro, synthetic; all pass)
| Suite | Result | What |
|---|---|---|
| test-sat-engine | 169/169 | §12 band labels, catalogue, the passband rule, Mode A on an HF-only radio, geo (bounded, nearest point at TCA), activeUntil; §13 the passes setting, the page's list = the engine's, txIndex, passAt |
| test-fctlm (new) | 24/24 | sync vector = Karn's printed bits; FEC round trip, σ 0.55 soft noise 20/20, swapped order found, garbage refused; **0 frames on 1 h of random soft symbols and 300 s of white noise through the whole receiver**; our transmitter: +350 Hz, 8 Hz/s ramp, 60 ppm, Eb/N0 14 dB 12/12; −500 Hz other polarity 12/12; no differential 12/12; frame-rate curve 12/12 down to Eb/N0 5 dB, 10/12 at 4 and 3 dB; SIMD = scalar (NEON, 14 399 symbols); the values + JSON |
| test-sat-cw (new) | 2/2 | a 20 WPM beacon at 3 dB with a locked pass's residual: CER 0.7 %; 30 WPM at 6 dB 0.4 %; lock off (−60 Hz/s): 23 % (printed) |
| test-aprs | 63/63 | + via a satellite (sat, "via ISS (RS0ISS)"), T# → sat_tlm, SONATE-2 STD HK with its calibrations, a bad CRC ignored |
| test-server-sat.mjs | 119/119 | through the real server: passband rule, CW / FUNcube / Fox picks, band / modeName / lo / hi, TO-108, SONATE-2 SSTV, pickPrivate, geo; **§9 an unattended ISS pass picked with nobody here (SSTV on the shared host), its decoder not counted as somebody, a listener gives it up and gets the dial back** |
| test-setup-satpass.mjs (new) | 10/10 | the setup page's real code: own switch, saved as satPasses beside a profile, Icom hidden, locked note |
| test_bg_decode_model.ts | 33/33 | the Android model: satPasses read / stored / untouched; caps list |
| directory test-sats.mjs | 52/52 | the mirror with the new catalogue |
| check-android-settings-parity | 185/185 | spOn / spMinEl ported |
| existing setup bg suites | pass | bgdecode, bg-band, bg-ism, bg-ndb, bg-release, bg-tiers unchanged |

**CPU (M5 Pro, one core):** the whole FUNcube receiver 0.035 % of a core (22 s of audio in 8 ms, test-fctlm §7) — a
Pi 2 estimate ≈ 1–2 % of a core (not measured). RAM ~ 60 kB a run (rings + one frame). The tracker's Icom stepping is
the existing 25 Hz tick. Unattended passes: one SGP4 pass lookup per picked downlink every 2 s while idle.
