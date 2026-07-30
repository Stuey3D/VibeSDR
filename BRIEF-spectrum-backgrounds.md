# BRIEF: custom spectrum backgrounds

**Status:** not started. Post-v10. Written 2026-07-30 from Stuart's design.

## What exists today
The spectrum pane can be drawn over a background image **sent by the server**. Only **UberSDR**
supports sending one — no other backend (Kiwi, OWRX, FM-DX, SpyServer, VibeServer) has any notion
of it. So on every backend but one, the pane is plain.

★ It works well: Stuart's own UberSDR skyline sits behind the trace and neither fights the other,
because the artwork is dark where the trace lives.

## The ask — TWO controls that compose, not three states
In **Display settings**:

1. **Custom background** — a button that opens the picture chooser (prompting for photo-library
   permission on first use), plus a **Clear** that returns to plain black.
2. **Apply to** — *All servers* / *Only servers that don't send their own*.

★★★ **THE ELEGANT PART: "cleared" IS a background.** Because the scope control governs the blank
choice too, `no image + apply to all` means **plain black everywhere** — which is how a user hides
a server's artwork. Hiding server backgrounds falls out of the two controls rather than needing a
third setting for it. (Stuart's design; Claude's first draft had a clumsier three-state "never /
when the server sends none / always".)

So the four combinations are all meaningful:

| Image | Apply to | Result |
|---|---|---|
| none | only where the server sends none | **Today's behaviour** — server artwork, else black |
| none | all servers | **Plain black everywhere** — server artwork suppressed |
| chosen | only where the server sends none | Server artwork wins; yours fills the other five backends |
| chosen | all servers | Yours everywhere |

★ Default must be row 1, so nothing changes for anyone who never opens the setting.

## ★★ NOT a moderation feature — do not re-argue this
Claude framed this as an App Store risk (unmoderated third-party imagery, Guideline 1.2). **Wrong:
6.1 has been live since 2026-07-01 with server-supplied backgrounds and passed review.** See
`memory/appstore_manual_release_checklist.md`. Build it for the user, not for the reviewer.

## Real constraints
- ★★★ **LEGIBILITY IS THE WHOLE PROBLEM.** The skyline works because it is dark behind the trace.
  A user will pick a bright holiday photo and make their own spectrum unreadable, then conclude the
  app is broken. Needs a **darkening scrim with a strength control**, defaulted high enough that any
  image is safe, and a live preview while choosing — [[design_live_bar_draggable_needle]]: a
  threshold is a live meter you drag, not a number you type.
- **Performance.** Downscale once to the pane's pixel size and cache; never hand a 12 MP photo to a
  view that redraws at 10–60 fps. The waterfall Canvas is the app's hottest path.
- **Storage is LOCAL.** iCloud sync uses `NSUbiquitousKeyValueStore` (~1 MB total, shared with
  every favourite and bookmark) — an image cannot go in it. Say so in the UI rather than let people
  expect it to follow them to another device.
- **Privacy.** The chooser prompts for photo-library access on first use, which is a new permission
  AND a new App Privacy answer on the listing. ★ Check whether `PHPickerViewController` (iOS) can
  do this WITHOUT the permission — it hands back only the chosen image and normally needs no
  authorisation at all, which would mean no prompt, no privacy declaration, and one less thing a
  cautious user can refuse. Same question for Android's photo picker.
- **Both panes?** Decide whether the image sits behind the spectrum only (as now) or the waterfall
  too. Behind the waterfall it will be invisible under a busy band and distracting under a quiet
  one.

## Open questions
1. Global, or per-favourite? Global is the ask; per-server is a natural follow-on and would let the
   image match the receiver (a coastal photo for a marine-band Kiwi).
2. Does the watch get this? Jr renders its own waterfall and has room for nothing decorative —
   probably not, and worth saying so out loud rather than leaving it ambiguous.
3. Web client too? It has skins already (`skinHtml.ts`), so the concepts may collide.

Related: `BRIEF-server-identity-header.md` (the same "make every backend feel like UberSDR" idea),
`memory/skinhtmlts_ui_notes.md`.
