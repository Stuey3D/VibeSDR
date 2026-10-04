# Decoder audit — 2026-10-04

Stuart: *"check all of our decoders for improvements as our decoders are based on UberSDR … WEFAX we did earlier is
now the gold standard, RTTY we are working on and that leaves the others."* Each decoder was compared clause by clause
against the best open references (fldigi, slowrx, QSSTV/MMSSTV, ft8_lib, WSJT-X, the NPL/PTB/NIST time-code specs,
ITU-R M.476/M.540). The audits were research only; nothing in this file has been built yet unless marked DONE.

Paths are under `android/app/src/main/cpp/` unless shown otherwise. Line numbers are as of 84dec247.

## Done

- **WEFAX:** the gold standard (d6d8d718, d82d785e, b210c30d): per-chart margin, per-station slant, clean rendering.
- **RTTY:** DONE 5ac2f1f9 + 84dec247.
  - Optimal ATC for selective fading.
  - Framing check with fast resync.
  - Three-character lock.
  - AUTO: shift, centre, baud and polarity found from the signal.
  - Measured on Stuart's DWD recording: old 828 characters with runs of garbage; new 1078, with only isolated
    letters lost.
  - Bench: `tools/rtty-bench/`. Test: `scripts/test-rtty-auto.sh`.

## Outright bugs (small, high value — do first)

| # | Decoder | Where | Bug |
|---|---|---|---|
| 1 | SSTV | `decoders/sstv_decoder.cpp:493-510` | **The Hough slant search can never find a peak.** `qMost` starts at 0, so `qmi = -60`, and the `qmi >= 0` guard never updates it. The rate is always the nominal one, so the slant has never been estimated. The bug is present since a8b7464c. |
| 2 | SSTV | `:630-646` | Slant correction is switched off on the premise that "an SDR's clock is locked". The slant comes from the **transmitting** station's sound card (100–1000 ppm). 100 ppm is about 25 px over a Martin M1 frame. |
| 3 | SSTV | `detectSync :271-272` | The sync window is centred 2 ms early (64 samples from −32, but the 12 kHz Hann window is 17), which shifts every picture by a fixed 4–10 px. Use `getWindow(-L/2, L)`. |
| 4 | SSTV | `:382-384, :409-411` | Robot/PD YUV is decoded full-range, but it is sent studio-range (BT.601, the Dayton spec). This lifts the blacks, dims the whites and desaturates the colour. |
| 5 | SSTV | `:532` | `if (xMax > 350) xMax -= 350` (copied from slowrx) gives about half a line of skip on a slightly late start, which wraps the right edge onto the left. Use `-= 700`, and treat Scottie separately. |
| 6 | Time | `decoders/time_decoder.cpp:482-488` | The WWV progress line still uses the old field map, so the minute and hour are shown in the wrong places. |
| 7 | Time | `:648-665, 684-686` | The WWV year comes from the host clock, but NIST sends it split: units at seconds 4–7, tens at 51–54. DST (seconds 2 and 55) and the leap-second warning (second 3) are hard-coded to false. |
| 8 | Time | `:336, :415` | **WWV and WWVB report a time one minute late.** They encode the minute just ending, unlike MSF/DCF77. Add a minute, with carry. |
| 9 | Time | `:364` (WWVB), `:227/:395` (MSF/DCF77) | One unreadable second shifts the rest of the minute. For DCF77, one missed dip (a 1.9 s gap) is taken as a minute start. Place each symbol by elapsed time from the minute anchor, as the WWV branch does. |
| 10 | FT8 | `decoders/ft8_decoder.cpp:24-57` | The hashed-callsign table (512 entries, never evicted) fills within hours. An ambiguous 10-bit match resolves to the wrong call. An unresolved `<...>` becomes an **empty-callsign spot**. |
| 11 | NAVTEX | `decoders/fsk_decoder.cpp:338-340` | Any 3 bad characters force a full resync, even DX-slot words that FEC would repair. That wipes the shift and phase and needs 4 new valid words. fldigi scores only the RX slot, resyncs past 5, and keeps the state. |
| 12 | NAVTEX | `:128-129` | One misread character can turn into a phasing code (0x66/0x0f) and flip the DX/RX phase for the rest of the message. Require two in a row. |
| 13 | NAVTEX | `:110` + host `:600` | The BEL control byte (FIGS 0x4B) goes through to the UI. fldigi prints `'`. |

## Bigger improvements (each needs its own measured bench, like rtty-bench)

**SSTV**
- Least-squares slant fit on per-line sync edges, then re-render in place from `storedLum` with the frequency
  averaged per pixel (both clients).
- Keep VIS detection running during decode, and restart on a new VIS.
- Abort after about 20 lines with no sync.
- Allow a no-VIS start from a periodic 1200 Hz sync. ISS pictures (PD-120/180) are lost today if the VIS fades.
- Horizontal resolution: estimate every 2 samples at 12 kHz, not 6 (`:362`), and replace the 4-tap box decimator.
- VIS bits by energy over each 30 ms bit, not by every slot being within ±50 Hz.
- Robot 72 timing (`pixelGrid :234`): check against a generated R72.
- The web client's 0x08 redraw opens a new image and banks the uncorrected copy as PREV
  (`decoders.ts:303`, `main.ts:11514`).

**FT8/FT4**
- Single pass only, about 2–4 dB short of WSJT-X Normal. Add signal subtraction and 2–3 passes, about 50 LDPC
  iterations plus a light OSD, and more candidates on fast boxes.
- Slot timing comes from the decode thread's clock at slot + 0.8 s. The first 0.3 s of every signal is lost, and a
  queue lag misaligns the slot. Timestamp audio at capture and cut slot − 0.5 s … slot + 15 s from a ring buffer.
- The SNR shown is `score*0.5 − 24`, not WSJT-X's 2500 Hz SNR.

**Time signals**
- Soft bit confidences, with evidence carried across minutes (compare against the previous decode plus one minute).
- Use the free framing checks: MSF A52–A59 = 01111110, DCF77 bit 0 = 0, the WWVB markers and its always-zero bits.
- Thresholds that survive LF lightning: today one crash raises the on-level for about 20 s.
- The decoder never sets the demod mode. MSF/DCF77/WWVB need CW/USB and WWV needs AM. Set the mode on start, or
  detect the carrier itself.

**NAVTEX**
- Real FEC: soft per-bit values, sum the DX+RX soft values, try the least-confident bit flips (fldigi's tiers), and
  print `_` for an unrecoverable character.
- Bit and phase alignment from DX==RX matches over a 14-character history, which works mid-message.
- Early/prompt/late bit sync, as fldigi replaced zero-crossings.
- AFC, and automatic inversion.
- Message cards for beginners:
  - Read ZCZC B1B2B3B4 … NNNN.
  - Name the station and the subject (navigational warning, met warning, SAR, forecast…).
  - Mark lost headers and trailers.
  - Deduplicate as M.540 receivers do.
  - Show the percentage of bad characters.

## Measuring

Every change gets a before-and-after on the same input:
- a synthetic generator with noise, fading, clock error and mistune, scored by character error rate or pixel error;
- then a real recording replayed through the same harness, as RTTY was on Stuart's DWD capture.
