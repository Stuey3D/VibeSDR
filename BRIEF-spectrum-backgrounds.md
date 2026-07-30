# BRIEF: custom spectrum backgrounds

**Status:** not started. Post-v10. Written 2026-07-30 from Stuart's design.

## What exists today
The spectrum pane can be drawn over a background image **sent by the server**. Only **UberSDR**
supports sending one — no other backend (Kiwi, OWRX, FM-DX, SpyServer, VibeServer) has any notion
of it. So on every backend but one, the pane is plain.

★ It works well: Stuart's own UberSDR skyline sits behind the trace and neither fights the other,
because the artwork is dark where the trace lives.

## The ask
Let the user supply their own image, with a **three-way choice** (Stuart's wording):

| Setting | Behaviour |
|---|---|
| **Never** | Server's image if it sends one, otherwise plain. Today's behaviour. |
| **When the server sends none** | The user's image fills the gap — so every backend gets the UberSDR look, and UberSDR keeps its operator's artwork. |
| **Always** | The user's image overrides the server's. |

★★ The middle option is the interesting one: it makes the feature about **the other five backends**
rather than about replacing UberSDR. Most users will never see a server-sent background at all.

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
- **Privacy.** Reading from the photo library is a new permission prompt and a new App Privacy
  answer. A file picker may avoid the library permission entirely — check before choosing.
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
