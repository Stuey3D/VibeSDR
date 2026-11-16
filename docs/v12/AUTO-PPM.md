# Automatic frequency correction ("auto-PPM")

V12, 2026-11-12. Code: `android/app/src/main/cpp/vibe_afc.h` (the rules), `local_sdr_shim.cpp` (`afcMix`, `afcTick`,
`afcJson`, `afcPllHz`, the socket messages and the two HTTP endpoints), `vibe_dab_service.h` (`clockRead`, the one hook into DAB),
`vibe_r82xx_if.h` (`r82xxTuneOffsetHz`, the R820T's tuning step, taken out of the IQ for every listener).
Tests: `vibeserver/test-afc.cpp` (K: the tuning step), `vibeserver/test-pll-iq.cpp` (the step out of the IQ), `scripts/test-server-afc.mjs`.

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

## The R820T's tuning step (2026-11-15)

An RTL dongle's tune puts each DAB block a few tens of hertz from the dial, by a different amount on each block. Without
removing it, auto-PPM learned that as clock error.

**What was seen on air.** Each radio had one correction in force when it read these:

| Radio | 7D (NNDAB) | 11D | 12B | Correction |
|---|---|---|---|---|
| Pi 500, RTL-SDR Blog V4L (R820T) | 0 Hz | +17 Hz | +58 Hz | −0.28 ppm, learned on 7D |
| Sony, Nooelec SMArt v5 (R820T2) | −51 Hz | | −1 Hz | +0.58 ppm, learned on 12B |
| Lenovo, SDRplay RSP1A (fine PLL) | +1 Hz | +1 Hz | +1 Hz | −0.78 ppm |

The RSP1A also read 10C +1, 10D −4 (MER 9 dB, weak) and 11A +1. So every UK transmitter is right, the small-scale 7D
included. A crystal error scales with frequency and cannot make the RTL pattern. The cause is the tuner's PLL:

- `r82xx_set_pll` (osmocom rtl-sdr 2.0.3) sets VCO = 2·xtal·(nint + sdm/65536). That is a step of 879 Hz at the VCO:
  55 Hz at the LO on the /16 divider (Band III up to about 215 MHz) and 110 Hz on /8 above it. The LO lands up to half
  a step from where it was asked, by an amount that is different on every block.
- The RTL2832's IF register is 22-bit: 1.625 MHz lands 5.3 Hz low, the same on every block.
- **The chip adds a quarter of an SDM step on top** (`kR82xxSdmMeanLsb`). librtlsdr's arithmetic does not include it.
  - The model of the registers agrees with the Sony's own register reads on all 43 tunes it logged (to the log's 1 Hz).
  - Without the quarter step the model predicts the V4L at +28.9 and +69.5 Hz and the Sony's 7D at −59.8 Hz. That is
    about 10 Hz out on all three.
  - Fitted on the V4L's two figures (+17.2 and +58.0 Hz), the quarter step then predicts the Sony's 7D at −50.8 Hz,
    against −51 on air. That figure was not used in the fit.
  - The likely mechanism is the sigma-delta modulator's dither. R18 bit 4 is left at 0 by osmocom, which leaves the
    dither on. **The figure is measured, from two dongles on three blocks.** Re-fit it if more data disagrees.

**The fix, for every listener (beta79 — Stuart: "if it makes RTLs more accurate it's worth having").**
`vibertl::r82xxTuneOffsetHz` (`vibe_r82xx_if.h`) works out that offset for what is on the tuner, from what the server
already knows:

- the centre the hardware writer last **put on the tuner** (`g_rtlHwTunedHz`). This is the physical LO's dial, and it
  includes the 15 kHz offset tuning. At 97.65 MHz the step at the tuner's 97.665 is −3.2 Hz, while at the dial's 97.650
  it would be −6.9 Hz. `test-pll-iq` B shows the dial's figure would leave the station 3.7 Hz out.
- `int_freq`, from the IF filter written (DAB writes 2.048 MHz, which gives 1.625 MHz);
- the integer ppm in the radio (the owner's figure plus the hand-over's);
- the tuner crystal, recorded at open by `afcNoteRtlTuner`. An R820T uses 28.8 MHz. An R828D uses 16 MHz, except the
  Blog V4, which uses 28.8 MHz, as librtlsdr does.

**It is taken out of the IQ, in the one mix pass that already existed** (`afcMix`):

- The shift is `vibeafc::captureShiftHz`: the learned ppm at the radio's frequency, less the step, as **one** phase
  increment. There is still at most one mix pass, and it runs whenever that combined shift is not zero.
- The IQ producer stamps every buffer with the step for the tune it was captured on (`IqBuf::pllHz`, from
  `rtlPllStamp`). A retune therefore changes the increment at the first buffer the new tune produced.
- The NCO keeps its phase when the increment changes, so there is no phase jump. `test-pll-iq` D checks that the
  increment across the boundary is either the old one or the new one.
- **Every consumer gets the corrected stream**, because the mix runs before all of them:
  - the spectrum and the waterfall;
  - the shared and per-listener demodulators;
  - the decoders;
  - DAB;
  - the full-capture raw IQ out (`iqTapFull`) and every listener's iqout.

  A station at RF X is therefore at X everywhere. `test-pll-iq` A checks this through the real capture cleaner, the mix
  and a listener's channel on 7D, 11D, 12B, FM 97.65 (offset tune) and airband 118.7 (offset tune). On each, the station
  lands within 1 Hz of its RF with the step removed, and the full step (−45.0, −34.3, +5.8, −3.2, −34.5 Hz) out without
  it.
  - That 1 Hz check uses the model on both sides, so it proves the sign, the frame and the order of the chain. Whether
    the model matches a real dongle is shown by the on-air readings above.
- **Who gets it:**
  - every USB R82xx dongle in tuner mode, **whatever the auto-PPM switch** (`vibeafc::pllToRemoveHz`). This was
    Stuart's call: the step is the tuner's exact arithmetic, not a learned value.
    - With auto-PPM off, or suspended, nothing is learned, so the one mix pass carries the step alone and the owner's
      manual ppm is handled exactly as before.
    - Turn off and Reset therefore mean the manual ppm exactly, with the tuning step still taken out.
  - not over rtl_tcp, not in direct sampling, and not on other tuners;
  - `VIBE_PLL_IQ=0` leaves the step in the IQ, for A/B tests.
  - `test-pll-iq` H checks every combination of auto-PPM on, off or suspended with the step removed or left in: the
    station's position, and what the reading hands auto-PPM.
- **The DAB reading cannot have the step taken out twice.** There is one rule, `vibeafc::pllLeftInReading`: the step
  the tune has, less what `afcMix` removed from the IQ the reading was measured on.
  - Normally it was removed, so nothing is left to subtract.
  - With `VIBE_PLL_IQ=0`, all of it is left, so all of it is subtracted.
  - `test-pll-iq` E checks both cases.
- **Shown:**
  - hwinfo `afc.pllHz` gives the step for the tune in force, and `afc.pllRemovedHz` what is out of the IQ now;
  - `afc.pll` gives the words "PLL −45.0 Hz removed", shown after the line beside the ppm on the setup page, the web
    radio menu, the app's hardware panel and the R36S's SDR menu;
  - each block change logs one line.
- **Cost:** one NCO mix (NEON, SSE2 or scalar, in place, with no allocation per block).
  - On the M5 Pro it measures 26.3 µs per 65536-sample block (0.40 ns a sample). That is 0.08 % of a core at
    2.048 MS/s, 0.10 % at 2.4 MS/s and 0.13 % at 3.2 MS/s.
  - A Pi 2-class core was not measured. A Cortex-A7 is perhaps 15–25 times slower per sample, which would be roughly
    1.5–3 % of a core at 2.4 MS/s.
  - The pass only becomes new work on a dongle with nothing learned yet. Once auto-PPM has learned a value, the pass was
    already running.

**The DC spike.** In the DSP loop the order is: the capture cleaner (`cleanCapture`, which takes out DC and the I/Q
image on the raw capture), then `afcMix`, then everything else. That order is the same for the old ppm mix and the new
combined one, and nothing after the mix assumes the spike sits exactly at the hardware centre.

- **Placement uses the tuner's frame.** The DC-avoidance decisions (the 15 kHz offset tuning, `vfoDcOff` /
  `vfoDcZone` / `vfoDcCentre` for a free VFO) are made against the tuner's centre. The mix moves what is left of the
  spike by the shift: tens of Hz plus the ppm, nothing against a 15 kHz margin.
- **The label uses the corrected position.** The spike's red label (`dcSpikeHz`) follows the shift, rounded to 10 Hz so
  that a gliding correction does not send a `dc` message every 100 ms.
- **Checked:** `test-pll-iq` C runs an offset tune with a spike. The cleaner measures the spike first, and what is left
  after the mix sits 150 dB under the station's channel.

**What it does not cover:**

- **Only USB R82xx dongles in tuner mode.** It does not apply to rtl_tcp, because the far end's librtlsdr version and
  IF filter are not known here.
- E4000, FC0012/13 and FC2580 tuners have their own PLLs, which are not modelled, so they are left alone.
- Direct sampling is not covered.
- **The retune boundary is the writer's.** A USB transfer already in flight when the tune lands, a few milliseconds of
  IQ, is stamped with the new step. This is the same ambiguity every consumer already has at a retune.
- An IF filter or ppm write inside librtlsdr retunes without moving the centre. The stamp follows from the next buffer
  on.

With the step out of the readings, the V4L's three on-air readings give −0.512, −0.511 and −0.511 ppm, and the Sony's
two give +0.611 and +0.612. So the V4L's crystal is about −0.51 ppm. The −0.28 it learned living on 7D was 0.23 ppm of
tuning step.

**Grading DAB references by transmitter count (considered on 2026-11-15, not built).** The proposal was to trust a
multiplex with two or more confirmed TII transmitters more than a one-transmitter small-scale one. It was based on 7D
looking 0.26 ppm off on the RTLs, and the RSP1A disproved that: 7D is exact, and it was the tuner. The existing rules
already stop a disagreement larger than 1 ppm (a 1 kHz slip is 4–6 ppm), and nothing now suggests UK transmitters are
off by less. If a genuinely off small-scale transmitter is ever measured on a fine-PLL radio, the TII confirmation is the
place to start (`TiiConfirm`, `vibe_dab_tii.h`).

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

**The intermittent "it glided" failure (10.4 ppm/s), traced on 2026-11-16.** The glide check fails about one run in
three, and it is not the glide or the hand-over. A run that dumped the DAB reports caught it mid-glide:

| Report time | Offset |
|---|---|
| 147.7 s | −510 Hz |
| 148.2 s | **−1499 Hz** |
| 148.7 s | −490 Hz |

One report is a whole carrier spacing (1 kHz, 5.15 ppm) off, and the next is back. The DAB receiver's reported offset
momentarily takes the wrong integer carrier while the frequency ramps at 0.1 ppm/s.

- **Auto-PPM is not affected.** Readings are blanked while a change glides (`settleUntil`), and a 1 kHz slip would also
  fail the frame-timing agreement.
- **What it costs:** one wrong figure in the DAB pane for half a second.
- **Ruled out:** stamping the integer-ppm hand-over per buffer was tried and taken out again. It made no difference
  (it failed two runs in three), and the slip happens before the hand-over.
- **Fix belongs in DAB:** the receiver's integer-offset tracking is the DAB work's to fix.

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
  and `source`, `line`, `why`. On a USB R82xx dongle it also has `pllHz` (the tuning step of the tune in force), `pllRemovedHz` (what is out of the IQ now) and `pll` ("PLL −45.0 Hz removed").
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
  2. **Manual: a button in the time-signal decoder** on supported carriers — RWM 4996/9996/14996 kHz, WWV/WWVH (not CHU: off the air since 2026-06-22),
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
