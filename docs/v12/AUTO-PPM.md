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

The correction is a **digital shift of the whole capture**, on top of the owner's manual ppm. `Impl::afcMix` does it in
place, right after the capture cleaner and before anything reads the IQ. It uses `vibedsp::NCO::mix`, the four-lane
NEON/SSE mixer the DDC already uses, so on a Pi 2 it costs one SIMD pass.

- A signal at f lands ε·f low on a radio ε ppm fast. The shift is ε at the radio's tuned frequency, so it is exact at
  the centre and within ε·span/2 at the edges (5 Hz for 4 ppm across 2.4 MS/s).
- **It glides** at 0.1 ppm/s. The first acquisition glides as well (4 ppm takes 40 s), so nobody on a shared dial
  ever sees a jump. Only the owner's own Reset and Turn off move it at once.
- **At zero the IQ is untouched.** With nothing learned, auto-PPM off, reset or suspended, not one sample changes.
- **The radio's own ppm is never written.** The brief first put the whole ppm into the RTL's tuner, but that would:
  - retune under every listener;
  - replace the owner's manual value in the radio;
  - make Reset only as exact as the write-back.

  All-digital is the same for every driver:

  | Radio | How the correction is applied |
  |---|---|
  | RTL, rtl_tcp, RSP | The manual ppm stays in the radio as the owner set it; the learned part is digital |
  | Airspy, HackRF, USRP, HF+ | No ppm register, so the learned part is digital (the HF+'s ppb stays the owner's) |
  | SpyServer | Digital (the remote radio is not ours to touch) |
  | Icom | Not offered (no IQ) |

  The cost is the sample clock's own error, which is a few ppm of audio-rate drift that every server lives with
  already. The DC spike also moves ε·f off centre.

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
   - **The first acquisition needs agreement**: 120 s of clean readings, or two blocks that agree.
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
- **The handheld's SDR menu** has the row. The Apple Watch spike's PPM cell (WristSDR ControlMenu) does not show the
  line yet.
