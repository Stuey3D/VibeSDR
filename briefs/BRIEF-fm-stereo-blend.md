# BRIEF: stereo without the hiss — noise-adaptive stereo blend

**Status:** not started. Post-v10. Stuart's idea, 2026-07-30: *"the ability to have stereo whilst
reducing the hiss as much as we can on weaker stereo signals to get them to sound closer to mono in
terms of noise. Basically having our cake and eating it too."*

## ★★★ It is NOT impossible, and it is not generic noise reduction
Stuart's worry was that NR on music does not really work. Correct — and irrelevant, because **the
hiss is not in the audio, it is in ONE of the two signal paths**, and we can act on that path alone.

**Why stereo hisses and mono does not.** FM demodulation produces **triangular noise**: noise power
rises with the square of frequency across the demodulated MPX.
- **L+R** lives at 0–15 kHz — the quiet end.
- **L−R** is recovered from a double-sideband subcarrier centred on **38 kHz**, i.e. it occupies
  ~23–53 kHz of the MPX, where that triangular noise is far larger.

So on a weak signal the L−R path carries roughly **20 dB more noise** than L+R. Switching a tuner to
mono does not "reduce noise" — it *discards the noisy path*. That is the whole phenomenon.

## The technique has a name: STEREO BLEND / HIGH BLEND
Classic tuners (Sony ES, Denon, Kenwood) shipped this for decades. Two levers, both applied **to
L−R only**:
1. **Attenuate L−R** as SNR falls → separation narrows smoothly toward mono, noise falls with it.
2. **Low-pass L−R** ("high blend") → keeps stereo at LOW frequencies, where the ear localises best
   and the noise is smallest, and goes mono only at HIGH frequencies, where the hiss lives.

★★ (2) is the one that sounds like magic: the image stays wide, the hiss goes, because the two live
in different parts of the spectrum.

## ★★★ We are already shaped for this — `vibedsp/pipeline.cpp`
The two paths are separate end to end and never touch until the final matrix:
```
lprBuf_   = MPX                                   // L+R
pll_.processBlock(demodBuf_, nc, lmrBuf_, ...)    // L-R from the 38 kHz subcarrier
audioLpf_->process(lprBuf_)   lmrLpf_->process(lmrBuf_)      // separate lowpass
deemph_.process(lprBuf_)      deemphR_.process(lmrBuf_)      // separate de-emphasis
resamp_->process(lprBuf_)     resampR_->process(lmrBuf_)     // separate resamplers
```
**A blend is a gain and a corner frequency on `lmrBuf_`/`lmrLpf_`.** Nothing else moves.

★ Stuart's instinct — *"in NR mode we also tune the mono signal and do subtractive noise reduction
whilst keeping stereo output"* — is the right idea and **cheaper than he feared: we already HAVE the
mono signal.** L+R *is* mono, free, in the same block. No second demodulator, no subtraction: the
matrix does it.

## What drives the blend
We already measure candidates — pick deliberately and log the choice:
- **`lockAmp_`** in the stereo PLL (pilot lock amplitude) — the most direct proxy for "is there
  enough carrier to trust L−R". ★ NOTE: this is the same variable suspected of latching NaN in
  [[rds_dies_until_restart_nan_pll]] — fix that first or the blend inherits the bug.
- Pilot deviation, RDS BER, the S-meter.

★★ **Hysteresis and slow time constants are mandatory.** A blend that tracks a fading signal
quickly will *pump* — the image breathing in and out is more objectionable than steady hiss. Slew
it like the waterfall floor, and give it a "settle" period.

## Honest limits — say these out loud
- **It is a TRADE, not free.** Separation buys noise. The information is not there on a weak
  signal. It works because the ear tolerates reduced separation far better than hiss.
- ★★★ **ONLY WHERE WE DO THE DEMODULATION.** This needs the MPX. It applies to **VibeServer and
  local USB SDR**. On Kiwi/OWRX/UberSDR/FM-DX we receive *already-decoded audio* and the damage is
  done — no client-side processing can unmix it. Do not promise it for those backends.
- De-emphasis ordering changes the character (blend before vs after `deemphR_`). Try both.

## Suggested shape
A single **STEREO BLEND** control near the existing stereo toggle: `Off / Auto / Always mono`, with
Auto the interesting one. If a strength control is wanted, make it the live-meter-and-ball pattern
([[design_live_bar_draggable_needle]]) rather than a number.

★ For FM-DXers specifically this is a genuinely differentiating feature: they listen to weak stereo
signals by definition, and every other client makes them choose mono or hiss.

Related: [[wfm_stereo_neon_optimisation]], [[rds_pira_calibration]], [[rds_signal_limited_not_decoder]].
