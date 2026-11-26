# The 11.0.17 iPhone Duo work on V12 (beta95) + the FM-DX screen on the Duo

Branch `b95-duo` (off v12-experimental 8fc99e180). V11 twin of the FM-DX fix: branch `duo-fmdx-v11` (off main), unpushed,
for the next store update.

## RESUME HERE

- Everything below is committed on `b95-duo`; nothing pushed or merged.
- Owed: a look on the Duo simulator / a real Duo at the half-open BIG boxes for the V12-only decoders (they stay one box
  below the fold, BIG not offered — see "Not split"), and at the FM-DX screen in every pose with a live FM-DX server
  (no simulator shots were possible — see "How it was tested").
- Not done on either branch (pre-existing, from the 11.0.17 notes): the band-label overprint rule in the WEB client
  (`web/client/src/main.ts` drawBandBar). The app's rule (WaterfallView, 35459577a) is ported.

## What was ported (main 18e95a09c..037f84650, the 11.0.17 release)

Cherry-picked in order with `-x` (so each V12 commit names its main original), conflicts resolved by hand against V12:

| Item | V12 |
|---|---|
| Duo layout: full bleed, camera / pill avoidance, deck lift, 0.81 short-window deck scale, REACH key, Split View shares, edge tabs, name fade, 4 spectrum ratios (b8b804c8c … 968a8d23d, ba6527430, c8056a460, df4a571b6) | ported |
| Status-bar hiding (ecff821d2) and its revert (e4df4c063) | not carried (they cancel) |
| SHARED TUNER head-count in the ask states (d997d2720) | ported, adapted: V12's banner speaks the chat's language (chatT) and keeps the scanner lock and DAB own-stations line ahead of it |
| Band-label overprint, app (35459577a) | ported (clean) |
| Half-open two-panel layout (2e737279b … 16877a204, 4e8a5a934, 19e2e4ff3) | ported; DabPanel adapted to V12's scan pane; DecoderPanel keeps V12's pad keys, LRPT outline, skimmer rows, Band governor chips |
| Advanced RDS / spots narrow panels (907ba19d4) | ported; compact spot rows keep CallLink / mode colours / skimmer text; compact time is HH:MM (seconds on the EXPAND line) |
| Servers tab auto-collapse + menu on top (f15efa51d, 30b7be937, 1a41575d8) | ported onto V12's TOP_MENU_LAYER (9000, on the root — V12 already had main's bc0bbeb60 as d1d2b0fd0) |
| Servers menu root layer (bc0bbeb60) | already present (d1d2b0fd0 / 0d7a5fd76); its SERVERS_MENU_Z 9000 and test carried |
| Spots map server location (fd02912e2) | already present (f65daddb4) |
| RTL-TCP fix (171d88b1b, f48883fe7, d796daa0b, 4cc98690e, 03b9958c5) | already present (rtltcp-fix-v12 merge 6a4bd8ea9; V12 then replaced the server with the engine's door) |
| HFDL MAP title (68ded8890) | ported (MapOverlay only; its swept-in pod files and their revert f66605a97 not carried) |
| About V11.0.17 note (f686ed136, 2424fb81e) | ported as the V11 history entry; a V12 beta95 note added |
| Version bump / iOS DSP lib (2424fb81e, 037f84650) | not applicable |

### Not split (V12-only bodies)
CW / PSK / RTTY rows, JT-TTY, DSC, NCDXF, balloons, APRS and the scanner have no two-panel layout. In half-open they stay
one box below the fold and BIG is not offered (DecoderPanel `noSplitBody`). Plain text decoders (TRANSCRIBE, RTTY text,
Morse) flow across the two panels as on V11.

## FM-DX screen on the Duo (TunerScreen.tsx)

The screen keeps its safe-area insets (no spectrum to run full bleed). On a hinged display:
- header and card columns stop 8 pt short of a camera / pill rect over their rows, and drop below a full-width status
  band — `duoGeometry.rowClearance` (tested in test_duo_layout);
- the deck lifts above a camera that falls on it (`deckClearance`), and sits half the home-indicator inset + 4 from the
  bottom in landscape, like the receiver screen;
- folded portrait: the REACH key (same remembered setting);
- half-open: the scroll ends 8 pt above the fold; the station strip and the deck are in the bottom half, the deck laid
  out smaller (UiWindowOverride) if it would reach the fold.
No rects (every iPhone / iPad / Mac / Android) = all zero, layout unchanged.

## How it was tested
- `tsc --noEmit`: clean on src (both branches; the scripts/ and handheld-ui/ node-type errors are pre-existing).
- test_duo_layout (with 8 new rowClearance checks), test_edge_chip, test_shared_banner, test_servers_chip,
  test_decoder_shell_layout, test_receiver_place (tsx) 29/0, test_chat_i18n 102/0, test_faceplate_nixie 18347/0,
  check-servers-menu-layer: all pass.
- Simulator: NOT run. A local iOS simulator build is not possible from this tree as it stands: the committed
  `ios/VibeSDR/opus/libopus.a` and `modules/vibe-local-sdr/libs/libvibelocalsdr_ios.a` are DEVICE-only arm64 (platform
  IOS), so a simulator link fails (Release; Debug failed earlier on RCTEventEmitter / a Swift 6.4.2-vs-5.10 module
  mismatch). Retagging the objects (vtool) failed (no room for the load commands); rebuilding opus for the simulator from
  the xiph source was refused by the permission system. The FM-DX fix rests on geometry reasoning with the simulator's
  measured rects (test_duo_layout) — a device / sim look is owed.
