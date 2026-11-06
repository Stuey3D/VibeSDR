# handheld-ui/src/wf — the app's waterfall + spectrum, ported to C++17 / GLES 3.0

This is a port of the VibeSDR app's waterfall, for `vibe-hhui` (R36S: RK3326, Mali-G31 / Panfrost, 640×480, SDL2
KMSDRM). It follows our own TS/SkSL code line for line, with the same maths and the same constants. The hard rule it keeps
is in `memory/feedback_waterfall_core_rules.md`:
- the ring is fully buffered;
- every row keeps its own geometry;
- a zoom never rescales the history, it only fans the new rows in;
- every tune step eases over a few rows;
- the output clock is fixed (sample-and-hold);
- edges with no data continue the row below.

## Files and where each came from

| C++ | Ported from | What |
|---|---|---|
| `wf_row_place.h/.cpp` | `src/utils/wfRowPlace.ts` | `rowViewOffset`, `writeRow`, `placedStartHz`, motion (`pairMotion`, `setMotion`, `motionAfterRow`, encode/decode), the fan (`fanNoteRow`, `fanAfterRow`, `fanRow`, `fanTable`, `easeView`), ease lengths (`zoomEaseRows`, `tuneEaseRows`, `cappedEaseRows`, the slope cap), `placeRow` |
| `wf_clock.h/.cpp` | `src/assets/wfClock.ts`, `src/assets/wfRowTarget.ts` | the fixed output clock (`clockArrive/SetRate/Run/Reset/Idle`), `clockRowRate`, the interaction boost and its taper (`boostRowRate/Active/Taper`, `taperRungs`), the feed-following row target (`snappedTarget`, `nominalRowRate`, `stepRowTarget`, `rowsForArrival`, …) |
| `wf_pace.h/.cpp` | `src/utils/wfPace.ts`, plus `WaterfallView.tsx` `paceTick` and `easeStep` | the display driver: `clockPace`, `rowPace`, `contPace` and the hand-overs, `shownRate`, `rowRate`, and the screen ease (`easeAt`/`easeCompose`, which give uZA/uZB) |
| `wf_combine.h/.cpp` | `src/assets/wfCombine.ts` | the frames a thinned feed passes over are combined (a linear-power mean, with peaks kept to within 6 dB) |
| `wf_signal.h/.cpp` | `src/assets/signalProcessor.ts` | dBFS → 0–255 intensity: auto-range, the 5-tap spatial smooth, the spectrum EMA, peak hold, and the clip and re-stretch |
| `wf_palette.h/.cpp`, `wf_palettes_gen.h` | `src/assets/colormapUtils.ts` (+ `colormaps.ts`), `WaterfallView` `specGradColors` | all 26 built-in LUTs. They are generated from `getColorLUT` by `test/gen_palettes.ts`, so they are the app's bytes. Custom palettes use the app's own `lutStops` / `lutPos` interpolation, and the spectrum fill's 9-stop gradient is built from them too |
| `wf_model.h/.cpp` | `WaterfallView.tsx`: `handleFrame`, `pumpClock`, `pushRow` (the resample to 1024), `traceShow`/`traceRun`, `specTweenTick`, the `jsPending` hand-over | the whole pipeline without GL |
| `wf_gl.h/.cpp` | `WaterfallView.tsx` `WF_SKSL` (lines 340–498) and the trace `<Path>`s | the shader in GLSL ES 3.00 (ring addressing, per-row motion, the zoom fan / tune join, the screen ease, unsharp, S-curve, LUT), the textures, and the spectrum fill + peak |
| `waterfall.h` | — | the one-object facade the main loop uses |

## API

```cpp
#include "wf/waterfall.h"
wf::Waterfall w;
w.init();                                      // a GLES 3.0 context must be current
w.setPalette("Jet");                           // or an id 0..25, or w.setCustomPalette(wf::paletteFromStops({0x000000, 0x00ff00, 0xffffff}))
w.setContrast(0); w.setSharpness(5);           // Display settings, the app's ranges and defaults
w.setSpeed(wf::Scroll::Default);               // Min2 / Low / Eco / Default (30) / Smooth (60) / Sharp (= NATIVE)
// each SPEC frame (u8 bins, DC first; dBFS = u8 − 256; 0 = no data):
w.pushFrame(bins, n, /*fftOrder*/ true, frameViewCentreHz, config.totalBandwidth, tSec);
// when the UI tunes or zooms (before the server's frames catch up):
w.setView(newCentreHz, newSpanHz); w.notifyInteraction(tSec);
// every display frame, waterfall FIRST (it runs the clock and the pace):
w.draw(0, 120, 640, 360, 640, 480, nowSec);
w.drawSpectrum(0, 0, 640, 120, 640, 480);      // baseline at the bottom of its rect
```

Use one monotonic clock for `tSec` and `nowSec`. `w.model()` exposes the CPU state: `dbMin()/dbMax()` for the axis,
`uniforms()`, `frames()`. The GL side uploads **one 1024-byte ring row per new row**, plus the 4 KB motion row when it
changed. There is no CPU pixel work per display frame.

**Build flag: `-ffp-contract=off`.** GCC in GNU mode, and clang on arm64, fuse `a + b*c` into an FMA by default. JS never
does. Fusing moves the odd `Math.round`/`floor` at a boundary: it made 3 ring bytes and 11 LUT bytes differ before the
flag was added. With the flag the port is bit-exact (see Tests).

## Deliberate deviations (and why)

1. **Motion planes are a separate texture.** The app packs h/g into four extra rows of its Gray_8 image. Here they live in
   a `1024×1` RGBA8 texture (hHi, hLo, gHi, gLo per slot). The 16-bit codes are the same, and so is the decode.
2. **uHeadF / uMoLo / uMoHi are rebased** by a multiple of 1024 before they go to the shader, so float32 stays exact after
   days of rows. Every difference and `mod` the shader takes is unchanged.
3. **No A/V hold** (`wfAvSync.ts`). The handheld has no audio playout delay to wait for, so the hold is 0. If one is ever
   needed, `clockArrive` already takes `holdMs`.
4. **No full-capture layer** (`wfWide.ts`, SPWB rows). The eased rows' edges continue the row below, as the app does on a
   server without SPWB. **This is visible:** during a long drag, a station near the trailing edge is copied up that edge
   as a vertical streak until the ease catches up. The app's own shader shows the same thing on the same rows
   (`picture_*_app.png`). On a VibeServer the app fills that strip from the coarse full-capture layer instead. This is
   the first thing to port next.
5. **No scale stamps** (`wfStamps.ts`) and no tap-to-tune lookup. `displayMap()` is there for when they are added.
6. **The ring is a fixed 1024 rows.** There is no `wfRingSize`/`wfRingGrow`, because 480 px never needs a deeper ring. The
   ring does not depend on the draw size, so resizing `draw()` keeps the history: it just shows more or fewer rows.
7. **The jump re-map branch is not ported.** `JUMP_KEEPS_HISTORY` is `true` in the app, so that branch never runs there
   either (a `static_assert` guards it). `motionRecompute` is ported but unreachable.
8. **The clock timer is run from the display frames.** Each due tick is run at its own time (`pumpClock(nextAt)`), the
   way the app's `setTimeout` chain does it, so the rows are the same.
9. **NATIVE (`Sharp`) skips the app's jitter buffer.** It is one row per arrival. The app runs NATIVE frames through its
   JS jitter buffer, which is not ported. The clocked modes bypass that buffer in the app too.
10. **Spectrum trace details.** The fill is not antialiased (the app's Android choice). The peak-hold line is the needle
    colour at 0.85, softened by a Gaussian band (σ 2 px) that stands in for Skia's mask blur. The needle, band plan, axis
    and labels belong to the rest of the UI.
11. **Dropped:** the covered/background guards, the watch provider, the frame-gap and diag logs, and
    `scheduleExtraRows`. Under the current rules the extras are always 0: every non-NATIVE mode is clocked, and NATIVE
    is 1 row per frame.
12. **One device pixel per ring row** (`rowPx = 1`), which is what the app does at a dpr of 1 (`devRowPx = round(dpr)`).
    `WaterfallConfig::scroll` defaults to **Default (30)**, as the brief asks. Note that the app's `SDRScreen` currently
    starts on `'sharp'` (NATIVE).

## Tests (`handheld-ui/test/`)

- `run_golden.sh [outDir]` (node ≥ 22 and a C++17 compiler; no GL).
  - `gen_golden.ts` builds synthetic scenarios as the engine would send them: u8 frames in FFT order, view changes,
    touches, and 60 Hz display ticks. It runs them through `ref_pipeline.ts`, which is the app's real TS modules composed
    the way `WaterfallView` composes them.
  - `wf_golden_test.cpp` replays them through the port and compares every processed row, every placed ring row with its
    motion texels and fan map, and every display frame's uniforms and spectrum trace. It also checks all palettes.
  - Scenarios: a 860→670 kHz drag with lagging tuner rows, ×√2 keys in ×3 then out ×3, a 700→1200 kHz jump, a 9–11 fps
    feed at Default/Smooth/Eco/Native, and the picture scenario.
- `run_render.sh [outDir]` renders the picture scenario twice:
  - with the **app's own `WF_SKSL`**, extracted from `WaterfallView.tsx` and run in CanvasKit (`render_app.ts`);
  - with the **port**, offscreen in Docker linux/arm64 on Mesa llvmpipe (`wf_render.cpp`, EGL surfaceless).
  It also builds and runs the golden test with aarch64 g++, then compares the two renders (`compare_png.ts`).

### Results (2026-11-06)

**Golden test: bit-exact.** All 8 scenarios and all palettes pass, on macOS clang and on aarch64 g++ (Docker):

- 0 ring or processed bytes differ (5.2 M bytes compared);
- the doubles match to a maximum relative error of 0;
- the spectrum trace matches to |d| = 0;
- all 47 104 palette bytes are equal.

**Row timing.** Output from the 9–11 fps feed, measured after 3 s:

| Mode | Row interval |
|---|---|
| Default | 33.33 ms ± 0.00 (30.0 rows/s) |
| Smooth | 16.67 ms ± 0.00 (60.0 rows/s) |
| Eco | 100.00 ms ± 0.00 (10.0 rows/s) |

Every row is a whole real frame (sample-and-hold), with no stalls and no bursts.

**CPU on the host.** A data frame takes 0.012 ms at 2048 bins and 0.023 ms at 4096 bins. A display frame takes 0.0012 ms.
On aarch64 in Docker: 0.023 / 0.028 / 0.0015 ms. The A35 will be several times slower, which is still negligible.

**Port vs the app's own `WF_SKSL` (CanvasKit).** Same scenario, same uniforms:

- **1024 px wide** (one texel per column): 0 / 13 / 41 / 85 px out of 614 400 differ by more than one LUT step.
- **640 px wide:**
  - 85–95 % of pixels are identical;
  - the rest sit almost entirely on every 5th column (x ≡ 2 mod 5). There, 640 px over 1024 bins puts the sample exactly on
    a texel boundary, and two float implementations break that nearest-texel tie either way;
  - the app's own GPU back-ends do the same to each other.

## Open items

- **Port the full-capture layer** (`wfWide.ts`: `layerWriteRow`/`layerFillRow` and the SPWB parse), so that eased-row
  edges show the real coarse capture instead of continuing the row below. This is deviation 4: it removes the vertical
  edge streaks during long drags.
- **Scale stamps and tap-to-tune** by a row's own scale (`wfStamps.ts`, `hzOnRow`).
- **The NATIVE jitter buffer**, and `wfAvSync` if audio latency ever matters on the handheld.
- **On-device check on the R36S.** It has not been run on the Mali-G31 / Panfrost yet; it is untested beyond llvmpipe.
  The shader is the app's: about 4 texture fetches a pixel at rest, up to ~14 inside a move. Check the frame time at
  640×360, 60 Hz.
- **Spectrum fill antialiasing**, if the stair-stepped top edge shows at 640 px.
