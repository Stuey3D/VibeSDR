# BRIEF — Waterfall frame alignment on the phone (place frames by their OWN centre)

**Status:** understood, not started. **Scope:** the phone app's waterfall render only — the watch
was fixed in `ee6c09dc` and is correct.
**Symptom (Stuart, 2026-07-19):** tuning quickly leaves the waterfall *beside* the signal rather
than on it, "until zoom is triggered". Seen on the **phone's own screen**, so it is not a watch bug
— Companion mode merely drives the phone hard enough to expose it.

---

## 1. What is actually wrong

The phone tracks two centres:

| | meaning |
|---|---|
| `this.view.centerHz` | the centre we ASKED the server for |
| `this.status.centerHz` | the centre the server actually SENT (`config.centerFreq`) |

While tuning fast these disagree, because frames already in flight still carry the old centre. The
renderer places every frame against the **current view**, so an in-flight frame is drawn at the
wrong offset — the signal sits next to the VFO. Zoom "fixes" it only because zoom re-asserts the
view and flushes the disagreement.

## 2. ★ Why the obvious fix is WRONG — panning

The watch fix (`trueCenterHz`, `ee6c09dc`) made the watch index bins by the frame's REAL centre, and
that commit deliberately left the phone alone:

> *"The main app's own render is untouched (keeps the predicted centre)."*

The reason is **VFO lock / waterfall panning** (`UberSDRClient.followVfo`, v5.1.0
[[vfo_lock_panning]]). With `followVfo=false`, `tune()` intentionally does NOT recentre the view so
the user can pan the waterfall away from the VFO:

> *"Unlocked continuous tuning leaves the view put so the user can pan freely."*

So the view centre is **intentionally independent** of the tuned frequency. Snapping the render onto
the frame's centre — the naive reading of "do what the watch does" — would drag the view back onto
the VFO on every frame and destroy panning. ★ Stuart identified this himself; do not re-litigate it
by "fixing" alignment the simple way.

## 3. The fix: an OFFSET, not a snap

Place each frame by its own centre, expressed relative to the view:

```
offset_px = (frame.centerHz − view.centerHz) / hzPerPixel
```

Why this satisfies both cases at once:

- **Locked** — the offset is normally zero, and non-zero only for in-flight frames during a tune.
  That is exactly the misalignment, corrected at the moment it occurs.
- **Panned** — the offset IS the pan. The view stays authoritative and the frame lands where its own
  centre says it belongs, so panning survives untouched.

It also **self-corrects**: no resync, no waiting for a zoom, and tuning responsiveness is unaffected
because the predicted centre still drives the VFO and readout. The prediction was never the problem;
placing frames as though the prediction were already true was.

## 4. ★ Guard: only shift frames of the SAME span

An offset can only place a frame whose bin scale matches the view. A frame from a different span
must be dropped or rescaled, never shifted — shifting it silently draws the right signal at the
wrong width.

`UberSDRClient` already computes exactly this test for VibeServer's capture-recentre case:

```ts
const sameSpan = v.binBandwidth > 0 &&
  Math.abs(this.status.binBandwidth - v.binBandwidth) <= v.binBandwidth * 1e-6;
```

Reuse that comparison rather than inventing a second notion of "same span".

## 5. Where to work

- `src/services/UberSDRClient.ts` — `_emitSpectrum` / the config handler already carry both centres.
- The Skia render path that consumes the emitted rows.
- ★ **Protected surface**: this is the zoom drum's waterfall ([[feedback_zoom_drum]]). Verify by
  hand, with the drum, before and after — spinning the drum must feel identical.

## 6. Test cases

1. Tune fast with the drum, locked — signal stays under the VFO, no "beside it until you zoom".
2. Tune fast from the **watch** in Companion — same, since this is the path that exposed it.
3. Unlock and pan — the waterfall must stay where you put it and NOT snap back to the VFO.
4. Zoom during a fast tune — no jump when the view re-asserts.
5. VibeServer capture-recentre (same span, new centre) — still adopts, still no snap-back.
