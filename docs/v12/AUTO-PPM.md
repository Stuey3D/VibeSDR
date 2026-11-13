# Automatic frequency correction ("auto-PPM")

V12, 2026-11-12. Code: `android/app/src/main/cpp/vibe_afc.h` (the rules), `local_sdr_shim.cpp` (`afcMix`, `afcTick`,
`afcJson`, the socket messages and the two HTTP endpoints), `vibe_dab_service.h` (`clockRead`, the one hook into DAB).
Tests: `vibeserver/test-afc.cpp`, `scripts/test-server-afc.mjs`.

## What it does

Every SDR's crystal is a little out, and it drifts as the radio warms up. A station therefore lands some hundreds of
hertz away from where it should. One tester's USRP B210 read "Frequency offset 781 Hz (4.02 ppm)" on DAB 7D, and later
lost a DAB lock as it warmed.

A DAB multiplex is a GPS-locked SFN, so its carriers are exact. The DAB receiver already measures how far off they land.
That figure is the radio's clock error. VibeServer now learns it from a clean DAB lock and corrects the whole radio for
it: every mode, every listener, the spectrum, the decoders and raw IQ out.

**The learning from DAB is on by default (Stuart).** The background "calibrate once an hour" slot is off by default
and is not built yet (see Phase 2).

## How it is applied

There are two parts. A crystal error moves two things: the **frequency** (the LO) and the **sample clock**.

- The DAB drift investigation (2026-11-12, `dabdrift`) showed that the sample clock is what costs DAB its MER, and it
  is what walked the B210's null out of the decode window:

  | Clock error | MER |
  |---|---|
  | 0 ppm | 35.6 dB |
  | 4 ppm | 33.2 dB |
  | 10 ppm | 28.1 dB |
  | 20 ppm | 22.7 dB |
  | 40 ppm | 16.9 dB |

- The frequency offset itself costs DAB nothing up to ±48 carriers.

1. **The digital shift (every radio).** `Impl::afcMix` shifts the whole capture in place, right after the capture
   cleaner and before anything reads the IQ. It uses `vibedsp::NCO::mix`, the four-lane NEON/SSE mixer the DDC already
   uses, so on a Pi 2 it costs one SIMD pass.
   - A signal at f lands ε·f low on a radio ε ppm fast. The shift is ε at the radio's tuned frequency, so it is exact
     at the centre and within ε·span/2 at the edges (5 Hz for 4 ppm across 2.4 MS/s).
   - It **glides** at 0.1 ppm/s. The first acquisition glides as well (4 ppm takes 40 s), so nobody on a shared dial
     sees a jump.
   - At zero, not one sample is altered.
   - **It fixes the frequency only. The sample clock stays as wrong as it was.**
2. **The hand-over to the radio's crystal correction (RTL and rtl_tcp).** `rtlsdr_set_freq_correction` feeds the
   corrected crystal to both the tuner's PLL and the RTL2832's resampler, so it fixes both. rtl_tcp's 0x05 is the same
   call at the far end.
   - Once a learned value has glided in and been confirmed, its **whole ppm** is written to the radio as
     `manual + round(learned)`.
   - At the same moment the digital shift steps down by exactly that amount (`g_afcJump`), so the dial does not move.
     Only the sub-ppm remainder stays digital.
   - It moves only on the first hand-over, or when the remainder would pass 1.5 ppm.
   - It happens only while the radio is in DAB, which gains most and rides a retune, or with nobody listening.
     Otherwise it waits for the next start, where it is written before anybody listens.
   - Reset and Turn off write the owner's manual figure back exactly; it is an integer.

| Radio | Frequency | Sample clock |
|---|---|---|
| RTL (USB) | Whole ppm in the crystal correction, remainder digital | Corrected (whole ppm) |
| rtl_tcp | The same, through 0x05 | Corrected (whole ppm) |
| SDRplay RSP | Digital | **Not corrected.** The API's `ppm` retunes, and whether it moves the sample clock is not documented, so it is not guessed at. Candidate for phase 2 after a bench check |
| Airspy R2/Mini, HackRF, USRP | Digital | **Not corrected** (no crystal register). The DAB DDC's resampler could take it in phase 2 |
| Airspy HF+ | Digital (the owner's ppb stays the owner's) | Not corrected (it rarely meets DAB: 260 MHz top) |
| SpyServer | Digital | Not corrected (the remote radio is not ours) |
| Icom | Not offered (no IQ) | |

On the digital-only radios the residual sample-clock error is what it was before auto-PPM, and DAB's re-centre fix
(`advanceAfterPush`) now copes with the walk it causes. Correcting it through a resampler already in the path (the DAB
DDC's) is phase 2.

## The safety rules (Stuart, 2026-11-12) and how each is met

> "We need to tread carefully though as this has the potential to fuck up a users radio in subtle ways and a user might
> not be able to diagnose it."

1. **Plausibility caps** (`capFor`). The cap is on the radio's total error (manual + learned), per driver:

   | Driver | Cap | Why |
   |---|---|---|
   | RTL | 100 ppm | Crystal dongles are tens of ppm out |
   | HackRF | 30 ppm | 20 ppm crystal |
   | RSP, Airspy, HF+, USRP | 10 ppm | TCXO radios, 0.5–2 ppm |
   | Anything else | 100 ppm | |

   A reading beyond the cap is rejected and logged (once a minute), and never applied. After acquisition no step is
   bigger than 0.5 ppm, and steps are at least 30 s apart.
2. **The 1 kHz carrier ambiguity.** A wrong DAB integer carrier shift is exactly 1 kHz, about 5 ppm in Band III.
   - **A reading needs a clean decode at that offset** (`dabWhyNot`):
     - locked on every frame of the 2 s span;
     - FIB rate at least 0.95 (the FIC's CRC cannot pass with every carrier in the wrong bin);
     - the MSC's pre-Viterbi error rate at most 0.08;
     - MER at least 8 dB;
     - no re-acquisition in the span or in the 10 s before it;
     - at least 8 frames averaged.
   - **Two measures of the same crystal must agree.** The carrier offset, and the sample clock seen as the null's walk
     through the decode window (`clockRead().walkPpm`, a least-squares slope of `frameAt` per frame, + the whole ppm
     already in the radio). A wrong integer carrier shift moves the first by about 5 ppm and the second not at all.
     They must agree within 2 ppm, from at least 10 readings, before any change is applied. `test-afc` J shows a 1 kHz
     slip on the only block (carrier 8.2, timing 3.0) applying nothing.
   - **The first acquisition needs agreement**: 120 s of clean readings, or two blocks that agree.
   - **A reference that keeps landing away from the rest disagrees.** Two or more of its readings more than 1 ppm from
     the median, set aside or still in the window, within the window's span, stop every decision.
   - Real data (Stuart's RSP1A on the Lenovo, 2026-11-12 22:03): six blocks from 7D to 12B read +153 to +178 Hz, which
     is 0.79 ppm slow on every one, agreeing to ±0.01 even at 10.6 dB MER.
     - `test-afc` J accepts them and applies −0.79 ppm, which leaves at most 0.7 Hz on any block.
     - With a 1 kHz slip on 10D (+1169 Hz) it applies nothing.
     - A good RSP is under 1 ppm, so its 10 ppm cap is generous.
   - **References that disagree stop every decision.** A 1 kHz slip is 5.7 ppm at 174 MHz and 4.2 at 240 MHz, so it
     cannot agree with another block. `test-afc` J shows a slip on 12B against a clean 7D applying nothing in 160 s.
3. **Self-check and auto-revert.** After each change, and once the glide has finished plus 5 s, the next 5 readings
   must not be worse:
   - the residual must not have grown by more than 0.1 ppm (and be above 0.2);
   - on the same block, the MER must not have fallen by more than 2 dB.

   Worse means **revert at once** to the previous value, with a log line. Worse twice means **suspended** for that
   radio: back to the manual ppm, said everywhere, and saved, so a restart does not undo it. Only Reset lifts it.
4. **Always visible, always undoable.** The line "Auto-corrected +4.0 ppm from DAB 7D, 2 min ago" appears in:
   - the setup page, beside the ppm;
   - the admin page (WHAT THIS RECEIVER HAS LEARNED);
   - the web client's radio menu, beside PPM, plus a "Clock corrected" row in the DAB signal pane;
   - the app's hardware panel;
   - the R36S's SDR HARDWARE menu.

   Each place has **Reset** and **Turn off**, and the sentence "If tuning looks off on this radio, turn this off to
   return to your manual setting." The server log gets one line per change, for example:
   `auto-PPM: +0.00 → +4.00 ppm from DAB 7D (MER 32.6 dB, offset -776 Hz)`.
5. **Shared dial.** The correction is the radio's, under everyone, and it glides (rule 5 above).
6. **Existing servers on upgrade.** A config with no `autoPpm` key reads as on. Nothing is applied until rules 2 and 3
   are met. The manual ppm is the starting point the correction adds to. If the owner changes the manual ppm, the
   learned value is forgotten, because the radio was re-trimmed under it.

## Why the loop is stable

Each reading is turned into an **absolute** clock error using the correction that was in force when it was taken:
ε = applied − residual. A reading taken before a step and one taken after it therefore agree. The window of readings
survives every step, and the correction is set to the estimate instead of being nudged by an error, so the loop is
deadbeat.

Readings taken while a change is still gliding through the pipeline are blanked (the glide plus 5 s). So DAB's measured
offset falls towards 0 and stays there. It never fights the DAB tracker: the tracker is only read, and its residual
moves by exactly minus each step.

`test-afc` F runs the whole Controller on a simulated radio warming from 4 to 5.4 ppm, with noise, 0.6 s of latency and
the glide. Over 30 minutes:

- 23 steps, every one at most 0.5 ppm and at least 30 s apart;
- no reversals and no reverts;
- after acquisition, the residual stays under 0.18 ppm and ends at 0.035.

End to end (`test-server-afc`, the bench multiplex with a +4 ppm clock), DAB's measured offset falls from −4.00 to
0.00 ppm.

## Persistence

- One file per radio serial, written atomically (temp file, fsync, rename): `afc-<serial>.txt` in the data directory.
- On the R36S it is `handheld/radio-<serial>.afc.txt`, beside that radio's memory, and Start+Select clears it.
- The file holds:
  - the learned correction, when it was learned, and the manual ppm it was learned on top of;
  - the source;
  - the revert count and the suspension;
  - on the handheld only, the switch (the handheld has no setup page).
- Servers keep the switch in their config (`autoPpm`, per radio). A live Turn off or on persists it through the
  live-setting path.
- That path now also keeps `{"ppm":…}`. It was being dropped, so a manual ppm set from the radio menu came back at the
  next start.

## Interfaces

- **hwinfo `afc`** (and `GET /vibeserver/hardware`): `available` (the radio can reach DAB), `on`, `state`
  (`off`/`learning`/`checking`/`applied`/`suspended`), `ppm`, `inForce`, `manual`, `cap`, `at`, `now`, `reverts`,
  `source`, `line`, `why`.
- **Socket** (admin-gated, like the ppm): `{"type":"autoppm","on":bool}` and `{"type":"autoppm_reset"}`.
- **HTTP** (admin-gated): `/vibeserver/autoppm` (setup page) and `/vibeserver/admin/autoppm` (admin page). GET returns
  the state; POST `?reset=1` resets and `?on=0|1` turns it off or on.
- **Caps**: `settings.autoPpm`, true only where DAB can run. The switch is drawn only there (AGENTS.md: a control that
  only works on one radio should not be there).

## Phase 2: slots designed in, not built

- **More references.** They go through the same `vibeafc::Reference` interface (`{source, rfHz, offsetHz, confidence,
  at, kind}`):
  - **DRM** is `Kind::Corroborating`. It never acquires and is counted only when it agrees with the primaries. Its
    gate (`drmWhyNot`: whole-kHz dial, MER 12 dB, under 150 Hz) and the estimator's handling are built and tested. The
    feed from the DRM listener (`RxStatus::freqOffsetHz` at the listener's dial) is not wired yet.
  - **Time stations** (WWV/WWVH 5/10/15 MHz and similar), for HF-only radios. These are `Kind::Averaged`: the source
    averages minutes of carrier into one reading to beat ±1 Hz of ionospheric Doppler, then offers it as a primary.
  - **LTE PSS/SSS, then GSM FCCH.** Only the measured frequency error is used. Never decode, store or show cell IDs,
    network names or anything else. Owner-enabled, with a plain explanation.
  - **Never FM or RDS.** A broadcaster's carrier is not a reference, and the pilot and RDS are baseband.
- **The calibration slot.** An owner option, off by default: "correct PPM about once an hour" while the radio is idle or
  background decoding.
  - It belongs on the same housekeeping scheduler as `bgTick` and `dxTick`.
  - It borrows the idle radio, as the DX sweep does: the strongest block from the server's own DAB scan list
    (`g_dabBand`), about 2 minutes of clean lock, then the radio goes back.
  - It gives way to a listener at once.
  - The owner also gets a "calibrate now" button.
  - The hook is `afcTick`, which already learns from whatever DAB the radio is playing.
- **Stuart's decisions, 2026-11-13** (these supersede the bullets above where they differ):
  1. **Automatic** whenever a radio is on a source: **DAB** (built), **DRM** (wire the feed), **mobile phone masts**
     (GSM FCCH, LTE PSS/SSS — the frequency error only, as above; to build).
  2. **Manual: a button in the time-signal decoder** on supported carriers — RWM 4996/9996/14996 kHz, WWV/WWVH, CHU,
     BPM (and MSF/DCF77 where a radio reaches LF). A live reading ("carrier 9996.000 kHz · your radio reads it +7.8 Hz
     high (+0.78 ppm)", with a steadiness mark); **SET RADIO OFFSET** lights once steady (~30 s) with enough SNR, greyed
     with the reason before; applied through the same correction (same Reset). **Owner-only on a shared server** — it
     moves the radio for every listener. (Stuart: RWM is what he used in Fldigi — "if it lent you adjusted the PPM".)
  3. **Scheduled, in Background Decoding**: a calibration entry with a **source** the owner chooses (a DAB block, a DRM
     station, a phone mast, a time station) and an **interval** (hourly … daily), run only while the radio is idle,
     taking it for the source's measurement (DAB ~2 min, a mast ~1 min, RWM ~10 min — hence daily for RWM, so other
     background decoding is barely disturbed), then back to the background profiles. Stuart's example: an RSP1A on
     NAVTEX, NDB and 160 m digital spots, sent to 12B for a couple of minutes every hour.
  - That example works with what is built: the RSP's **frequency** is corrected (the digital shift, every radio — Stuart's
    RSP1A learned −0.78 ppm from DAB 11A on 11-13 and restores it at every start). Only its **sample clock** is not
    (table above), which at a TCXO's <1 ppm costs DAB a fraction of a dB. The scheduler is the missing piece.
  - Queued after the R36S flasher.
- **The handheld's SDR menu** has the row. The Apple Watch spike's PPM cell (WristSDR ControlMenu) does not show the
  line yet.
