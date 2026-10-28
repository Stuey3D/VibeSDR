# The full-capture layer — spec for the APP (iOS/Mac JS path + Android native engine)

**Status:** server + web built on `v12-wf-wide-buffer` (2026-10-28) — SPWB **v2** (floor + peaks). **App built on
`v12-app-wf-wide` (2026-10-28)** — see "The app build" at the end; this is still its spec.
**Why:** Stuart, beta26 web, 02:00: *"the waterfall should be buffering outside the zoomed area so that the black
doesn't show at all."* His acceptance spec: *"NO SLICING when zooming or tuning, MINIMAL black gaps ONLY when absolutely
necessary due to the SDR catching up from a large move or rapid tuning, and the waterfall PRESERVED on orientation change
and resizing. A FULLY BUFFERED WATERFALL with a RING BUFFER."* And of the first attempt (v1, 1024 max-held bins): it must
look like **the same waterfall, just less sharp** — no hot blobs, the same floor, no vertical blocks, a thin carrier a thin
line. The app's zoom fan (wfRowPlace.ts `FanState`, `wf_zoom_fan.h`) has the hole the web had: rows recorded while zoomed
in are only the zoomed view, so a zoom out shows them as a narrow strip in black.

Reference implementation: `web/client/src/wfWide.ts` (pure, `scripts/test_wf_wide.ts`), its use in
`web/client/src/waterfall.ts` (`pushWide`, `_blendLayer`, `_layerGL`, `_coarseFor`, `captureWindow`, `layerFloorCheck`)
and `wfgl.ts` (`coarseAt`, `layerRow`); the encoder `android/app/src/main/cpp/vibe_wide_row.h`.

## 1. The wire (server side is done)

Add `&wide=1` to the `/ws/user-spectrum` URL **only when the server's `hwinfo` carries `"wide":2`** (the format
version) — and **teach `spec_decoder.cpp` the frame first: it faults on any magic but `SPEC`.**
Files: `src/services/VibeServerWsClient.ts` (`_specUrl` ~L1672/1691, binary handler ~L1886); Android
`cpp/spectrum/spec_socket.cpp` + `spec_decoder.cpp`.

Frame (little-endian; `vibe_wide_row.h`, docs/PROTOCOL.md 2026-10-28):

| bytes | field |
|---|---|
| 0–3 | `'S','P','W','B'` (u32 0x42575053) |
| 4 / 5 | version = **2** (refuse anything else) / flags = 0 |
| 6–13 | u64 timestamp ns |
| 14–21 | u64 centre Hz — the CAPTURE's physical centre (tuner + offset), not the view's |
| 22–25 | u32 span Hz — the capture width |
| 26–31 | u16 F (128), u16 C (1024), u16 P |
| F bytes | the FLOOR, low → high: dBFS + 256 (median of its FFT bins); 0 = no data |
| F bytes | the floor's SPREAD σ, 0.1 dB units (median − 16th percentile) |
| P × 3 | PEAKS: u8 cell delta from the previous (first from −1), u8 level dB + 256 (≥ 1), u8 shape = `off16 \| (w16−1) << 4`; `(255, 0, 0)` = advance 255 cells, no peak |

A peak's centre = `(cell + (off16 + ½)/16) / C` of the capture, half-width `w16/32/C`. Peaks are the cells whose strongest
bin stands ≥ max(6 dB, 3.5 σ) over the floor, the strongest 110 kept. Sent after every 4th SPEC frame while the view is
< 98 % of the capture; never raises the frame rate; none while the view covers the capture; not while audio is backed up.

## 2. What the app keeps (per full-capture row)

- Newest and previous: floor (F), spread (F), per-cell level (C, NaN = none) and shape (C) — `cellsOf()`. Interpolate in
  time between them by `t = (now − arrival)/gap` (gap = EMA 0.3 of arrival intervals): floor and spread linearly; a cell
  in both linearly; a cell in only one fades between it and the floor under it (`_blendLayer`). Different centre/span =
  no interpolation. Stale after 4 s. Drop all on a history clear.
- Floor offset `off`: on arrival, `detP50 − mean(layer floor over 16 points across the view)`, `detP50` = MEDIAN of the
  latest detailed row's dB. Clamp ±30 dB, EMA 0.3. (Measured on the web: within 1 dB in and outside the view.)
- dB → index: the detailed rows' own SignalProcessor mapping (brightness, auto range, the 14.97 clip) with `off` added.
  **The FLOOR's index is `floorIdx(m, σ)` = E[idx(m + σZ)]** (5-point Gauss–Hermite) — a detailed floor is a speckle of σ
  round m and through the clip that speckle is what the eye sees; the median alone draws the layer black.

## 3. Per ring row (this is what must match the fan)

Beside the fan's `xfA/xfB`, store per slot the row's **k = rowSpan/capSpan** (0 = no layer: no fresh row, or
rowSpan ≥ 0.98·capSpan) and **m = (rowLo − capLo)/capSpan**, and write the layer's row at the SAME slot in CAPTURE
space: floor indices (F), (index, shape) per cell (C). A screen pixel of the row maps `u = a·x + b` (the fan), and the
layer at `uc = k·u + m`. k changes only with the zoom, m with every tune — the fan never touches either.
GPU memory on the web: floor F × rows + peaks 2C × rows + m 4 × rows ≈ 5.6 MB for 2560 rows (app with 1024 rows:
~2.2 MB) — allocate at the first SPWB only.

## 4. Drawing (per row, at the row's exact centre)

- Inside `[0,1]` → the detailed ring as today. Else the layer at `uc` (0 → black):
  `v = floor(uc)` (linear between floor bins), then for the cells `floor(uc·C) ± 2`, each peak whose
  `|uc − centre| ≤ max(0.6 px, half-width)` (0.6 px = `0.6·k·a/width` in capture units) raises `v` to its level — a thin
  carrier stays a thin line at any zoom.
- Where the detailed row's edge is ON SCREEN (`−b/a > 0` or `(1−b)/a < 1`), cross-fade over ~12 px. Off-screen edges never
  fade.
- The soft vertical look (each display row = ½ row j + ½ row j+1): inside the detailed rows keep the one-fetch blend;
  where the layer shows, compute both rows separately (each its own m) and average.
- **JS path (`wfRowPlace.ts` FanState):** the fan regenerates the displayed ring on a zoom (`disp`); fill the texels outside
  each row's detailed extent from its layer row by the rule above (per pixel). Keep the SkSL shader as it is.
- **Android engine (`wf_zoom_fan.h`, `spectrum_engine.cpp`):** the same in C++ on the engine's thread; mirror
  `test_wf_wide.ts` in `test_spectrum_engine.cpp`.
- If the app ever draws a 2D twin instead (the web's no-WebGL fallback does: `wideRowInto` over the row's own u extended
  to `[−X, 1+X]`, X = ceil(capSpan/rowSpan)), a carrier is as wide as two twin texels magnified (~22 px at 10×) — the
  per-pixel peak rule above is what keeps it thin.

## 5. The history clear

Only clear when the new view lies outside the previous view ∪ the last fresh SPWB capture (web: `main.ts` onConfig,
`wf.captureWindow()`) — a pan across the capture at a narrow zoom must not wipe it.

## 6. Tests the app must pass (mirror the web's)

- Pure: `test_wf_wide.ts` (v2 parse incl. the escape, spread, floorIdx, dense floor/peaks, a one-cell carrier NARROW at its
  frequency, a wide peak as wide as its shape, m's 24-bit round trip).
- Zoom in AWAY from a carrier, zoom out: every row written meanwhile shows it at its true column (web 55/55; beta26 0/59),
  **1 px wide** at half brightness (GPU), the layer's floor within 1 dB of the detailed median in and outside the view,
  and the filled rows' floor as bright as the detailed rows' beside it. Zoom in ON it: rows stay detailed. Pan across the
  capture: every row has data across the screen. Portrait → landscape → portrait: history kept.
- Old server (no `"wide"`): URL unchanged, nothing allocated, picture as today.

## 7. Costs (measured on the web/server build)

- Bandwidth: ≤ 618 B every 4th frame — +14.2 % worst case of a 1024-bin listener's spectrum bytes, +4–7 % measured on a
  quiet band (10 fps: 10.65 → 11.38 KB/s). Nothing while not zoomed.
- Server: the encode (medians of 64 sampled bins per floor bin, one pass over the cells) only when a subscriber is due a
  row — 51 µs a row on an M-series core (32768 bins), ~1 ms on a Pi 2 (estimate ×15–25); 5 rows/s at 20 fps.
- App: one layer row per ring row (C + F + 4 bytes), one blend pass per row written (C cells); the per-pixel peak rule
  only where the layer shows.

## The app build (2026-10-28, `v12-app-wf-wide`)

- **Pure core, both paths:** `src/utils/wfWide.ts` (JS) and `android/app/src/main/cpp/spectrum/wf_wide.h` (native), line for
  line: parse, the time blend + floor match (§2), the LAYER RING (§3) and the fill (§4). The fill runs inside the zoom fan's
  `fanRegen` (wfRowPlace.ts / wf_zoom_fan.h) for every row it regenerates — the texels the detailed row has no data for
  (`lo[x] < 0`) take the layer at `uc = k·u + m`; the on-screen edge cross-fades over ~6 pt. The SkSL shader is
  UNCHANGED (the GPU pays nothing; the shader's own two-row blend gives the soft vertical look, each row with its own m).
- **Sparse peaks, not dense:** per slot F floor indices + ≤ 160 (cell, index, shape) + k, m — 784 B a slot, ~0.8 MB for
  1024 rows (the web's dense 2·C is 2.2 MB), allocated at the FIRST SPWB. A thin carrier lights the texels within
  max(½ texel, its half-width): one texel.
- **The wire:** `&wide=1` only for a server remembered as `"wide" ≥ 2` (`src/services/wideCapsCore.ts`, per base URL,
  AsyncStorage). hwinfo arrives AFTER the spectrum URL is chosen, so the FIRST visit to a server gets no SPWB; every later
  socket (resume, reconnect, next session) does. Server change that would fix the first visit: put `"wide":2` in the
  `/connection` preflight reply (the app already reads it there).
- **Routing:** JS socket → `VibeServerWsClient._parseBinaryFrame` recognises SPWB before the SPEC magic check →
  `services/wideRowBus.ts` → WaterfallView (JS path: wideArrive + the layer ring; Android engine: `engine.pushWide`).
  Android native socket → `SpecDecoder::kWide` → `SpectrumEngine::pushWideNative` on the socket thread (not queued for JS).
- **History clear (§5):** the app never clears its history (a jump re-maps it, a far one scrolls it off), so there is
  nothing to gate; a stale (> 4 s) or other-band picture draws nothing outside the capture it describes.
- **Tests:** `scripts/test_wf_wide_app.ts`, `scripts/test_wide_caps.ts`, the layer sections of `scripts/test_wf_row_place.ts`
  and `scripts/test_wf_shader_motion.ts` (the real shader), `cpp/spectrum/test_wf_motion.cpp`, `test_spectrum_engine.cpp`
  §4c, `test_spec_decoder.cpp`.
