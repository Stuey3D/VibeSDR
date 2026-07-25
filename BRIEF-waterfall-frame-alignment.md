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

---

## 7. ★ The margin buffer — a SEPARATE, later idea (Stuart, 2026-07-25)

Distinct from §3, and worth not conflating. §3 fixes WHERE frames are drawn; this fixes the fact
that a newly exposed edge has **no data at all**, because the server was never asked for it. Correct
placement cannot invent spectrum you were never sent.

Stuart's proposal: request a chunk WIDER than the visible span and hold it, so a pan is served from
the local buffer first and only reaches the server when it runs out. Also thins the view-send spikes.

★ It is not free, and the trade is real: the server sends a fixed `binCount` across whatever span is
asked for, so over-fetching means either coarser bins for the same bandwidth (a blurrier screen) or
more bins for more bandwidth, continuously — in exchange for removing occasional spikes.

★★ **DECISION (Stuart, 2026-07-25): take the slightly higher data rate and keep CLEAN bins by
default; the existing LOW DATA link mode selects the cheap coarse variant instead.** That maps the
trade onto a control users already understand (Link Management: FULL / AUTO / LOW DATA) rather than
inventing a new setting, and it puts the blurrier picture behind an explicit "I am on metered data"
choice.

★ SEQUENCING: do §3 first and re-measure. §3 costs nothing and is expected to remove the
stuck-signals symptom on its own — when the view pans, rows placed by their own centre SLIDE with
the pan instead of freezing, which is what "the ticker moves but the signals stick" actually is.
Only then judge the buffer, on data-rate grounds alone rather than as a fix.

---

## 8. ★★ CORRECTION to §7's sequencing — the buffer is the MECHANISM, not a later optimisation

Found while looking at how to implement §3 in the GPU waterfall (2026-07-25).

**§3 cannot be implemented as written.** `WaterfallView`'s SkSL samples the ring with a SINGLE
GLOBAL x mapping — `tx = clamp(xy.x / uDrawW * uTexW, ...)` — and the ring is 1024 rows of raw bins.
There is nowhere to put a PER-ROW offset, which is what "place each frame by its own centre"
requires. Shifting rows at write time does not work either: it aligns each row to whatever the view
was when it arrived, so after a pan a signal appears at different x in old and new rows and the
trace BENDS.

**So the fix is to store rows by ABSOLUTE FREQUENCY in a window WIDER than the view**, and give the
shader one offset for where the view currently sits inside it. That is §7's margin buffer. Three
results from one mechanism:

1. Alignment correct BY CONSTRUCTION — a signal stays vertical through tunes and pans. No in-flight
   misplacement, no resync, no zoom-to-fix. (This is §3's goal, reached structurally.)
2. Panning and small tunes are FREE — served from the margin, no server round trip.
3. ★ The data rate goes FLAT. The spikes are not throttled, they stop existing: a tune inside the
   margin needs no new server view at all.

★★ **WHY FLAT MATTERS — Stuart's argument, and it is the strongest one here (2026-07-25):**
*"UberSDR already sends very little data and I would rather a consistent data rate from the server
than low when not tuning with a massive spike when tuning. If a server has a dodgy internet
connection or is very close to its user capacity and we hammer a massive spike of data, that could
cause breakup and stutter for ALL users, not just us."*
So a flat rate is SERVER ETIQUETTE, not merely efficiency — the same value as
`kiwi_reconnect_etiquette` and `owrx_profile_etiquette`. A slightly higher steady baseline is
preferable to a low one punctuated by bursts, because the bursts are what hurt other people.
Combined with §7's decision (clean bins by default, LOW DATA selects the coarse variant), the
default should be a steady, slightly-wider request rather than a narrow one that is constantly
renegotiated.

★ SCOPE WARNING: this is a render-path change on a PROTECTED SURFACE (the zoom drum's waterfall,
[[feedback_zoom_drum]]). It wants doing deliberately, with the §6 test cases run by hand before and
after — not bolted on at the end of a session.

---

## 9. ★★ ATTEMPTED 2026-07-25 AND REVERTED — read before trying again

§8's plan was implemented (ring widened to 1.5x the view, rows written at the bin offset their own
centre gives, shader sampling a sub-range via new `uViewStart`/`uViewBins` uniforms). It DID fix the
alignment and made history slide correctly under a pan. It was reverted anyway, because it destroyed
a feature Stuart will not compromise on:

★★ **THE WATERFALL HISTORY MUST SURVIVE A ZOOM.** Absolute placement means a row is only meaningful
at the bin scale it was stored at, so a scale change invalidates the buffer — and the implementation
cleared it. Every zoom restarted the waterfall. Stuart: *"that's a visual element of our app I am not
going to compromise on"*, and zoom is a constant action (worse than rotation, which was unaffected).

★★ **THE DEEPER POINT, and why §8 was still incomplete.** Today's waterfall is deliberately "ALWAYS
THE CURRENT VIEW": history is implicitly re-labelled by whatever the view now is. That is exactly WHY
a zoom instantly rescales the whole history and stays continuous — and it is the same property that
makes an in-flight frame land at the wrong offset. **The bug and the hero feature are the same design
decision.** Any fix must keep continuity, not trade it away.

### The route that could satisfy both (NOT yet attempted)
1. Hold the ring at a FIXED bin scale and let the shader stretch for zoom — it now can, given
   `uViewStart`/`uViewBins`. Small zooms become free magnification with history intact.
2. RESAMPLE the ring (nearest-neighbour, 1024 rows) only when the scale has drifted far — say >1.5x.
   Rare enough to afford ~10-20 ms.
3. ★ THE CATCH: while zoomed in beyond the ring's scale, incoming rows are downsampled into a coarse
   grid — losing the detail the user zoomed in FOR. So the re-base must be prompt, and a zoom-drum
   spin will trigger several.
★★ AND IT IS WORSE THAN A THEORY: the reverted attempt only CLEARED the ring on a scale change, and
that alone was enough to make Stuart report that "zoom feels sticky". Resampling is strictly more
expensive than clearing, so per-scale-change ring work is DEFINITIVELY off the table at drum rates.
**Any viable fix must not touch the ring on zoom at all.**

### ★★ BUILT AND VALIDATED — build 183, code landed in `7e37a92`
Do NOT store rows by absolute frequency. Keep the ring exactly as it is — view-relative — and simply
SHIFT THE INCOMING ROW AT WRITE TIME by `(trueCenterHz - viewCenterHz) / hzPerBin` bins, filling the
vacated edge with the floor.
- Fixes the in-flight misalignment, which is the actual reported symptom ("signal beside the VFO
  until you zoom").
- Nothing touches the ring on a scale change, so the hero feature AND the drum's feel are untouched.
- No shader change, no extra memory: one offset in the existing `set()` call.
- ★ LIMITATION, accepted deliberately: it cannot retro-correct history, so panning while data flows
  leaves a slight kink where the alignment changed, instead of today's smooth-but-wrong re-labelling.
  In-flight offsets are only a few bins, so the vacated edge strip is invisible.
This is the inverse of the reverted attempt: accept that history is approximate, and correct only
the frames arriving now.

★★ **OUTCOME: it works.** Stuart on device (build 183), including the protected surface:
*"bitrate doesn't spike, zooming feels good and tuning still feels nice too."* So the position to
hold is **in-flight frames correct, history approximate** — not a stepping stone to absolute
placement, which is ruled out above.

★ HOUSEKEEPING: the code for this landed inside commit `7e37a92`, whose message is about POSITIONING
— swept in by a careless `git add -A`. Not rewritten, because the branch was already pushed and the
Pi has a clone off it. If you are archaeology-ing this fix later, `git show 7e37a92 --
src/components/WaterfallView.tsx` is where it is, and this section is the real commit message.

### What is still NOT fixed, and may not be fixable
"Ticker moves but the signals stick" during a PAN. That needs HISTORY to move, which needs absolute
placement, which is what broke zoom. The reverted attempt fixed both, so the ceiling is known to
exist — it just costs zoom continuity. Do not attempt it again without a plan for §9's crux.

### Facts established, worth not re-deriving
- ★ `binCount` is NOT requestable — it comes from the server (`status.binCount = floats.length`).
  So `span = binBandwidth x binCount` means a wider span ALWAYS costs resolution, and there is no
  "more bins for more bandwidth" option. Bytes per frame are FIXED regardless of span.
- ★★ Therefore the data-rate spike on tuning was never bigger frames — it was MORE FRAMES, one per
  view reconfiguration. **Data rate is proportional to frame rate, full stop.** Over-fetching a
  margin from the server would cost resolution but NOT bandwidth.
- The 1.5x-wide ring costs 1.5 MB instead of 1 MB, and no resolution at all on its own: the view
  still shows the same bins. Resolution only enters if the SERVER is asked for a wider span.
- `trueCenterHz` is already plumbed through to the renderer (the watch uses it); the phone's own
  render deliberately does not.

---

## 10. ★★ THE FIX IS UBERSDR-ONLY — Kiwi and OWRX get nothing (found 2026-07-25)

`trueCenterHz` is set in exactly ONE place: `UberSDRClient.ts:1117` (`emit.trueCenterHz = frequency`).
`KiwiAdapter` and the OWRX adapter NEVER set it. So on those backends it is `undefined`, the
renderer's `?? centerHz` fallback returns the predicted centre, the computed offset is zero, and
**the §9 alignment fix is completely inert.**

★ This matters for interpreting test results. Stuart validated build 183 on his OWN KIWISDR (MLA30+,
Moulton) and reported it "solved" — but what improved there was the SWEEP work (send coalescing,
the band-default/handsOn fix, the adaptive sweep rate, unlocked edge-follow). The residual
*"very occasional signal next to VFO"* he still sees is the ORIGINAL bug, untouched on Kiwi. It is
not flakiness in the fix; the fix is not running.

Properly fixed on: **UberSDR and VibeServer / local hardware** (both go through UberSDRClient).
Not fixed on: **KiwiSDR, OpenWebRX**.

### To extend it
Each adapter must report the REAL centre of the frame it is emitting, not the one that was
requested. ★ UNKNOWN and not yet checked: whether the Kiwi and OWRX protocols disclose the centre
per waterfall frame at all. If they do not, the adapter would have to infer it from its own
outstanding-request bookkeeping (it knows what it asked for and when the server acked), which is
weaker but probably good enough — an in-flight frame is by definition one sent before the ack.

★★ **THE REAL UBERSDR HAS THIS BUG TOO** (Stuart, 2026-07-25). So it is PROTOCOL-INHERENT, not ours:
a frame in flight inevitably carries the centre it was generated at, and any client that places rows
against the current view will misplace it. Two consequences worth holding on to:
- Nothing was done wrong here — the naive placement is the obvious implementation, and the reference
  client makes the same choice.
- On UberSDR we now handle something the reference does NOT, which is a genuine (if quiet) edge.
It also means there is no upstream fix to wait for, and no reference behaviour to copy: extending it
to Kiwi/OWRX is original work either way.

---

## 11. ★★★ ON VIBESERVER WE OWN BOTH ENDS — which dissolves the whole conflict (Stuart, 2026-07-25)

Every constraint above is an UBERSDR PROTOCOL constraint, not a real one:
- `binCount` comes from the server because we do not own the server.
- A wider span therefore costs resolution, because bins cannot be added.
- The ring's bin scale changes when the server changes it, which invalidates absolute placement,
  which is what restarted the waterfall on zoom (§9).

**On VibeServer none of that holds.** We write both ends, and `RxPipeline::start(fs, fftSize,
fftRate, outRate, cb)` ALREADY takes the FFT size as a parameter. So the server can be asked for a
window WIDER than the viewport at a FIXED, FINE bin scale, with the FFT sized to match.

★★★ Consequences, and they are not incremental:
1. **Panning and sweeping cost nothing** — served entirely from the client-side buffer, no round trip,
   so the data rate is flat by construction rather than by throttling.
2. **Zoom costs nothing either** — it becomes pure client-side sampling inside the buffer.
3. ★★ **And that removes the blocker.** The ring's bin scale NEVER CHANGES, so the ring is never
   invalidated, so history survives zoom AND absolute-frequency placement works. The conflict
   between correct alignment and zoom continuity (§9) existed ONLY because the server owned the
   scale. All three problems were the same problem.

### Design sketch
- Server sends a window of, say, 4x the viewport at the viewport's bin resolution — allowing ~2
  octaves of zoom-out and free panning within it before any renegotiation.
- Client stores rows by absolute frequency (as §8/§9 attempted) — now safe, because the scale is
  stable.
- Renegotiate only when the view leaves the window or wants finer resolution than it holds.
- Public backends (UberSDR, Kiwi, OWRX) keep today's behaviour; this is a VibeServer capability.

### Costs to measure
- **Bytes per frame scale with binCount**, so a 4x window at full resolution is 4x the frame size.
  Unlike the UberSDR case this DOES cost bandwidth — but view reconfigurations disappear, so the
  total during active tuning may be flat or lower. MEASURE, do not assume.
- FFT cost is n log n; VibeDSP's FFT is a small share of the WFM budget (see the pi-bench figures),
  but a 4x FFT on a Pi is not free.
- Client memory: the ring is already 1 MB per 1024 bins; 4x is 4 MB.
- ★ A tunable window factor is the obvious lever, and it maps onto the existing LOW DATA mode:
  narrow window when metered, generous when not.

★ This is the first genuine case of VibeServer doing something the public backends CANNOT, rather
than merely matching them — worth noting for positioning as well as engineering.
