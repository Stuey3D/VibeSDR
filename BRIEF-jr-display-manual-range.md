# BRIEF — Jr: manual range, and returning to where you were

Design session 2026-07-28. Jr build 45.

## 1. Colour sync to Jr: NOT BUILDING IT — decided

The phone syncs its display blob through iCloud (`syncGlobalDisplayPrefs` +
`syncServerDisplayPrefs`). **Jr must not join that.**

★ The tone controls are watch-local *on purpose* — `DisplaySheet` already says
why: "the wrist varies in readability far more than the phone". A phone sitting
on a desk indoors and a watch in sunlight want different brightness, and syncing
would have the phone silently overwrite the adjustment you made *because* you
were outdoors. That is the opposite of useful: it leaves a user out and about
with a waterfall they cannot fix.

★ Palette / VFO colour are a different case and STAY AS THEY ARE. They default
to `"sync"` (match the phone) and stop tracking the moment the user picks one
manually. That is a sensible default, not a takeover.

## 2. Parity with the phone's display options: NO, and deliberately

The phone's blob carries ~20 fields (`SDRScreen.tsx:1272`): sharpness,
spatialSmooth, specPeakScale, specFloor, avgFrames, spectrum ratios, wfCoarse…
Almost none change whether the waterfall is READABLE, which is the only question
the wrist asks. Jr already has the ones that do: auto contrast, brightness,
contrast, palette, VFO colour, peak hold.

★ The one real gap is RANGE. Auto contrast with no manual override is the single
thing the phone can do that Jr cannot, and it bites exactly when auto guesses
wrong on a strong signal — which is when you most want to fix it.

## 3. MANUAL RANGE — the feature

**Where it lives.** Inside the existing "Auto contrast" crown mode, not as a new
row. Turning the crown DOWN past 0 lands on **MAN**. So the control reads
`0 … 10` for auto strength, and one detent below 0 hands you manual — the same
gesture, no new menu to find.

**What it reveals.** Selecting MAN populates two more rows in the Display sheet:
**Floor** and **Ceiling** (dBFS). Both behave exactly like Brightness and
Contrast already do — a crown mode that dismisses to the waterfall so the change
is seen LIVE against real signal, never a slider in a sheet judged blind.

**Why live preview is non-negotiable here.** A floor/ceiling pair is meaningless
as a number; it is only ever "does the noise go black and the signal stay
bright". The phone learned this (the display panel is judged against a running
waterfall); the wrist has less screen and needs it more.

**Reset** must cover the new state: back to AUTO at 5, floor/ceiling cleared.

## 4. HOLD-MENU RETURNS YOU TO WHERE YOU WERE

The problem this solves is created by §3. Manual range is inherently a PAIR: set
the floor, look, set the ceiling, look. Today every adjustment dismisses to the
waterfall and the next one costs a full menu → Display → row navigation, three
times over, on a watch, outdoors.

**Rule.** After adjusting a Display setting via a crown mode, HOLD MENU returns
to the **Display sheet with that row still in focus**, not to the root menu.

**It expires.** If the user has not gone back within a timeout, hold-menu reverts
to its normal behaviour (the default menu). Otherwise a display tweak made an
hour ago silently changes what the menu button does, which is worse than the
navigation it saves.

★ Suggested timeout: **60 s**. Long enough to cover look-adjust-look-adjust,
short enough that it never surprises you later. Also drop the shortcut when the
app leaves the foreground — coming back to the wrist is a fresh intent.

## Open questions

- Does the shortcut apply to ALL Display crown modes (brightness, contrast, auto
  contrast, floor, ceiling), or only the range pair? Leaning: all of them, since
  brightness/contrast are adjusted in a look-tweak-look loop too.
- Floor/ceiling units on the wrist: dBFS numbers, or unitless steps like
  brightness? Leaning: dBFS, because they must mean the same thing as the
  phone's `dbMin`/`dbMax` when comparing two devices side by side.
- Should MAN persist per server, or watch-globally like the other tone settings?
  Leaning: watch-global, matching brightness/contrast, since it is a readability
  setting rather than a property of the aerial.

## Related

- [[watch_split_jr_buddy]] — Jr is standalone; this is Jr only, Buddy takes its
  display from the phone.
- `BRIEF-inputs-shack-mode-mac.md` — the crown-mode language this reuses.
