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
  (`watchProvider.ts`). Decide whether both get it in the first build.
- The phone's DIGITAL SPOTS panel already consumes the server's spots — reuse its message handling, do not
  invent a second reading of the same feed.

## Questions to settle before building

- **The shared dial.** Picking a band retunes the receiver for everyone listening (THE SHARED-DIAL CONTRACT:
  the client transmits only on a user action — a pick IS one, so it is allowed, but it takes the dial from
  anyone else). Show the listener count and the shared-tuner line before the retune, as the phone does?
- **Which bands to list.** Only those the connected radio can tune (from hwinfo), so an FM-only RTL-SDR server
  never offers 160 m. Dial frequencies from the standard FT8/FT4 table.
- **Where the decoder runs.** Confirm the server decodes on whatever frequency the dial is on (so the background
  tune is all it takes), and what the spot message carries for the map (grid, distance, SNR, time).
- **Leaving the screen.** Does the dial stay on the FT8 frequency (as DAB stays on its multiplex), or return?
