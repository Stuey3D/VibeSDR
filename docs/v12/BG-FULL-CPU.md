# Nobody listening: background decoding may use the whole machine (V12 beta94, 2026-11-25)

Stuart, 25 November: *"if nobody is using the pi then the full cpu can be allowed for background use, soon as a listener
joins and starts using then the background gets cut back as needed"*.

## What he saw

Pi 500 (4 cores), RSP1B (`vibeserver@240513CA60`), a Band profile at RF centre 6.8 MHz, nobody listening:

- `Band: CW paused — the CPU is behind (a slotted mode skipped a slot); the slotted modes keep their slots` every few
  minutes, RTTY paused too — with the machine ~47 % idle (load ~5 on 4 cores).
- `RTTY skimmer: 2 search worker(s) (the default for this hardware)`.
- A WEFAX VFO `waiting — no decoder slot free (listeners come first)`.

Every limit was sized for a box with a listener on it and stayed that size with nobody there.

## The policy (`android/app/src/main/cpp/vibe_bg_fullcpu.h`)

Driven by the shim's health tick (~2 s, `bgFullCpuTick`), pure (`fullcpu::Policy`), its result written into a
`fullcpu::Live` (atomics) that every background VFO's host reads.

**NORMAL** — anybody *streaming* from this radio (`s_streamingCached`: the listener count less held sessions — a held
place is somebody who may come back, not somebody using the CPU), or still inside the calm period, or background not
running: today's budget exactly.

**FULL** — nobody streaming for the calm period (the owner's idle grace `g_vsIdleGraceSec`, clamped 30 s … 10 min), not
hot, not on battery. Background decoding may spread over `cores` (the allowance):

| Lever | Normal | Full |
|---|---|---|
| RTTY skimmer search workers (`RttySkimRun::setWorkers`, polled every 2 s of stream) | the owner's / benchmark's figure | max(that figure, allowance), ≤ 8 |
| Decoder slots (`BgSlots`) | the box's limit, listeners first | + `allowance` slots over the limit, background only |
| Band governor cap (`Spotter::setCpuView` → `BandBudget::rescale`) | benchmark's units (one core's 70 %) | × allowance; the live budget scales with it |
| Pause rule: a skipped slot | pauses CW, then RTTY | pauses only when < 0.5 core is free (or unknown) — and is kept out of the governor's "too heavy" cut |
| Pause rule: backlog over 1.5 s | pauses at once | must last 10 s (a burst no longer sheds) |

Threads keep their rung: the RTTY search workers of a background host now take the background rung too
(`vibe-bgrttysk*`, nice 19). FULL uses more of the idle CPU; it never outranks the OS, the capture or the DSP.

### The allowance — measured, and shared on a multi-radio box

The Pi 500 runs one process per radio. Each process puts a line on the **box board** beside the decoder slots:
`<slot dir>/bgcpu-<pid>.idle`, held by an exclusive flock while the process lives, `1` when its radio is idle with
background running. Then, from the health sampler's whole-machine busy % (`/proc/stat`):

- `share = max(1, cores ÷ idle radios)` — its fair part of the machine (4 idle radios on 4 cores: 1 each);
- `spare = cores × (100 − busy %) ÷ 100`;
- the allowance starts at `min(share, max(1, ⌊spare⌋))`; at most one step per 10 s it goes **up** one while
  `spare ≥ 1.25` and under the share, **down** one (never below 1) while `spare < 0.4`. Between the two it holds.

Spare CPU is used only while it is actually spare; another radio's listener simply leaves less of it.

**Unknown load** (Android: only this process is visible) — the share is capped at half the cores; skips still count.

**Phones and the handheld** — never FULL while the health sampler says the CPU is throttled (heat, power, unknown cause)
or the temperature level is HIGH or worse, or on battery (a battery present, neither charging nor on mains). Leaving FULL
for one of those is logged.

## The load ladder: depth drops before whole modes or bands (`decoders/digi_load_ladder.h`)

Stuart, 25 November: *"what should happen in the background is if the cpu is fully loaded the depth drops before
dropping entire bands"*.

When Band decoding is **behind** — a slotted engine skipped a slot, the decode's backlog over 1.5 s (in FULL mode: a
skip only without headroom, a backlog only after 10 s), or the governor shed a slotted mode's period (its budget
exceeded) — the ladder steps **down one depth at a time, one step per 15 s** (an FT8 slot, so each step can show):

| Depth | What it turns off (existing knobs only) |
|---|---|
| 0 full | — (passes as the slot allows, OSD, a-priori, "Dig deeper" if the owner has it on) |
| 1 no Dig deeper | the deep tier (repeats, 8 AP hypotheses, FT8's looser second look); the book still learns |
| 2 no OSD or a-priori passes | `osdMaxErrors`, `osdPairSpan`, `apTry` off — BP and the second look only |
| 3 one pass a slot | `maxPasses` 1; every Band engine `loaded` (light) and the governor costs every mode light |
| 4 fewer candidates | FT8/FT4/FT2 `maxCandidates` 140 → 70 |
| 5 stricter sync (the minimum) | `minScore` 10 → 14 |

Every band and every mode keeps decoding through all six. **Only at the minimum** does the pause rule take a mode — CW,
then RTTY, then wide PSK, the slotted modes keeping their slots — and a skip is let through to the governor's "too heavy"
budget cut (which sheds lowest priority first). Last of all, for a listener, background easing may pause a whole Band
VFO (`vibe_bg_ease.h`).

**Recovery in reverse:** paused modes come back first (the pause rule's 30 s of calm each, RTTY then CW); depth comes
back only when nothing on the ladder is paused, one step per 30 s of calm.

**One ladder per host** (a listener's Band: its audio and CW/RTTY spotters share it). A background profile's Band VFOs
all share **the runner's** — depth drops on every band before any band loses a mode.

**A listener's arrival** (`vibe_bg_ease.h`): easing's first step is now tier −1, *decode depth to the minimum on every
band* (`LoadLadder::forceMin`), before any band's extra modes go off; given back like any other step.

Log lines: `Band: CPU behind (a slotted mode skipped a slot) — decode depth reduced to one pass a slot (all bands keep
decoding)` · `Band: depth at minimum — CW paused — the CPU is behind (…); the slotted modes keep their slots` ·
`Band: decode has caught up — depth back up to fewer candidates` · `background decoding: eased for listeners, step 1 —
decode depth to the minimum on every band (…)`. The admin line adds `Band decode depth reduced (one pass a slot) — every
band still decoding` while below full; the runner's JSON carries `depth` / `depthName`.

A Spotter without a ladder (the tests' own, the benchmark) keeps the beta91 pause rule exactly.

## A listener joins

NORMAL at once (no calm period that way round):

1. A decoder slot a listener needs is freed immediately, as before — `BgRegistry::makeRoomForListener` now asks for as
   many as it takes (`victims()`: lowest-ranked running first, idle before mid-picture), since background may hold more
   than the limit.
2. On the radio itself, `slotTick` gives back the slots it took over the limit, lowest VFO first, one per tick (twice a
   second), until the box is within its limit.
3. The RTTY search workers fall to the floor at the next 2 s look; the Band cap falls to the benchmark's at the next
   block (the governor sheds lowest priority first); the pause rule is today's — CW, then RTTY pause first if the
   decode falls behind, the slotted modes keep their slots.

Never: a background thread above a listener's (thread priority rule), or a listener waiting for a slot.

## The log and the admin line

- `background decoding: nobody listening — using the whole CPU (4 cores): up to 3 cores now`
- `background decoding: nobody listening — spreading to 4 cores (3.6 of 4 free, this radio's share 4)` / `… easing back to 2 cores …`
- `background decoding: a listener joined — background cut back to the normal budget (2 RTTY search workers, decoder slots shared with listeners, CW/RTTY paused first if the decode falls behind)`
- `background decoding: nobody listening, but the CPU is hot or throttled — staying at the normal budget`
- `Band: a slotted mode skipped a slot — the continuous modes carry on (nobody listening, the CPU has headroom)` (at most every 10 min)
- `background decoding: VFO 4 (…) paused — background back to the normal budget`

The runner's status JSON carries `"fullCpu":{"cores":4,"rttyWorkers":4,"extraSlots":4}` while FULL; the admin page adds
`· nobody listening — using up to 4 cores (4 RTTY search workers, 4 extra decoder slots)` to the background line
(`web/client/src/adminBg.ts bgFullCpuText`).

## Trade-offs (stated, as the efficiency rule asks)

- **CPU ↔ decodes:** FULL spends idle CPU on more search workers and fewer shed/paused modes — on a box nobody uses.
  Power draw rises on an idle Pi; phones are protected by the heat/battery rule.
- **RAM:** up to 6 more RTTY search worker threads (stacks, mostly untouched) and the decoders of the extra slots (a WEFAX
  VFO's host) — bounded by the allowance (≤ cores).
- **Cost of the policy:** one tick per 2 s: a directory read of a few small files on the slot dir, a handful of atomics.

## Unproven until it runs on the Pi

- Whether the "skipped a slot" on the Pi 500 is CPU at all: with 47 % idle the slow engine is single-threaded (or held
  off by nice 19 against the box's other radios). FULL stops it pausing CW/RTTY; it does not make that engine faster.
- The allowance's thresholds against the real load of four radios; the per-radio share when radios come and go.
- WEFAX in an extra slot alongside the others on the Pi's real profile.
- The ladder's 15 s step / 30 s calm against the Pi's real skips; how many decodes each depth step costs on a busy band
  (depth 4–5 trade weak-signal FT8 decodes for CPU — measured only as knobs in FTX-SENSITIVITY.md, not as a ladder).
- A box whose benchmark budget is small (a Pi 2) sheds slotted periods every period, so it now sits at minimum depth
  rather than full depth with modes shed — intended by the rule, unmeasured.
- Android: no whole-machine load figure, so the half-the-cores cap and the heat/battery rule are the only guards.

## Tests

- `vibeserver/test-bg-fullcpu.cpp` — the policy (calm period and its clamps, listener in/out, held ≠ streaming via the
  input, the share against fake load figures, step pacing and hysteresis, unknown load, heat, battery), the threads'
  view, the box board.
- `vibeserver/test-digi-spots.cpp` — the pause rule with a FULL view (skips with headroom pause nothing and do not cut
  the budget, a 6 s burst does not pause, 10 s does; the cap × 4) and the cut back (cap at once, CW then RTTY, the
  slotted mode keeps its slots).
- `vibeserver/test-bg-priority.cpp` — extra slots (the waiting WEFAX VFO runs), a listener on another radio freeing three
  at once, the extras given back lowest first when a listener joins, `victims()` across radios.
- `vibeserver/test-digi-spots.cpp` (beta94 load ladder) — two spotters on one ladder: behind → five depth steps with
  every mode decoding, the engine told each depth and decoding light from one pass; only at the minimum CW, then RTTY
  pause; calm → RTTY, CW back first, then depth one step per 30 s back to full; `forceMin` for a listener.
- `vibeserver/test-bg-ease.cpp` — the depth step is easing's first, before a band's extra modes.
- `scripts/test_admin_bg.ts` — the admin wording.

`VIBE_TESTS="test-bg-fullcpu test-digi-spots test-bg-priority" scripts/run-tests.sh`
