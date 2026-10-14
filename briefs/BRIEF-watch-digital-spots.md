# BRIEF — Digital spots on the Apple Watch (VibeServer)

**Status:** designed by Stuart, 2026-10-14. **Not built** — candidate for the next build.
**Why:** VibeServer already decodes FT8 itself (server-side spots), but the watch has no way to show them. The
watch already has two screens that, between them, are exactly this feature: the DAB list (a picker plus a live
list) and the ADS-B view (a list that flips to a map).

## The design (Stuart's)

1. **Menu → DIGITAL SPOTS.** Opens a list view like the DAB screen, tuned to a sensible default such as
   **40 m FT8, 7074 kHz USB**. Spots arrive from the server and fill the list.
2. **The band/mode button** sits where DAB's multiplex button is (`DabBlockSheet`). Tapping it lists the modes
   by band:

   ```
   160m FT8
   160m FT4
   80m FT8
   80m FT4
   …
   ```

   Picking one tunes the SDR in the background to that dial frequency and mode, and the decodes from there
   appear in the list.
3. **The map button** switches between the list and a map, exactly as it already does for ADS-B
   (`AircraftView`): each spot placed by its grid locator.

**Only when connected to a VibeServer** — that is where the decoder runs. On other servers the menu item is not
offered (AGENTS.md: a control that only works in one scenario must not be visible where it does nothing).

## Starting points in the code

- `ios/VibeSDRWatch/DabView.swift`, `DabBlockSheet.swift` — list + picker shape (Buddy).
- `ios/VibeSDRWatch/AircraftView.swift` — list ↔ map switch (Buddy).
- Jr (`spike/WristSDR/WristSDR/`) talks to the VibeServer directly; Buddy goes through the phone
  (`watchProvider.ts`). Both get it (see Decisions).
- The phone's DIGITAL SPOTS panel already consumes the server's spots — reuse its message handling, do not
  invent a second reading of the same feed.

## Decisions (Stuart, 2026-10-14)

- **The shared dial: the same as DAB on a VibeServer today.** Whatever the watch's DAB mode does about other
  listeners when it takes the dial, digital spots does too. No new rule.
- **The DIGITAL SPOTS menu item appears only when BOTH:** the server reports it can tune a band that has digital
  spots, **and** the owner has the feature enabled. Otherwise it is not in the menu at all.
- **The picker lists every band the radio can tune, in band order, FT8 and FT4 each.** Those are the two modes
  supported today; the list is built from data so more modes can join later without a new screen.
- **Both apps get it:** Buddy (through the phone) and Jr (direct to the VibeServer), in the same build.
- **Leaving goes back to the radio.** Leaving DAB or digital spots returns the dial to the **last signal the user
  was listening to as an ordinary radio**, or, if they went straight in without listening to anything, to the
  **landing frequency the server owner set**. ★ This is the rule for DAB as well — check what the watch's DAB
  exit does now and bring it into line in the same build (memory note: DAB remembers and resumes, but the
  landing half was unbuilt).

## Still to confirm in the code

- That the server's FT8/FT4 decoder follows the dial, so the background tune is all a band pick needs, and what a
  spot carries for the map (grid, distance, SNR, time).
- Where the owner's enable switch and the "can tune a digital-spots band" signal live in hwinfo, so the watch
  reads them instead of guessing (APP HAS NO HARDCODED SETTINGS vs a server).
