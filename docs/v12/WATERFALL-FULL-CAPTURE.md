# The full-capture layer — spec for the APP (iOS/Mac JS path + Android native engine)

**Status:** server + web built on `v12-wf-wide-buffer` (2026-10-28). The app is NOT done — this is its spec.
**Why:** Stuart, beta26 web, 02:00: *"the waterfall should be buffering outside the zoomed area so that the black
doesn't show at all."* His acceptance spec: *"NO SLICING when zooming or tuning, MINIMAL black gaps ONLY when absolutely
necessary due to the SDR catching up from a large move or rapid tuning, and the waterfall PRESERVED on orientation change
and resizing. A FULLY BUFFERED WATERFALL with a RING BUFFER."* The app's zoom fan (wfRowPlace.ts `FanState`,
`wf_zoom_fan.h`) has the same hole the web had: rows recorded while zoomed in are only the zoomed view, so a zoom out
shows them as a narrow strip in black.

Reference implementation: `web/client/src/wfWide.ts` (pure, `scripts/test_wf_wide.ts`) and its use in
`web/client/src/waterfall.ts` (`pushWide`, `_coarseFor`, `captureWindow`) and `wfgl.ts` (`layerAt`).

## 1. The wire (server side is done)

Add `&wide=1` to the `/ws/user-spectrum` URL **only when the server's `hwinfo` carries `"wide"`** (a number, the row's
bin count, 1024 today) — older servers ignore the parameter anyway, but the app must not change its URL against a
server it has not seen advertise it, and **`spec_decoder.cpp` rejects any magic but `SPEC` as a fault**, so it must be
taught the new frame BEFORE the URL changes.
Files: `src/services/VibeServerWsClient.ts` (`_specUrl` ~L1672/1691, binary handler ~L1886) and, on Android,
`cpp/spectrum/spec_socket.cpp` + `spec_decoder.cpp` (the native socket parses SPEC itself).

Frame (docs/PROTOCOL.md, 2026-10-28), little-endian:

| bytes | field |
|---|---|
| 0–3 | `'S','P','W','B'` (u32 0x42575053) |
| 4 | version = 1 (refuse anything else) |
| 5 | flags = 0 |
| 6–13 | u64 timestamp ns |
| 14–21 | u64 centre Hz — the CAPTURE's physical centre (tuner + offset), not the view's |
| 22–25 | u32 span Hz — the capture width (the sample rate) |
| 26… | bins **low → high** (natural order — NOT SPEC's DC-at-0 rotation); dBFS = byte − 256; **0 = no data** |

Sent after every 8th SPEC frame while this listener's view is < 98 % of the capture; never raises the frame rate; the
first one rides the first zoomed frame; none while the view covers the capture. Not sent while the listener's audio is
backed up.

## 2. What the app must keep

- `wide`: the newest row (copied — parse buffers are reused), its arrival time, and the previous one (`widePrev`) for
  time interpolation; `wideGap` = smoothed interval between arrivals (EMA 0.3). A row whose centre/span differs from the
  previous one is not interpolated across (`widePrev = null`).
- Stale after **4 s** (a 5 fps listener gets one every 1.6 s): then rows get no layer.
- Floor offset `wideOff`: on each arrival, `detP20 − coarseP20` where `detP20` = 20th percentile of the latest detailed
  row's dB (all bins) and `coarseP20` = 20th percentile of the coarse dB over **at least 24 coarse bins** centred on the
  view (fewer and a deep zoom matches nothing — the layer is drawn ~10 dB hot). Clamp ±30 dB, EMA 0.3. (`percentile()`
  ignores NaN, needs ≥ 8 values.)
- On a history clear (big jump), drop `wide`, `widePrev`, the drawn row.

## 3. Per ring row (this is the part that must match the fan)

For every row written to the ring, also write its **coarse twin** at the SAME slot of a second ring, and store a third
per-slot value **X** alongside the fan's `xfA/xfB`:

- `X = wideExtent(capSpan, rowSpan)` = `ceil(capSpan / rowSpan)` (cap 4096), **0** when `rowSpan ≥ 0.98·capSpan` or no
  fresh row. X depends on the zoom only — a tune at one zoom keeps every row's X equal.
- The twin covers the detailed row's OWN u-space extended to `[−X, 1+X]`: texel `t` ↔ `u = −X + (t+½)(1+2X)/W`, i.e.
  frequency `rowLo + u·rowSpan`. `wideRowInto()`: linear between coarse bins when a texel is finer than a bin, else the
  **max of the bins whose centres it covers** (one-bin carriers survive); 0 outside the coarse row; real data stored ≥ 1.
- dB → index through the **detailed rows' own mapping** (SignalProcessor: `(db + wfBrightness − dbMin)/(dbMax − dbMin)`,
  the 14.97 clip and re-stretch) with `wideOff` added — same palette, same auto range.
- The coarse dB used is `widePrev → wide` interpolated by `t = (now − wideAt)/wideGap` (clamped 0..1) — without it the
  layer steps in blocks of rows.
- W (texels): web uses **2048** (≥ the coarse resolution at every zoom with symmetric X). That is 2048 × rows bytes.
  App budget note (32-bit 1 GB phones are real users): with the app's 1024-row ring that is 2 MiB; allocate it
  only at the first SPWB (an older server costs nothing), and drop it with the fan's display ring.

## 4. Drawing

The fan composes zoom changes into `xfA/xfB`; **X is never changed by the fan** (it is relative to the row's own u). A
screen pixel `x` of a row reads:

- `u = a·x + b` (the fan's map). Inside `[0,1]` → the detailed ring, as today.
- Coarse: `uc = (u + X)/(1 + 2X)`; inside `[0,1]` and the texel ≠ 0 → the coarse ring at `uc`.
- Detailed where it has data, coarse elsewhere, 0 (black) only where neither does. Where the detailed row's edge is ON
  SCREEN (`xl = −b/a > 0` or `xr = (1−b)/a < 1`), cross-fade over ~12 px: `mix(coarse, detail, clamp(dist/12px))`.
  Off-screen edges never fade (no lower detail at the screen's own edges).

App specifics:
- **JS path (`wfRowPlace.ts` FanState, iOS/Mac):** the fan regenerates the DISPLAYED ring from the stored one on a zoom
  (`disp`). Generate the coarse contribution into the same `disp` rows in that pass — for each displayed texel outside
  the detailed extent take the coarse ring through `uc` (same per-pixel bin ranges, worked out once per distinct
  (a, b, X)). When `disp` is null (no zoomed history on screen) the layer is invisible anyway (every row covers the view)
  — keep writing the twins so a later zoom out has them. Keep the SkSL shader unchanged (the XCover is GPU-bound).
- **Android engine (`spectrum_engine.cpp`, `wf_zoom_fan.h`):** the same, in C++, on the engine's own thread: a
  `coarse` ring + `xfX` next to the fan's maps; parse SPWB in `spec_decoder.cpp`; the regen in `wf_zoom_fan.h` reads the
  coarse ring outside each row's extent. Mirror `test_wf_wide.ts` in `test_spectrum_engine.cpp`.
- Glide (`wfRowPlace.ts glideRow`, `wf_glide.h`): streak/glide only the detailed row; the twin is placed by the row's
  own placement, so it bends with it.

## 5. The history clear

Today a new config whose window shares nothing with the previous VIEW clears the history. With the layer, the history
covers the CAPTURE: only clear when the new view lies outside the previous view ∪ the last fresh SPWB capture
(web: `main.ts` onConfig, `wf.captureWindow()`). Otherwise a pan across the capture at a narrow zoom wipes it.

## 6. Tests the app must pass (mirror the web's)

- Pure: `test_wf_wide.ts` cases (parse, extent, placement inside/outside the view, 0 only beyond the capture, one-bin
  carrier survives).
- Zoom in AWAY from a carrier for a few seconds, zoom out: every row written meanwhile shows the carrier at its true
  column (web: 57/57; beta26: 0/59). Zoom in ON it: those rows stay detailed (1 px). Pan across the capture while zoomed:
  every row has data across the screen. Rotate portrait → landscape → portrait: history kept.
- Old server (no `"wide"` in hwinfo): URL unchanged, nothing new allocated, picture as today.

## 7. Costs (measured on the web/server build)

- Bandwidth: +1.37 KB/s at 10 fps (10.65 → 12.02, +12.8 %), ~+2.6 KB/s at 20 fps — only while zoomed.
- Server: one max-hold pass over the wide FFT row per averaged frame while a subscriber is zoomed — 6.3 µs a row
  (32768 → 1024) on an M-series core; ~0.1–0.2 ms on a Pi 2 (estimate, ×15–25), ≤ 0.4 % of one core at 20 fps.
- App: the twin ring (W × rows bytes) and one resample (W texels) per row written; regen cost on a zoom ≈ the fan's
  current cost plus one coarse lookup per displayed texel outside the detailed extent.
