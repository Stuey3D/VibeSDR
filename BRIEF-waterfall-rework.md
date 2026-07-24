# BRIEF — Phone waterfall rework (jitter buffer + 1px rows)

2026-07-24. The phone waterfall handles a variable/low frame rate noticeably worse than Jr (watch) and
the VibeServer web client. Diagnosed to two root causes. Do this as a FOCUSED pass with on-device
iteration per step — it's the render hot path and rushing it made things "horribly broken" once already.

## Symptoms (Stuart, on-device)
1. Interaction "speedup" jerks/stutters and pulses a brighter noise band that settles on release.
2. "Sluggish for a second then settles" every time the incoming data rate changes.
3. Portrait blurs and scrolls too fast; landscape is clear. All 3 frame rates clear in landscape,
   stretched/blurred in portrait. Portrait + landscape must scroll at the SAME rate.
4. Overall: the phone reacts per-arrival; Jr/web feel far smoother at variable rates.

## Already fixed (safe, shipped — builds 167/168 on `experimental`)
- #1: interaction no longer derives the interpolation multiplier from the live (spiking) data rate —
  boost uses the stable static multiplier; the dynamic target-rate interp is settled-path only.
- #2: frame-interval estimate SEEDS fast on a jump (alpha 0.7) instead of a slow EMA crawl — like Jr's
  setExpectedRowRate. (WaterfallView handleFrame, the avgFrameMs update.)

## Root cause A — fixed 256-row ring stretched to the view height (→ symptom 3)
`WaterfallView.tsx`: `ROWS = 256` (line ~82), shader `Lc = xy.y / uDrawH * uRows` (line ~239) maps 256
ring rows over the waterfall pixel height uDrawH. Portrait (tall uDrawH) ⇒ each row ~2 px ⇒ the r1/r2
inter-row BLEND stretches/blurs AND the scroll covers ~2 px/row (too fast). Landscape (short) ⇒ ~1 px/
row ⇒ sharp.
FIX: make **1 data row = 1 screen pixel** — set the ring depth (uRows / ROWS / the `n*ROWS` idxBuf at
~line 547 / the `uRows` uniform at ~467) to ≈ round(uDrawH * dpr), re-allocated on a size/orientation
change. Then both orientations are sharp (r1≈r2, no stretch blend) AND scroll at the SAME px/sec for
the same rows/sec. Watch the ring lifecycle: alloc, the shader uniform, and pushRow must all agree on
the depth; history is briefly lost on a reorientation realloc (fine).

## Root cause B — per-arrival render, no jitter buffer (→ symptoms 1, 2, 4)
The phone renders on each frame arrival (restarts the scrollFrac glide per arrival, dur = the interval
estimate). Any arrival jitter or rate change reaches the display directly. Jr and the web client use a
JITTER BUFFER: queue arrivals, drain on a STEADY clock, interpolate the sub-row fraction, prefill to a
small depth (latency insurance), drop back to holding when dry.
FIX: port Jr's model (`spike/WristSDR/WristSDR/WaterfallBuffer.swift`, the jitter-buffer/tickScroll
section) to the phone:
- Arrival (handleFrame): run the processor, forward to the watch, update dB labels, update the interval
  estimate (seeded), and ENQUEUE the processed frame {row, spec, peak, dbMin, dbMax}. Do NOT render.
- Steady drain (self-scheduling timer / RAF at the display rate): prefill to targetDepth≈1; advance a
  fractional accumulator by dt/interval; when it crosses a whole row, POP the next queued frame and
  render it via an extracted `renderFrame()` (pushRow + spectrum swap + peak + range). scrollFrac = the
  fractional part. If dry ⇒ hold/prefill. This decouples render cadence from arrival jitter.
- Consider UNIFYING away the boost/settled split — Jr has ONE model and that's why it feels better.
  targetDepth=1 keeps tuning latency low (Jr's own reasoning).

## Order
Do A first (contained, big visible win: sharp portrait + matched scroll rate), test, then B (the jitter
buffer, the deeper smoothness). Test each on-device. See [[transport_rate_design]], [[watch_vdsp_waterfall_lever]].
