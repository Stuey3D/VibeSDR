# BRIEF: VHF steps stop at 1 kHz, so some airband channels are unreachable

**Status:** not started. Next build. Reported 2026-07-30.

## The problem
`src/services/sdrTypes.ts`:
```ts
export const STEPS     = [10, 100, 500, 1000, 9000, 10000];          // <= 30 MHz
export const STEPS_VHF = [1000, 5000, 6250, 12500, 25000, 50000, 100000];  // > 30 MHz
```
**Above 30 MHz the finest step is 1 kHz.** An airband VOLMET at **128.5928 MHz** therefore cannot be
reached with the controls at all — the grid snaps to whole kHz and steps straight over it.

★ Stuart can type it on the phone as a workaround. **On the watch there is no keypad on that path,
so it is simply unreachable** — the crown can never land on it.

## ★★ There is direct precedent, in the comment above the constant
6.25 kHz was added for exactly this reason:
> *"6.25 kHz is the narrowband digital-voice raster … Without it those channels can only be reached
> by typing the frequency, which is what a user reported on GitHub."*

Same argument, one order of magnitude finer.

## The fix
Add **100 Hz** (and probably **500 Hz**) to the front of `STEPS_VHF`. `STEP_LABELS` already has
entries for both, so the formatting is free. The ladder stays ascending.

★ It is only a default: nobody is forced to tune 100 MHz of VHF in 100 Hz steps, they just gain the
option when they need it.

## ★ Open question: an 8.33 kHz airband step?
Airband in Europe is channelised at 25/3 = 8.3333 kHz, and no step in the ladder lands on that grid.
Tempting to add — but **check the arithmetic before doing so**:
- 128.5928 MHz does NOT sit on an 8.33 grid anchored at 128.000 (`592.8 / 8.3333 = 71.1`, not an
  integer), so this particular VOLMET is not an 8.33 channel and 100 Hz remains the fix for it.
- More importantly, the tuner **snaps to a grid** (`Math.round(cur / s) * s`), which for a
  non-integer step anchored at 0 Hz will not align with the real channel raster. An 8.33 step would
  need an **anchor/offset**, not just a step size. That is a bigger change than it looks.

Do the 100 Hz fix; treat 8.33 as a separate, properly-designed item.

## Watch
Jr and Buddy have their own ladders — check `spike/WristSDR` and `ios/VibeSDRWatch` so the wrist can
reach anything the phone can. (FM-DX already gained a selectable 10/50/100 kHz step on 2026-07-30;
this is the VHF/airband equivalent.)
