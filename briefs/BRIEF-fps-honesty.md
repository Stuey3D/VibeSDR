# BRIEF — Only offer an FPS the box can actually hold

**Status:** designed, not built
**Scope:** `vibe_benchmark.h` (the measurement) · setup page FPS selector (the offer) · server config

## The problem in one line

Stuart, 2026-09-25: *"5 looks like a choice, 8 looks like a bug."*

The selector offers 5 / 10 / 20 fps on every box. The Pi 2 and the Sony TV ask for 10 and deliver
7.8, so a listener reads a fault where there is only a limit. The Pi 2 at **5** is smooth and reads
as a deliberate setting. So: **measure what the box can hold, and offer only that.**

## What "can hold" means — and it is NOT frames per second

★★★ Stuart, 2026-09-25: *"if the higher FPS triggers too high a thread usage, audio stutters, etc
then it shouldn't be offered, maybe test it alongside a heavy demodulation or decoder to really
test it."*

A frame rate the box reaches *while the audio breaks up* is not a frame rate it can hold. The test
must fail an option for **any** of three reasons:

1. **The frames do not arrive** — achieved < 90 % of requested.
2. **The audio is late** — a block delivered after its wall-clock due time. This is the stutter the
   listener hears, and it is the reason the reverted spectrum queue was a regression: it hit 10 fps
   and broke the audio (*"I'd rather have the broken 8fps and it working than this"*).
3. **A thread is saturated** — the hottest thread over ~85 % of one core, the existing `grade()`
   amber/red line. A box at the edge has nothing left for a second listener.

## Why the current benchmark cannot answer this

`runOne()` feeds the pipeline **as fast as it will take it** and scores CPU per signal-second. That
is the right way to measure *cost* and the wrong way to measure *rate*: there is no wall clock in
it, so achieved fps is meaningless and lateness is undefined.

★ The FPS row therefore needs a **real-time paced** feed: hand over one block, sleep until that
block's due time (`n_samples / fs` from the start), repeat. Then wall time and signal time are the
same thing and both "frames arrived" and "audio was late" become measurable.

★ `pipe.start(fs, 4096, 10.0, 48000, cb)` — the `10.0` is already the requested fps. Nothing new is
needed on the pipeline side; the harness is what changes.

## The scenario: the heaviest thing the box will really be asked to do

Per Stuart's note, the fps probe runs **under load**, not on a quiet chain:

- **WFM stereo with RDS, and Advanced RDS armed** (`setRdsExtWantedFlag(&rdsOn)` — the eye diagrams
  land on `vibe-demod` and are the single heaviest thing a WFM listener can switch on).
- At the **recommended sample rate** the benchmark has just chosen, because that is what the server
  will actually run at.
- One **encoded listener** alongside it if Opus is compiled in (`runListener`'s encoder), so the
  audio path carries its real cost and the lateness figure means something.
- ★ Where DAB is available, a second probe with `runDabRows`' receiver instead of WFM: DAB+ is
  measured at ≈0.6 core-s/s on Pi 3B+ silicon and is the true worst case.

Probe **20 first, then 10**, each for ~6 s after a warm-up. Stop at the first one that passes: the
answer is the highest rate that holds.

## The offer

```
"fps": { "max": 5|10|20, "achieved": 7.8, "lateAudioPct": 4.2, "hottest": "vibe-dsp", "hottestPct": 96.1 }
```

- The setup page lists only options **≤ `max`**. It does not grey the others out — an unexplained
  grey control invites the same "is it broken?" reading we are removing.
- One line of copy under the selector naming the measurement, e.g. *"This server was measured at 5
  fps; 10 could not be held without the audio breaking up."* A number with a reason is a choice.
- ★ The server **clamps** to `max` regardless of what a config asks for, and says so in its log —
  an old `vibeserver.conf` carrying `fps=20` must not resurrect the fault.
- ★ Re-run on an update (the rule for every benchmark row): a thread-split fix that earns the box
  10 fps should show up as 10 being offered, without Stuart editing anything.

## Traps

1. ★★★ **A fast feed passes every rate.** Without the paced loop this whole brief measures nothing;
   the first version of the fps check will be a test that cannot fail. See
   [[test_that_cannot_fail]].
2. ★★★ **Do not score fps from the benchmark's own CPU rows.** The 7.8 fps ceiling was NOT a CPU
   ceiling — the box was 50 % idle. It was a single-slot worker dropping every second window. A
   CPU-derived verdict would have offered 20 fps on the Pi 2.
3. ★★ **The benchmark's own paced sleep must not be the thing that is late.** Measure lateness from
   the callback's arrival against its due time, and discard any block where the harness itself
   overslept (compare against the harness's own wake-up error).
4. ★★ **A box on mains vs a throttled box measure differently.** Record `health`'s throttle state
   with the result; a benchmark run while thermally capped should be marked, not trusted.
5. ★ **Never offer below 5.** Below that the waterfall stops reading as a waterfall.

## Related

- The real fix is finishing the thread split so spectrum **delivery** leaves the DSP thread
  (NETWORK > AUDIO > SPECTRUM > DECODERS — `PENDING-NEXT-RELEASE.md`). This brief is what we offer
  users **until** that lands, and the honest gate afterwards.
