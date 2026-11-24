# Background decoding eases off for listeners (V12, 2026-11-24)

## What happened

Stuart, 24 November, listening on the Pi 500's SDRplay RSP1B (HF YouLoop). The radio's background-decoding profile
(`radios.0.bgDecode`) carries on while a listener is in range: nine VFOs at 6.8 MHz — four Band skimmers (80/60/40/30 m),
three DSC watches (4/6/8 MHz), RTTY weather and WEFAX stations (eight fax frequencies in one slot).

The health card said **"Background decoding: TOO HEAVY"**. His words: *"we need to throttle back the background decoding
when users are on before it gets to this stage, also does background decoding get spread across cores as right now it
says 87% of 1 core in the admin menu"* — and then: *"the sdr feels responsive enough, just the warning that came up"*.

What the server was doing (journal, `top -H`, 24 Nov 08:35–09:15):

- Background decoding gave way **0.3–0.5 s of IQ a minute**, every minute, with a listener on ("background decoding:
  0.4 s of IQ dropped in the last minute"). Beta92 made that deliberate: its threads run at nice 19 and the DSP thread
  only *tries* the background queue's lock (a chunk it cannot hand over is dropped and bridged). Band skimmers bridge the
  holes, so an FT8 slot with a gap still decodes.
- `vibehealth::BehindWatch` named any decoder that dropped input in the last minute — so the intended give-way showed as
  a fault.
- The admin "87 % of one core" is the **sum** over all background threads. Per thread: `vibe-bgdsp` (the shared FFT and
  every VFO's cut) 31–42 %, one `vibe-bgdec` up to ~19 %, everything else under 4 %. 29 `vibe-bgdec`, 12 `vibe-bgdemod`,
  16 `vibe-band*` workers. The machine was ~25 % idle (load 7–8 on 4 cores, partly a 250 kHz raw-IQ recording on another
  radio at the time). **No single background thread is near a core**, so no split was needed (item 4 of the brief: the
  hottest-thread rule is met; recheck if `vibe-bgdsp` ever passes ~80 %).

## What changed

### 1. The health verdict (the actual complaint)

`BehindWatch::note(key, label, dropped, now, excused)`: background decoding's drops are **excused** while a listener is
on and the easing still has room to act. Excused drops move the baseline and are never named. "TOO HEAVY" stays for:

- drops with **nobody** listening (the box cannot run its own profile — real overload), or
- drops above the allowance **after everything that may be eased has been** (only DSC/NAVTEX left, or nothing).

While steps are in effect the health message carries a new additive field, `notes`:
`"Background decoding: eased for listeners (3 of 9 paused)"` (or `… (extra modes off on 2 bands)`). The web pill and the
app's HealthPill draw it as a quiet line in the caption's colour — information, never the TOO HEAVY amber — and read it in
the screen-reader sentence. Older clients ignore the field.

### 2. Easing — conservative, measured, one step at a time

`vibe_bg_ease.h` (pure; `Governor` + `pickNext`), driven by `BgRunner::easeTick` from the shim's health tick (~2 s).

**It acts only when somebody is paying:**

| Signal | Threshold | Why |
|---|---|---|
| The listener's IQ queue at its fullest (new `g_iqLagPeakPermille`, USB paths) | ≥ 60 % full, twice in 10 s | 150 ms of a 250 ms queue waiting: the DSP thread is losing the race; one more stall is an audible gap. Acted on after 10 s. |
| The listener's dropped IQ buffers (`g_iqDrops`) | any, in 10 s | That *is* the harm. |
| Background decoding's own IQ drops | > 1.5 s a minute, over ≥ 30 s since the last step | The Pi 500 gives way 0.3–0.5 s/min unnoticed. 1.5 s/min (2.5 % of the capture) is where whole slots start going. |

Machine CPU % alone **never** triggers a step: the Pi 500 runs at load 7–8 and the listener was fine.

**Steps come back** last-first, one at a time, when *all* hold: 3 calm minutes since the last change and the last harm,
background drops ≤ 0.5 s/min (the proven-harmless level — the 3× gap to 1.5 is the hysteresis), the machine ≤ 85 % busy,
and twice the step's **measured** cost free (each VFO's threads' own CPU clocks over the last 20 s, recorded when the step
was taken; for a chips step the whole Band VFO's cost — conservative). A step needed again within 10 min of coming back
doubles the wait (3 → 6 → 12 → … 30 min); half an hour without a step forgives it. Between the thresholds nothing moves —
the test drives 30 minutes of a noisy 0.6–1.4 s/min signal with lone lag spikes and sees zero actions. **The listener
leaving gives every step back at once.**

**The order** (cheapest loss first, distress last):

0. **A Band skimmer's extra chips** — CW, RTTY, wide PSK (`digi::iqModes`) — on the band **furthest from the listener's
   dial** first. FT8/FT4/WSPR on that band carry on: they are the core of the PSK Reporter feed (the all-band skimmer
   goal). Done through the VFO's `bandOverride`, read at each gate period; the owner's own Always/Off per mode wins.
1. **Pictures** — WEFAX stations, WEFAX, SSTV. An idle one before one part-way through a chart.
2. **Other text decoders** — RTTY weather, amateur RTTY, SITOR-A, APRS… — the owner's lowest row first (his order is his
   priority).
3. **Whole skimmer VFOs** — Band skimmers (furthest band first), FT8, CW/PSK/RTTY skimmers, NDB.
4. **DSC and NAVTEX — last, and only for the listener's own harm.** Never to stop background decoding's own drops:
   pausing a distress watch so that the distress watch drops less input would be self-defeating. If only safety is left
   and background decoding still drops too much, the card says TOO HEAVY instead.

For the Pi 500's profile with the listener on 40 m that is: chips 80 → 30 → 60 → 40 m, WEFAX stations, RTTY weather, Band
80 → 30 → 60 → 40 m, then (listener harm only) DSC 8414.5 → 6312 → 4207.5.

Each step is one log line in plain words, e.g.
`background decoding: eased for listeners, step 1 — VFO 1 (3573.0 kHz Band skimmer (summary)): its extra modes (CW, RTTY,
wide PSK) off, FT8/FT4/WSPR carry on (it was costing 6.2 % of one core) — background decoding dropped 1.8 s of IQ a minute
(more than the 1.5 s it may give way)`, and `… eased step 1 given back … — calm for 3 min …`.

An eased VFO is not "waiting" in the box's priority registry (`bg-<id>.state`), so it never holds another radio's
lower-ranked VFOs back, and the ordinary resume path (`slotTick`) leaves it alone — only `easeTick` brings it back.
**A listener's own decoders are never touched** — the governor only reaches its own runner's VFOs.

### 3. The admin figure

`vibeadmin::BgThreadSampler` (Linux/Android: `/proc/self/task/*/stat`, nice 19 and not SCHED_IDLE) counts the background
threads and finds the busiest. The runner's status carries `threads`, `cores`, `hottestPct`, `hottestName` and `ease`. The
admin line reads:

> Background decoding · profile 1 · carrying on with a listener on · 9 of 9 VFOs running · 87 % of one core in all —
> spread over 41 threads on 4 cores (22 % of this machine) · busiest thread 33 % (vibe-bgdsp)

An older server, or a Mac, keeps the bare figure. Eased VFOs read "paused (eased for listeners)"; a Band skimmer with its
chips eased reads "CW / RTTY / PSK eased off for listeners".

### 4. Also fixed: the RSP's IQ queue had no bound

`enqueueIqInt16` (every SDRplay RSP) with no jitter buffer had **no limit at all**: a DSP thread that fell behind grew the
queue without bound (memory rule: no OOM, ever) and the listener heard it as ever-growing delay, never as a counted drop —
invisible to the health card and to the easing. It now holds a quarter of a second at the running rate (≥ 262 144
samples), oldest dropped and counted — the float path's rule, word for word.

## Trade-offs (stated, as the efficiency rule asks)

- **Cost of the governor:** one `easeTick` per 2 s — a few dozen atomic reads and a vector of per-VFO CPU counters
  (bounded: ≤ 16 samples × VFOs). The lag measure is one compare-and-swap per IQ buffer on the radio's reader thread.
  The thread sampler reads ~90 small `/proc` files, only while the admin page polls (every 2 s), never faster than 1 s.
- **Background decoding can lose a little more than before** when a listener's receiver is in trouble — that is the
  point. It loses nothing when nobody is paying.
- **Restores are slow on purpose** (3 min, doubling on a flap). A step that comes back too early and is shed again costs a
  WEFAX chart or an FT8 slot each time; waiting costs only decodes the listener's box could not afford anyway.

## Not done / for Stuart to decide

- **Cross-process easing.** Each radio's process eases its own background work for its own listener. A listener on radio
  A harmed by radio B's background decoding (no listener on B) is not eased on B. The box registry could carry a
  "listener in trouble" flag; not built.
- **The 0.3–0.5 s/min give-way itself.** With `vibe-bgdsp` at ~35 % of a core and the machine 25 % idle, those drops are
  most likely the DSP thread finding the background queue's lock held by a nice-19 worker that was pre-empted mid-pop
  (beta92's try-lock), not the worker falling half a second behind. A lock-free single-producer ring would remove them
  entirely at no cost to the listener. Not done here (it changes beta92's tested hand-over); worth doing next if he wants
  the give-way gone rather than merely excused.
- The predicted costs use what each VFO measured on this machine, not `vibe_bench_profiles.h`: the live figure is the
  same box under today's load, which the benchmark's quiet-box table cannot be.

## Tests

- `vibeserver/test-bg-ease.cpp` — the order (Pi 500 profile, listener on 40 m and 80 m, idle before mid-chart), the
  governor (the Pi 500's 0.4 s/min for 20 min → nothing; 3 s/min → one step after 30 s, the next 30 s later; restores
  after 3 calm minutes, reverse order; flap doubles the wait; 30 min of a noisy signal → no action; listener lag → a step in
  ≤ 4 s, the next 10 s later; DSC never for background's own drops, last for listener harm; restore gated on busy % and
  twice the cost free; listener leaving gives all back), BehindWatch excused drops, the `notes` JSON, and the thread
  sampler on Linux (run in the bookworm-arm64 container: 2 threads, busiest named).
- `vibeserver/test-bg-priority.cpp` — the runner's side: WEFAX → RTTY weather → the lowest-row NAVTEX, the registry, the
  resume path leaving eased VFOs alone, everything back when the listener goes.
- `scripts/test_admin_bg.ts` — the admin wording.

`VIBE_TESTS="test-bg-ease test-bg-priority" scripts/run-tests.sh`
