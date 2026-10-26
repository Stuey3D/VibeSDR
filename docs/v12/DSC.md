# DSC (V12): maritime Digital Selective Calling, MF/HF and VHF channel 70

Branch `v12-dsc` (worktree `~/VibeSDR-dsc`), 2026-10-26. Engine: `android/app/src/main/cpp/decoders/dsc_decoder.{h,cpp}`.
MMSI → country: `vibe_maritime_mid.h`. Server: `vibe_dsc_run.h` (one receiver inside a decoder host, ext `dsc`),
`vibe_decoder_host.h`, background decoding (`vibe_bgdecode_config.h` rows `dsc` / `dsc-vhf`), the gallery
(`vibe_dsc_gallery.h`), setup (`vibe_setup_page.h`: the block switch, the two decoders, **+ DSC watch**). Web:
`web/client/src/dsc.ts`, `decoders.ts`, `main.ts`, `index.html`. App: the gallery's DSC filter only.

Built from **ITU-R M.493** (the signal, the 10-bit code, time diversity, phasing, the message formats, the ECC), **M.541**
(operation), **M.821** (expansion messages) and **M.585** (MMSI numbering). The MID table was typed in from the ITU's
published list of Maritime Identification Digits (292 MIDs); a MID it lacks is shown as "MID 217", never guessed. No
other decoder's source was copied or read for this.

## What a listener sees

DECODERS → **DSC** (offered where vibeserver.json says `"dsc": true`; the owner can switch it off). Channel keys for
2187.5 · 4207.5 · 6312 · 8414.5 · 12577 · 16804.5 kHz and Ch 70 — only those the radio reaches; a key tunes USB 1.7 kHz
below the frequency (or NFM for Ch 70). A dial a little off still decodes: the client works out the audio centre from the
nearest DSC frequency. Calls arrive newest first as the server's readable line:

    08:27:28z  2187.5 kHz   DISTRESS · MMSI 235012345 (United Kingdom) · Sinking · 50°12'N 004°10'W · 14:32 UTC · then J3E telephony
    SAFETY TEST · to coast station 005030001 (Australia) · from 212384000 (Cyprus) · acknowledgement requested
    SAFETY · ships in the area 55°N 002°W, 6° south × 10° east · from coast station 002050480 (Belgium) · J3E telephony · 2761.0 kHz

Distress red, urgency orange, safety amber. A transmission's repeats (a distress alert is sent five times) are one row,
×5. A call that sent a position has **MAP** (this server's own map, GPU or Leaflet, every file from the server).
Every format is read: distress alert, acknowledgement, relay (all ships, individual, geographic area), urgency/safety/
routine to all ships, a geographic area, a group or one station, test calls and their acknowledgements, position
requests/replies, unable to comply (with its reason), semi-automatic calls with a telephone number, and M.821 expansions
(enhanced position, speed, course, persons on board).

## The receiver (what it adds to the standard)

- **Both tone senses searched.** The phasing search runs on the soft bits and their inverse, so USB, LSB and mirrored
  receivers all work. Real calls settled which is which: **binary 1 is the lower tone** (1615 Hz on USB) — every one of the
  calls on 8414.5 kHz came in that way. VHF: 1300 Hz = 1.
- **Soft, both copies.** Each symbol is the best of the 128 valid words on the sum of its DX and RX soft bits; the format
  specifier on all four copies; the EOS on up to four (its DX repeats after the ECC). Fine timing tracked (early/late).
- **The check character corrects** — but only when the soft evidence says so: one changed symbol (≤ 0.04 of a symbol's
  energy overruled, ≤ 6 positions in doubt) or two (≤ 0.06, ≤ 4), the changed symbol must be the runner-up where it
  stands, and the next valid alternative must cost clearly more. One changed symbol can *always* satisfy an XOR, so the
  check alone proves nothing; these limits come from the bench (below).
- **Not shown:** a call that cannot be put right; one that passes with symbols in doubt (on the real recordings these were
  wrong as often as right — "from 477165800 to 477165800"); a station calling itself. Counted, not shown. The sender
  repeats.
- **Strict reading.** MMSI fields must end in 0, positions and times must be possible, "message 2" is exactly two
  frequency fields or a position — so a call cut short by a misread EOS is not taken for a shorter one.
- Measured and rejected: weighting a copy that is not itself a valid word at half (for bursts) — it cost 3–5 points of
  decode rate in noise, so copies are summed plainly.

## Measured

`scripts/test-dsc.sh` (bench `tools/dsc-bench`, 20 draws × 16 formats a point, ±30 ppm clock, random phase; SNR in 3 kHz):

| condition | decoded | wrong calls shown |
|---|---|---|
| MF/HF −6 dB / −8 dB / −10 dB | 100 % / 94.7 % / 23 % | 0 |
| MF/HF Rayleigh 0.5 Hz, 0 dB / −3 dB / −6 dB | 93 % / 76 % / 33 % | 0 |
| VHF +6 dB / +3 dB / 0 dB | 100 % / 96 % / 5 % | 0 |

0 wrong calls shown in 5,440 trials. **Noise:** 30 min each MF/HF and VHF → 0 calls (≈100 / ≈800 phasing syncs, all
rejected at the phasing or the format specifier).

**Real signals** — Stuart's UberSDR, 2026-10-26 07:32–09:32 UTC, all six HF DSC frequencies at once (12 h of audio):
**248 calls** — 2187.5: 42 · 4207.5: 7 · 6312: 18 · 8414.5: 117 · 12577: 27 · 16804.5: 37 (7 put right by the check
character). Nearly all SAFETY TEST calls to coast stations and their acknowledgements — Olympia (Greece, 002371000),
Australia (005030001), Spain (002241022), the USCG (003669991/7/8), Chile, Brazil, Peru, China, Lyngby (Denmark) —
plus Spanish coast-station safety broadcasts, a Belgian geographic-area safety call (55°N 002°W), Peruvian safety calls
on 8291 kHz and a Panamanian ship's position report to the USCG (29°15'N 094°28'W). No distress traffic in those hours.
Every MMSI's country and every coast station is the right one for the frequency and time; no external log (YaDDNet) was
compared. Three 10-minute recordings and their decodes are kept in `~/VibeSDR-test-audio/dsc/`; test-dsc replays one.
VHF channel 70 was not recorded (the UberSDR is HF only) — VHF is tested on synthetic FM through the real server.

**CPU** (Apple M5 Pro, one core, a call every ~10 s): MF/HF **0.026 %**, VHF **0.037 %** of a core. Scalar loops 0.028 /
0.040 % — the NEON/SSE2 FIR saves 4–8 %, little because the decimating FIRs are small; most of the cost is the two tone
correlators, scalar. **Pi 2: an estimate, not measured** — ~25× an M5 core on this float work → ≈ 0.7 % (MF/HF) and ≈ 1 %
(VHF) of one Cortex-A7 core per DSC VFO. RAM ≈ 40 KB per receiver (the 8 k-sample soft ring, the FIR lines, up to four
candidate calls). Compiled for armv7 NEON, i686 and x86_64 with the Android NDK; x86-64 syntax-checked.

## Multi-frequency watch — what is possible

Background decoding cuts one channel per VFO from one capture, so the question is the capture's window
(`halfWindowHz` = rate/2 − min(50 kHz, 15 %) − 3 kHz). **+ DSC watch** (setup → a profile) adds every DSC frequency inside
it around the profile's centre (choosing the centre that hears the most if none is set) and says which fall outside:

| radio, rate | window | DSC in one profile |
|---|---|---|
| Airspy HF+ 768 kS/s | ±0.33 MHz | one frequency |
| RTL-SDR (direct sampling) 2.4 MS/s | ±1.15 MHz | two neighbours: 2187.5 + 4207.5, or 4207.5 + 6312, or 6312 + 8414.5 |
| SDRplay RSP 8 MS/s | ±3.95 MHz | 4207.5 + 6312 + 8414.5 (or 2187.5 + 4207.5 + 6312) |
| SDRplay RSP 10 MS/s | ±4.95 MHz | 2187.5 + 4207.5 + 6312 + 8414.5 |
| any VHF radio | — | Ch 70 alone (156.525 MHz, NFM) |

12577 and 16804.5 always need a profile (or a radio) of their own. Profiles are by time of day, so a narrow radio can
watch 8414.5 by day and 2187.5 by night. Each DSC VFO takes one decoder slot.

## The gallery

`vibe_dsc_gallery.h`: per DSC frequency per UTC day, one log ("DSC 8414.5 2026-10-26.txt"; title "DSC 8414.5 kHz ·
26 Oct · 23 calls · 1 distress"), appended once a minute (at once for distress/urgency); and every distress or urgency call
as an item of its own, `"lv"` 3/2, pinned (`"plat"`/`"plon"`) where it sent a position — the web gallery highlights it and
offers MAP. Repeats are not written twice. Listeners' DSC decodes are kept like any other (`live`).

## Tests

- `vibeserver/test-dsc.cpp` (run-tests.sh): the code and ECC, every format's line on MF/HF and VHF, the strict reading, the
  MID table, decode rate vs SNR (both senses, fading, 0 wrong), a faded DX or RX stream, 10 min of noise each → 0 calls,
  a 12 kHz input, the real 8414.5 kHz recording word for word, the host (line, JSON, repeats), the gallery, the
  background table.
- `scripts/test-server-dsc.mjs`: the real server on a fake rtl_tcp — MF/HF on 2185.8 USB, VHF ch 70 through the real NFM
  chain, the gallery's items, background decoding on a DSC VFO.
- `scripts/test-web-dsc.mjs`: headless Edge (`--mute-audio`), desktop and 390 px — the button, channel keys, coloured
  rows, MAP's window, a channel key's tune, the gallery's DSC chip.
- `scripts/test-setup-bgdecode.mjs`: the decoders per demodulator, + DSC watch at 768 kS/s, 2.4 and 10 MS/s, Ch 70.

## Not done

- **The app's DSC box** (Mode → CLIENT DECODERS): owed — the app has the gallery filter only.
- No AFC: a station off by more than ~30 Hz loses sensitivity (the dial is the user's or the profile's).
- VHF was never heard off air here; its sense (1300 Hz = 1) is the standard's, and both senses are searched anyway.
- Expansion specifiers 101, 104, 105 are recognised but not read.
- Coast stations are named by MMSI and country, not by name (no public-domain coast-station list was typed in).
