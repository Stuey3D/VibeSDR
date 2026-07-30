# Expo HAS CHANGED

Read the exact versioned docs at https://docs.expo.dev/versions/v56.0.0/ before writing any code.

# WHEN A CONTROL MOVES, FIX THE COPY THAT SAYS WHERE IT IS

Moving a control is two jobs. The second one is invisible and gets skipped, and then the app
misdescribes itself to the only people who read the tour: new users.

This has now bitten twice in one afternoon (2026-07-30), both times in the same tour card:

- Audio controls moved to the speaker button in **V7.1.0**. The tour still sent people to the cog
  for noise reduction — for three releases. `AboutOverlay`'s own release note describes the move.
- Bookmarks moved to the frequency card. `FreqModal` says so in its own comment ("relocated from
  MenuSheet §4.2"). The tour still listed them under the cog.

★★ The second one was worse: the card was being CORRECTED at the time. The noise-reduction half was
verified against the source and the bookmarks half was copied through unchecked. **Verify the whole
sentence, not the part you came to fix.**

## The grep list
Anything that tells a user WHERE something is:

- `src/screens/SDRScreen.tsx` — `sdrTour` (the coachmark cards)
- `src/screens/InstancePickerScreen.tsx` — `pickerTour`
- `src/components/AboutOverlay.tsx` — release notes AND the feature lists
- `spike/WristSDR/WristSDR/*.swift` and `ios/VibeSDRWatch/*.swift` — `TutorialSheet` / `*TutorialTips`
- `website/index.html`

## The rule
Grep for the control's OLD home and its NAME before you finish the move. A tour that misdirects is
worse than no tour: someone hunts for a slider that is not there and concludes the FEATURE is
missing, not the card. And do not trust these files to be right — they describe a layout that was
true when they were written.
