# Band decode: CW, RTTY and wide PSK chips (V12 beta91)

Stuart, 2026-11-22: *"is it worth adding cw skimmer and rtty skimmer to the full band skimming? so one decoder for all
digi psk cw rtty modes?"* … *"yes please, i want to get this working as a pskr beast"*. Then: the chips must work in a
listener's **live** Band too, not only background VFOs; when the box falls behind, the continuous chips **pause first**;
and PSK (PSK63 was a first for him that day) should cover the whole narrow-band digimode segment, not just 3.4 kHz above
the PSK31 calling frequency.

Branch `b91-bandskim` (off v12-experimental 3f3395890). Not merged, not pushed.

## RESUME HERE

Built and tested (synthetic signals only — see "What was tested how"). Open:

1. **A live look on a real band** — the coordinator: a Band background VFO (or a listener's Band) on 40 m / 20 m in the
   evening, check the minute line (`chips: … CW:active(0.4%) RTTY:… PSKW:…`), spots in the list / map / PSK Reporter.
2. **The Pi 2 (one-pipeline radio, the figures reference)** — on 40 m the CW window is an extra slice at 7.000–7.040, so
   the wide tap must keep 6.997–7.104 MHz instead of 7.038–7.104: at 2.048 MS/s its front end halves once fewer
   (÷16 → ÷8: 256 kS/s, a 4096-point channeliser instead of 2048 — test-band-slice). Not measured on the Pi 2. If its
   `tap dropped` grows, the owner can switch CW off for Band (`spotsband-cw`): a slice whose every chip is off is no longer
   cut (it takes effect at the next Band start or band change).
3. Stuart's Pi 500: once this is live, its 4 CW skimmer VFOs and the RTTY skimmer VFO on bands its Band VFOs cover can go
   (the Setup page now says so on each); 13 VFOs → 8, inside its 8 slots. Nothing was migrated — his call.
4. PSK125 / QPSK31 / QPSK63: **built** (branch `b91-psk2`, see "PSK125 and QPSK31 / QPSK63" below) — synthetic signals
   only. Wanted from the coordinator: a live look for real PSK125 / QPSK on air (rare; 20 m 14.070–14.073 evenings, and
   contest weekends), and that PSK Reporter shows them under PSK125 / QPSK31 / QPSK63.
5. The test-server-spots edge: **found and fixed in the PSK skimmer** (below). Re-run counts are in the section.

## ★★ The test-server-spots regression and its fix (beta91, after the first merge)

After the merge, `scripts/test-server-spots.mjs` missed the 20 m clip's PSK31 (G4PSK) or FT2 (IU8AAA) in the
**one-pipeline Band** check in about 1 run in 3 (coordinator: 3 of 7; baseline b9b0419db 5 of 5). Bisected with
temporary run-time switches, ≥4 runs each (one at a time, `nice`):

| variant | one-pipeline misses |
|---|---|
| baseline b9b0419db | 0 / 4 (and 0 / 5 the coordinator's) |
| everything as merged | 1 / 4 |
| IQ runs off, the slice-id rule, main-slice rule, warm-up forget, DC rule, pause rule, start delay off (each and all) | still 1 in 4 |
| chips switched off by the owner (no IQ extra slice, the tap back to ÷8) | 1 / 5 |
| IQ modes registered but with **no windows** (no runs, no slices) | 2 / 4 |
| IQ modes **not registered at all** | **0 / 10** |
| IQ modes out of the registry but inside the Band spotter | 1 / 6 |

So the misses came from the IQ ModeDefs being **in the audio modes' spotter (and registry)** at all, not from their
CPU, their slices or the tap. The exact path inside the spotter was not pinned down. What is known: the clip's PSK31
CQ is phase-sensitive in the offline harness for the baseline too (starting 4–8 s into the 15 s loop it is first spotted
after 45 s, not within it), so the 50 s check sits on an edge that small changes in the spotter tip.

**The fix keeps the audio path exactly as it was.** `digi::modes()` is the audio registry again, unchanged.
`digi::iqModes()` holds CW, RTTY and PSKW, and `digi::allModes()` is both, used only where the chips are listed or
sliced: the spotModes / spotBands JSON, `bandExtraSlices`, and the status chips. The chips run in a **spotter of their
own** (`DecoderHost::iqSpotter_`), fed the same slices. The audio spotter is therefore the baseline's, and its modes,
plan, gate and governor are untouched. The IQ spotter's pause rule reads the decode queue and the audio spotter's
skipped slots (`Spotter::setSkipped`). Kept from the bisect:
- `setExtras_` never publishes a transient "no extras";
- the DC rule ignores IQ-only slices;
- the IQ chips start 20 s into a Band run (`kIqStartSec`), restarted on a band step.

**Results on the final build** (c94f2fafd: the IQ chips in their own spotter with no second gate, and their threads in
the idle class), quiet Mac, load 1–2.5: test-web-spots **5/5** (70/0 each, the WSPR chip check included);
test-server-spots **4/5** (the fifth missed the one-pipeline PSK31 / FT2 again). Before the separate spotter, the misses
were ~1 run in 4. The baseline was 9/9 here and the coordinator's. **Still open:** the last ~1 in 5. The offline harness
shows the clip's PSK31 is phase-sensitive in the baseline as well (`test-band-slice real` on the clip rotated 4–8 s:
no PSK31 within 45 s; spotted by 150 s), so the 50 s check sits on an edge.

### ★★★ The cause, found (b91-psk2): a tentative carrier that has gone was HELD OFF

The PSK31 miss is the PSK skimmer's, not the spotter's. The clip's G4PSK sends one 5 s CQ (0.5–5.5 s) in every 15 s
loop. When the engine first hears that CQ part-way through, the channel it opens cannot confirm (too little text), and the
skimmer's rule for a tentative carrier was: probe 12 s, then drop it **and hold its frequency off for 20 s** — a rule for
a carrier that STAYS and will not decode (a birdie, RTTY, a data signal). A station that simply stopped was treated the
same, so its next CQ, 15 s later, began inside the 32 s of probe + hold-off and was missed as well; the one after that
is at 45 s and beyond. Measured offline (scratch `phase.cpp`: psk::Skimmer on the clip's CQ joined at each of 30 start
phases of the 15 s loop, 12 kHz audio): **11 of 30 phases first spotted after 45 s or never at 0 dB, 14 of 30 at −8 dB**
(the old engine) — exactly the 1.5–6.5 s band the offline harness had shown.

So the server path does not "dodge" anything on the baseline: whether G4PSK makes the 50 s is decided by WHEN the
audio spotter's PSK engine first hears the clip relative to its 15 s loop — when the gate floor is ready and the window
lit (4 s periods, 2 s of held lead-in), and whether the governor sheds PSK (priority 70, the lowest) for a stretch and
lets it back in mid-CQ. Anything that moves Band's early seconds by a second or two (the IQ chips' slices and threads,
the extra slice's replan, CPU from other work on the Mac) moves that moment, and about one start in five lands on the
bad stretch. The fix makes the engine indifferent to the moment instead of tuning the moment:

- **`psk_skimmer.cpp` `markHeard` / `housekeeping`: a tentative channel whose carrier has GONE is dropped at once and NOT
  held off.** Gone = this transform's windowed power over ±6 bins (≈ ±18 Hz) under +3 dB on the floor for `kGoneSec`
  (1.5 s). Not the carrier finder's 1 s average: after a strong over (+17 dB in the clip) that took ≈ 5 s to fall.
  The 12 s probe + 20 s hold-off still applies to a carrier that stays.
- Result, the same offline sweep: **0 of 30 late at 0 dB and at −8 dB**; worst first spot 34.7 s (0 dB), 35.1 s (−8 dB).
  test-psk unchanged (all its pins pass, the forced-open noise runs still 0 characters).

**Re-runs (Mac shared with other agents, one at a time, nice):** before the fix (the branch's start, cdc0e22bb's skimmer)
test-server-spots **5/6** (run 5 missed G4PSK in the one-pipeline Band — the edge reproduced); with the fix **4/4**
test-server-spots (55/0 each) and **2/2** test-web-spots (70/0 each). Each run took ≈ 11 min on the loaded Mac, so the
≥ 10 / ≥ 5 the brief asked for were NOT reached in the time-box — the coordinator's run owes the rest.

The FT2 (IU8AAA) half of the old misses is not explained by this. The test now prints, on a miss, the missing station's
mode chip as it changed (`chipHistory`: e.g. "FT2 … 21s shed → 36s light"), so the next one says whether the governor
shed it (a slot skip cutting the budget) or the decoder missed it.

## PSK125 and QPSK31 / QPSK63 (beta91, b91-psk2)

Stuart (2026-11-22): PSK125, QPSK31 and QPSK63 in the band decode's wide PSK chip AND the ordinary PSK decoder. One
change does both: they are new **lanes** in `psk::Skimmer` (`decoders/psk_skimmer.cpp`), the engine behind the listener's
PSK decoder (ONE SIGNAL and SKIMMER), Band's PSK chip (`digimodes/psk.h`) and PSKW (`digimodes/pskw.h`).

- **PSK125** = BPSK at 125 baud, Varicode as PSK31: a BPSK lane at 125 Bd (3 channel samples a symbol at ≈ 375 Hz — enough;
  CER 0.8–2.7 % at 0 / −5 dB in 2.5 kHz).
- **QPSK31 / QPSK63** (from the published PSK31 specification, G3PLX; no program's source used): the last five Varicode
  bits choose the phase SHIFT (0, +90, −90, 180); the table is the rate-½ K=5 convolutional code with generator
  polynomials 0x19 / 0x17, (a,b) = (0,0) 180, (1,0) 0, (1,1) +90, (0,1) −90 — idle (all zeros) is continuous reversals,
  as BPSK. The decoder generates its table from the polynomials; the test synth (`benchsynth/psk_synth.h qpskShift`) has
  the spec's 32 rows written out, and test-psk checks they agree. A QPSK lane: the same matched filter and timing as its
  BPSK sibling (its filter output is copied, not recomputed), a fourth-power Costas loop, EVM against the nearest of four
  points, differential detection d = s_k·s*_{k−1}, and a **16-state soft-decision Viterbi** (branch metric = d's
  correlation with the branch's shift, traceback 25 symbols). **Both sidebands**: two Viterbis run (±90 read either way);
  the one whose best path gains more speaks (QPSK on the other sideband decodes, CER 0.9 %). Cost: 32 add-compare-selects
  + a 25-step traceback a symbol, ≤ 62.5 symbols a second — microseconds; no SIMD is worth it at that rate (the
  per-sample loops it rides on — the FFT, mask, NCO, matched filter — are SIMD and unchanged).
- **Telling BPSK from QPSK, both ways** (`kindOk`): BPSK's two points are two of QPSK's four, so a BPSK signal reads clean
  on a QPSK lane too (cleaner at a weak copy: noise has four points to fall near — an EVM comparison alone picked QPSK
  for weak PSK63). But a QPSK signal can NEVER read clean on a BPSK lane: half its symbols sit on BPSK's error axis, so
  that lane's Es/N0 cannot pass ≈ 3 dB. QPSK = the BPSK sibling under 3 dB, the QPSK lane 3 dB cleaner, and ≥ 15 % of
  shifts ±90. (A four-point loop's minor-axis share was tried first and dropped: on weak BPSK the noisy loop wandered.)
- Lanes of different speeds are compared over the **same time** (32 symbols of PSK31 = 1 s; 128 of PSK125), and a lane
  may take the lead only after ≈ 1.5 s — 32 PSK125 symbols are a quarter of a second, and that lane read best on noise
  for that reason alone (weak PSK63 lost its copy to lead flips before this).
- A QPSK-led channel's frequency comes from z⁴ (squaring leaves QPSK's data on; worse, a run of +90 shifts squares to a
  line baud/2 off the carrier, and a channel moved onto it garbled "good morning" every time). A character a QPSK lane
  decoded while it was not yet the signal's reader is held as noise (its points read clean to the lenient EVM).
- Spot modes: `PSK31 PSK63 PSK125 QPSK31 QPSK63` (`digimodes/psk.h spotModeOf`); Es/N0 → 2500 Hz: −19 / −16 / −13 dB.
  PSK Reporter (`vibe_spot_report.h adifMode`): PSK125, QPSK31, QPSK63 — ADIF submodes of PSK. The signals list carries
  `"baud":125` and `"qpsk":true`; web (`decoders.ts`, `main.ts`) and app (`cwList.ts pskRowMode`, `CwView`) rows say
  PSK125 / QPSK31 / QPSK63; the map, legend and MODE filter take them as the PSK family (`digiModes.ts`).

**Tested how (synthetic only, the spec's transmitter in `psk_synth.h`)** — test-psk (all pass): the 32-row table;
CER per mode at two levels, audio and IQ, 3 draws (PSK125 0 / −5 dB: 2.5 / 2.7 % audio, 0.8 / 2.7 % IQ; QPSK31 −5 /
−8 dB: 3.5 / 4.1 %, 2.7 / 1.0 %; QPSK63 0 / −8 dB: 12.2 / 11.6 %, 7.0 / 25.5 % — QPSK63's CER is mostly the first
characters before the lane takes the lead); LSB QPSK31 0.9 %; **the mode never mistaken**: 5 modes × 3 levels (+10, −2,
−8 dB) × audio/IQ = 30 runs, 30 heard, **0 listed as another mode** (and a wider scratch sweep, 5 modes × 5 levels × 2 × 4
draws: 0 wrong); noise: the existing 10 min / forced-open pins with all five lanes on — **0 characters, 0 spots**; a
QPSK63 and a PSK125 CQ spotted as QPSK63 / BPSK125; BPSK31/63's existing CER pins unchanged.

**CPU (M5 Pro)**: busy 3 kHz (8 PSK31/63 signals, 12 kHz audio) 0.02 → 0.03 % of a core (×1.7); a 48 kHz PSKW window,
noise 0.12 % either way, busy (19 signals of the five modes, +10 dB) 0.160 → 0.220 % (×1.38) — PSKW's `costVsFt8`
3.3 → 4.5. Per tentative carrier 5 lanes instead of 2 (≈ 2.5× the per-channel work, which is small beside the shared
FFT); memory ≈ 10 KB more per tracked carrier (≤ 128 carriers: ≈ 1.3 MB at most).

### Stuart's decisions (2026-11-22, after the first report)

1. **Strong signals fixed** — one station is one channel and one spot at any level. The FLL's weight had grown with the
   signal level (at +20–30 dB it walked the channel ±2 Hz and lost the copy); PSK125's lane could lead a strong PSK31;
   Es/N0 stopped at the EVM's own 22–25 dB floor (past 14 dB the spectrum now says it); a gone signal was listed where its
   AFC wandered; a near-duplicate (within 0.6 baud of a confirmed station, or QPSK's baud/4 data line) could confirm.
   test-psk: **40/40** (five modes × 0/+10/+20/+30 dB × audio/IQ) one channel, one spot, right mode, worst 0.32 Hz, 1.9 dB.
2. QPSK63's weak start: left as it is.
3. **CPU**: Band PSK `costVsFt8` 0.6 → 1.0 (×1.7), PSKW 3.3 → 4.5. And **one budget**: the CW / RTTY / PSKW spotter had
   no budget at all (only the pause rule) — it now has the audio spotter's cap less what the slotted modes have committed
   (`Spotter::setBudgetOthers`), so a weak box sheds the chips by the governor before it is overloaded. test-digi-spots:
   a 3-unit (Pi 2-class) box with the slotted modes committing 2 — before, 2 + 2.25 committed (over); now 2 + 0.8, PSKW and
   RTTY shed. The listener's own PSK decoder is measured by the benchmark's `dec_psk` / `dec_pskskim` rows, which run the
   real engine (so its five lanes are in the figures the slots come from).

## Design

### IQ modes (their own registry and their own spotter, since the fix above)
`decoders/digi_mode.h` gains **IQ modes** (`ModeDef::iq`): Band only, no tuner, no audio. Instead of dials an IQ mode has
**windows** of RF per band (`ModeDef::windows`), and its engine (`makeWin(lo, hi)`) is fed the complex IQ of whichever
Band slice holds the window (`Engine::processIq`). `dialsOf()` is empty for an IQ mode, so every audio-mode reader (the
benchmark, Single mode, the tuners, the published-dial tests) ignores them without change.

Three are registered last in `decoders/digi_modes.cpp`:

| chip | engine | window per band | priority (shed) | pause order |
|---|---|---|---|---|
| **CW** | `digimodes/cw.h` — `cw::Skimmer` (the CW skimmer VFO's engine) | the band's CW segment | 120 (first) | 1 (first) |
| **RTTY** | `digimodes/rtty.h` — `rttysk::Skimmer` + one search worker | the RTTY / narrow-band digimode segment | 110 | 2 |
| **PSKW** | `digimodes/pskw.h` — `psk::Skimmer` (PSK31, PSK63, PSK125, QPSK31, QPSK63 — beta91) | the SAME segment as RTTY | 105 | 3 (last) |

The windows come from `vibe_skim_bands.h` (`bandWindows`) for the server's IARU region (`skimbands::bandRegion()`, kept
in step with the band plan's region by `LocalSdrShim::setBandRegion` and set from `Env::region` at Band start), plus the
region's practice segments (Region 2's US RTTY 7080–7125). A segment wider than 96 kHz is split into equal parts (6 m CW
100 kHz → 2 × 50; 10 m RTTY 120 kHz → 2 × 60). 40 m Region 1: CW 7000–7040, RTTY + PSKW 7040–7050.

The original **PSK** chip stays exactly as it was (200–3600 Hz above each PSK31 calling dial); **PSKW** spots nothing
inside those windows (`PskWideEngine::originalWindows`), so a station is never spotted twice.

### The slices
`digi::spanOf` places the IQ windows **after** the audio modes' cluster and extras, never moving them
(`addIqWindows`): a window wholly inside the main span or an existing extra rides that slice; the rest are grouped
(lowest first, up to 100 kHz) into extra slices of their own, sized exactly for their extent (bandSliceBins already makes
the width the slice's flat part). So every band's audio slices — and every test of them — are unchanged.
`kMaxExtraSlices` 3 → 8 (10 m now has five extras). Per band, Region 1:

| band | main slice carries | IQ extra slices |
|---|---|---|
| 160 m | … RTTY + PSKW (1838–1840) | CW 1807.5–1840.5 |
| 80 m | — | CW 3494–3576, RTTY/PSKW 3567–3603 |
| 60 m | — | CW + RTTY + PSKW 5347–5371 (one) |
| 40 m | … RTTY + PSKW (7040–7050) | CW 6997–7043 |
| 30 m | — | CW + RTTY + PSKW 10096–10154 |
| 20 m | — | CW + RTTY + PSKW 13992–14107 (one 115 kHz slice) |
| 15 m | — | CW 20994–21076, RTTY/PSKW 21067–21113 |
| 10 m | — | CW 27994–28076, RTTY/PSKW ×2 28065–28195 |
| 6 m | — | CW ×2 49996–50104 |

(From a scratch inspector that printed `spanOf` for every band in all three regions.)

A slice is fed only where the capture holds it (`captureHolds`); one it cannot is the chip's **out** state, as before.
An extra slice whose every chip the owner switched off is **not cut** (`bandExtraSlices(dial, modeOff)`): switching CW off
gives the DSP thread its extract back, and a one-pipeline radio its front-end decimation.

The Spotter now recognises a moved slice by the host's **slice id** (`feedIq(…, sliceId)`), not only by place and rate:
CW / RTTY slices sit beside the main one, and the old rule could have taken a neighbour at the same rate for the main slice
moved (and re-planned every block).

The DC rule (`bandDcCentre`) keeps the IQ slices clear like any other, but does **not** treat the 40–100 kHz windows as
"passbands": where a narrow capture has no room (300 kS/s) a DC inside a skimmer window is a steady carrier neither
skimmer decodes; forbidding them pushed the DC onto FT8's neighbours instead (test-band-dc, 15 m / 10 m).

### Threads (hottest-thread rule)
Each IQ engine decodes on **its own worker thread** (`digimodes/iq_worker.h`) at the decoders' LAG rung
(`vibeDecoderLagThread`, one notch below FT8's workers): Band's decode thread only copies the block into a bounded queue
(2 s; past that the block is dropped and counted — drop, never wait). RTTY has a second thread for its parameter searches.
Each engine's thread CPU is reported (`Engine::onCpu`) into the host's health figure and per chip into the minute line.
Thread priority stays Network > Audio > Spectrum > Decoders > these.

### The same spot path
An IQ engine's spot carries its RF (`Spot::rfHz`) and goes through `Spotter::wire` → `DecoderHost::emitSpot`, exactly as
FT8's: the band's one summary card (`Ft8SummaryTap`, which counts them by mode: `Spots heard: 6 (FT8 3, CW 1, PSK63 1,
RTTY 1)`), the stations store (the digi modes map), PSK Reporter (`vibe_spot_report.h`: `adifMode` CW, RTTY, PSK31,
PSK63; `cleanHeardCall`, plausible calls only, `kRepeatSec` once an hour per call + band + mode, each station once a
batch) and the `digital_spot` frame (mode CW / RTTY / PSK31 / PSK63, how CQ / DE / TEST, country). SNR: CW the
skimmer's own ≈ 47 Hz-bin figure, as the standalone CW skimmer reports it; RTTY the tones over the floor; PSK converted
to 2500 Hz as `psk.h` does. Spots only where the engine heard a callsign identifying itself **twice** (each skimmer's own
rule — `cw::identify`; RTTY also needs text that reads as text).

### Toggles
Each chip is a registered mode, so it gets everything a mode gets, from the server's JSON: `spotModes` (the Setup page's
"Band, mode by mode" Auto / Always / Off — `spotsband-cw`, `spotsband-rtty`, `spotsband-pskw`; the Android server
screen's `bandModes` node; the handheld's list — its cap raised 12 → 16), `spotBands[].modes`, and the chips in the web
and app spots box header (`digital_spots_status`). PSKW is a chip only: its spots are PSK's family, so it is kept out of
the MODE filter and the map legend (`digiModes.ts spotsListModes`). Styles: CW teal "C", RTTY olive "R", PSKW PSK's
purple "p" (Tol's muted set; `test_digi_modes` keeps every fill ≥ ΔE 10 in normal vision).

### Load: the governor, then the pause rule
- **The governor** (unchanged mechanism): each chip costs `costVsFt8` per 48 kHz of window (never under half), measured
  below. With a benchmark budget, the IQ chips are shed first (priorities 120 / 110 / 105, above every audio mode), so a
  box that cannot afford them shows them SHED — **never hidden**; the owner can set Always.
- **The pause rule** (new, `Spotter::pauseTick`, once a second of capture): CW, RTTY and PSKW are continuous, unlike the
  slotted family. If the band decode is behind — the host's queue of slice IQ not yet decoded (`setBehind` =
  `queuedIqSec()`) or any IQ engine's own backlog over **1.5 s**, or **any slotted engine skipped a slot** — the next chip
  in pause order is **paused** (CW, then 5 s later RTTY, then PSKW), its queue dropped at once. Caught up (under 0.3 s, no
  skip) for **30 s**: the last one paused resumes (PSKW, RTTY, then CW). The pause beats the owner's Always (a listener's
  audio and FT8's slots come first). The log says so plainly:
  `Band: CW paused — the CPU is behind (the decode is 3.0 s behind the capture); the slotted modes keep their slots` /
  `Band: CW resumed — the decode has caught up`. The chip shows **PAUSED** (web, app: "paused — the band decode fell
  behind the radio, so the continuous modes wait and the slotted ones keep their slots").

### Setup page
A CW / RTTY / PSK skimmer VFO on a band that a Band VFO in the **same profile** covers (its chip among the band's modes
and not switched off by the owner) gets one line: *"VFO 1 (Band, 40 m) now skims CW on this band too — this VFO can be
removed to free a decoder slot."* Nothing is changed for them; existing configs load and save back byte for byte. The
standalone skimmer VFOs and the listener's own CW / RTTY / PSK skimmers are unchanged. No licence wording anywhere.

## CPU

**Measured, M5 Pro** (scratch `chipcost.cpp`: each engine fed 30 s of IQ on the slice the server cuts, its own threads'
CPU, % of one core per second of radio; "busy" = a CW, an RTTY and a PSK signal every 2.5 kHz of window, +10 dB, all
calling CQ):

| window (slice) | CW noise / busy | RTTY noise / busy | PSKW noise / busy |
|---|---|---|---|
| 10 kHz (37.5 kHz) — 40 m R1's RTTY/PSK segment | 0.042 / 0.062 % | 0.103 / 0.158 % | 0.103 / 0.131 % |
| 30 kHz (37.5 kHz) | 0.087 / 0.076 % | 0.155 / 0.207 % | 0.083 / 0.199 % |
| **48 kHz** (75 kHz) | 0.136 / 0.118 % | 0.206 / 0.331 % | 0.205 / 0.276 % |
| **96 kHz** (150 kHz) | 0.150 / 0.240 % | 0.346 / 0.450 % | 0.363 / 0.461 % |
| one FT8 engine, 10 stations, full depth (the unit) | 0.083 % | | |

So per 48 kHz: **CW ≈ 1.6 FT8 units, RTTY ≈ 4.0, PSKW ≈ 3.3** (`costVsFt8`), never under half that for a narrow window.
Every busy window found its stations (e.g. 48 kHz: 18 / 19 / 18 of 19). Whole process, test-bg-band's 40 m run
(FT8 + CW + RTTY + PSK63 heard, 4× real time, the runner's channeliser included): 2.2 % of one M5 core.

**Estimated, Pi 2** (Cortex-A7 900 MHz, armv7 -marm, the figures reference) — the 20–30× M5 → A7 factor
DIGITAL-SPOTS.md uses for float work; **not measured on the hardware**:

| chip | 48 kHz CW | 96 kHz CW | 40 m RTTY (10 kHz) | 48 kHz RTTY | 40 m PSKW (10 kHz) | 48 kHz PSKW |
|---|---|---|---|---|---|---|
| % of one A7 core | 2.5–4 % | 4.5–7 % | 3–5 % | 6.5–10 % | 2.5–4 % | 5.5–8.5 % |

**Estimated, Pi 5-class** (Cortex-A76 2.4 GHz; a 2.5–4× factor from the M5 core — an estimate, not measured): 48 kHz CW
0.3–0.55 %, 48 kHz RTTY 0.8–1.3 %, 48 kHz PSKW 0.7–1.1 % of one core; a whole 40 m band's three chips ≈ 1–1.5 %.

Plus, on the DSP thread, one more slice extract per IQ extra slice (one small inverse FFT a block — 40 m: one 62–75 kHz
slice; 20 m: one ≈ 150 kHz slice), and on a one-pipeline radio possibly one less front-end halving (see RESUME HERE 2).

**What the benchmark defaults to.** The benchmark still measures only the audio modes' Band row (`dec_ft8band`); its
budget (`70 % ÷ (row % ÷ light units)`, divided between a profile's Band VFOs) is what the chips compete for, and they are
shed first. Rough budgets (estimates from DIGITAL-SPOTS.md's figures): a **Pi 2** ≈ 28 units — one Band VFO on 40 m
(audio modes ≈ 20 units busy + CW 1.3 + RTTY 2 + PSKW 1.7) fits; two or more Band VFOs share it, so on a busy band the
chips there show SHED (the owner can set Always, the pause rule still protects the slots). A **Pi 4 / Pi 5 / Pi 500** has
several times that: four Band VFOs each keep their chips. A **Mac / PC**: unlimited in practice. No benchmark: no budget —
everything runs and the pause rule is the guard. RAM: per chip at most 2 s of slice IQ queued (≤ 2.4 MB at 150 kHz) plus
the engine (RTTY ≤ 64 stored 10 s channels at 3 kS/s int16 ≈ 3.8 MB at most); bounded on a 256 MB board.

## What was tested how

All synthetic (no real radio, no server of Stuart's touched), built with `scripts`' recipe via the build lock, run with
`nice`. `scripts/run-tests.sh` was **not** run (the coordinator does).

- **test-bg-band 57/0** (+ the cost section skipped): a 40 m Band background VFO at 250 kS/s centred 7.000 MHz, 64 s of
  capture (32 s fed twice) with CW **G4CWX** at 7012.3 kHz (26 wpm), RTTY **G4RTY** centred 7045.0 kHz (45.45 Bd / 170 Hz)
  and **PSK63 G4PSK at 7046.3 kHz — outside the old PSK window** (7040.15–7043.65), plus FT8:
  each spotted at its RF (±60 / ±30 Hz) into the stations store with mode CW / RTTY / PSK63; PSK Reporter's datagram
  (a local sink) carries G4CWX as "CW", G4RTY as "RTTY", G4PSK as "PSK63"; **one decoder slot** in use mid-run; the
  admin line lists CW 7.0200, RTTY 7.0450, PSKW 7.0450; the summary card counts them by mode; the minute line's
  `chips:` names CW and RTTY; nothing else spotted. **Noise only: 0 CW / RTTY / PSK spots.** An FT8 crowd inside the
  RTTY/PSK window, a steady carrier and call-less keyed CW in the CW window: **0**. The owner's Off for the three: "off",
  nothing spotted. A beta90 profile (Band + CW skimmer + RTTY skimmer VFOs) parses, validates and writes back unchanged.
  **The live path**: a listener's DecoderHost in Band fed main + extra slices with ids, as the per-listener extract does —
  the `digital_spots_status` frames carry CW, RTTY and PSKW; `digital_spot` frames for CW G4CWX (7012300), RTTY G4RTY
  (7045000), PSK63 G4PSK (7046300) and FT8; with the three chips off, their slice is not asked for (2 → 1 extras) and the CW chip says "off" (the owner's), not "out".
- **test-digi-spots all passed**, with the **pause rule**: dummy IQ modes beside a slotted one — 3 s behind: CW pauses
  first, RTTY 5 s later, neither is fed, the slotted mode keeps being fed, the log line; 20 s caught up still paused, 30 s
  → RTTY resumes, another 30 s → CW; a slotted engine skipping a slot (nothing queued) pauses CW.
- **test-band-slice all passed** (slice ids passed as the host does; 40 m's tap now decimates ÷8, not ÷16 — expected,
  above), **test-band-dc 980/0**, test-spot-report, test-bg-skim (unchanged code paths).
- **test-setup-bg-skim all passed**: the hint on CW / RTTY / PSK skimmer rows beside a same-band Band VFO, nothing on
  another band or an older server, no licence wording, the profile saves back byte for byte. test-setup-bg-band,
  test-spots-band-default passed.
- **test_digi_modes all passed** (CW / RTTY / PSKW fills), **test_spots_band_app 46/0** (the web's and app's 'paused'
  state and words agree; PSKW not a filter choice). The web bundle builds (esbuild); `digiModes.ts` / `spotsBand.ts`
  type-check.
- **test-server-spots 55/0** against this branch's vibeserver binary (built via the lock) and fake-rtl-tcp: spotModes and
  20 m's modes now end CW, RTTY, PSKW; the chips arrive.
- **Updated but not run here**: `scripts/test-web-spots.mjs` (11 chips on 20 m; the legend gains CW and RTTY) — it needs
  the web page rebuilt into `vibe_web_page.h` (scripts/build-web.mjs, the coordinator's step before a release) and Edge.
- CPU: the scratch chip bench above (M5 Pro).

## beta92 — the BAND SKIMMER (Stuart, 2026-11-23, the Lenovo RSP1A)

"digi band should be called Band Skimmer as it covers cw which is analogue." Every word a person reads now says BAND
SKIMMER (BAND SKIM on the compact key in a decoder group; "Band skimmer (summary)" on the Setup page's background VFO;
"The band skimmer is not available …" in the refusals). The wire is untouched: scope `band`, `spotsband`, `spots-band`,
`ft8band`, `digital_spot` — older servers and clients still talk.

**The band_skim frame** (`vibe_decoder_host.h` `skimJsonLocked_`, on the decoder socket while Band runs; additive):

    {"type":"band_skim","data":{"windows":[{"mode":"CW","lo":7000000,"hi":7040000,"state":"active"}, …],
                                "signals":[{"mode","id","hz","snr","wpm","wpmLo","wpmHi","q","active","set","last",
                                            "call","country","settings","text"}, …]}}

- Each IQ engine (`digimodes/cw.h`, `rtty.h`) copies its skimmer's `signals()` into a `SkimRow` snapshot once a second
  of stream time, on its OWN worker thread, under its own lock (`iq_worker.h` `rowsDue` / `publishRows`) — the decode
  thread never reads decoder state. ≤ 40 rows per window, text ≤ 120 characters (the CW skimmer's own row).
- `Spotter::skim` lists each CW / RTTY window (RF edges, state) and the rows of the windows still decoding (a paused,
  shed or switched-off window's last rows are not shown as live).
- The host sends it at most once a second, only when it changed (and every 15 s regardless); ≤ 80 rows; printable ASCII,
  RTTY's line breaks as spaces. Typical frame on a test band: under 1 KB.

**The clients** (`src/services/bandSkim.ts`, one rule for web and app — `scripts/test_band_skim.ts`):
- a signal with no call yet is a row of its own: its frequency, "waiting for callsign" (never two rows for a station
  already spotted within 150 Hz in the last 10 min);
- EXPAND reads each CW / RTTY row's text in its confidence colours; EXPLAIN / RAW is in the band box's header;
- the waterfall marks each signal heard (the signal-label layer, `src: 'skim'`, by call or mode, teal CW / olive RTTY),
  while Band runs, whether or not SIGNAL LABELS is on; a tap tunes it;
- the Band VFO is drawn over the band's span widened to the CW / RTTY windows (40 m: down to 7000 kHz).
- An older server sends no frame: the list, the marks and the VFO are exactly as before.
- End to end: `scripts/test-web-band-skim.mjs` (the CW bench clip on 40 m); server: `test-bg-band` "band_skim".
