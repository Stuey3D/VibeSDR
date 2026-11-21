# Skimmers by band (V12 beta90)

Stuart, 2026-11-21, on the Pi 500's Background Decoding editor:

- "cw skimmer and rtty skimmer should be per band rather than entering a centre frequency"
- "48k should default to 7024 for 40m 96k 7048 etc so the bands cw allocation is fully covered"
- "in fact cw/rtty skimmer could have a band selection in the decoder box header and the normal tune buttons could fine
  tune it if needed"
- "background should have the band presets but with an option to set their own centre if needed"
- "this then ties in with the pskr reporting we are adding"

## The rule (one place)

A skimmer's window starts at the **bottom of the band's segment for its mode** and extends by the skimmer's width:
`centre = segment bottom + width / 2`. 40 m CW in Region 1 (7000–7040): 24 kHz gives 7012, 48 kHz gives 7024, 96 kHz
gives 7048. RTTY and PSK work the same way on their own segments.

- Server: `android/app/src/main/cpp/vibe_skim_bands.h` holds the segments for each IARU region and `windowCentre()`.
- JSON: `,"skimBands":{"region":R,"cw":[{id,name,lo,hi}…],"rtty":[…],"psk":[…]}`, written by one function
  (`vsSkimBandsJson`, local_sdr_shim.cpp). It goes into vibeserver.json/hwinfo (the listener's decoder boxes), GET
  /vibeserver/hardware (the Setup page) and backgroundCapsJson (the Android server screen). A running radio lists only the
  segments it can tune (its range, the owner's allow/block ranges, a locked range). The door or a stopped radio lists the
  whole region.
- Clients apply the same rule to that JSON: the Setup page's `bgSkim*`, `src/services/bgDecode.ts` `bgSkim*` and
  `src/utils/skimBands.ts` (the web decoder box and the app's ModeSelector). None of them keeps a table of its own.

## Several windows per band (beta90 follow-up, Stuart: "maybe list multiple centre points per band")

`vibe_skim_bands.h points()` lists each band's windows at a width, in two parts:

- **Tiles.** The segment is cut into windows of the skimmer's width, starting at its bottom (the first tile is the
  preset). Each centre is `lo + width/2 + k·width`, for as long as the tile starts below the segment's top. For 40 m CW in
  Region 1 that gives 7012 and 7036 at 24 kHz, 7024 at 48 kHz and 7048 at 96 kHz.
- **Practice segments.** Where activity really sits outside the plan, the region's practice segments are tiled by the same
  rule and labelled:
  - Region 2, 40 m RTTY, 7080–7125 kHz, "US RTTY". Source: the ARRL's Considerate Operator's Frequency Guide.
  - Region 2, 40 m PSK31, 7070–7075 kHz, "US PSK31". Common practice only, so it is kept to one 5 kHz stretch.
- **60 m.** This is the WRC-15 allocation, 5351.5–5366.5 kHz. CW and RTTY take all of it; PSK takes the narrow-band part,
  5351.5–5354. No licence note on 60 m (Stuart 11-21: we only receive — licences govern transmitting).

**JSON.** Each band in skimBands now has `"points":{"24000":[{"hz":…,"note":…}],…}`, with entries for every width its
skimmer offers: CW and RTTY at 24, 48 and 96 kHz, PSK at 6, 12 and 24 kHz. Only windows this radio can tune end to end
are listed, and a band with none is left out. The pages compute nothing beyond a window's edges, `centre ± width/2`. An
older server, whose bands have no `points`, still gets one rule-centre per band.

**The pickers:**
- **Setup page.** The windows are grouped under `<optgroup>`s, with "Own centre…" last.
- **Android screen.** The windows are chips under each band's name, with "Own centre…" last.
- **Both background pickers.** A stored centre matching any listed window loads as that window. A width change takes the
  same band's nearest window.
- **Listener decoder box.** Picking a band goes to its first window. A second compact select (web) or one chip row (app)
  lists that band's windows. It is drawn only when the dial is on a band with more than one window. This is the
  least-noisy design: no extra control for the many bands that have a single window.

## The segments

The segments come from the published plans:

- R1: IARU Region 1 HF Band Plan, effective 16 Oct 2020. 6 m CW comes from the R1 VHF plan.
- R2: IARU Region 2 Band Plan, September 2020.
- R3: IARU Region 3 Band Plan, R3-004, revised 3 Sep 2019.

What each mode uses:

- **CW** uses the plan's CW-only segment.
- **RTTY** uses the plan's narrow-band digimode segment. None of the plans gives RTTY a sub-segment of its own.
- **PSK** uses the same segment, but starts where PSK31 is conventionally found: 1838, 3580, 7040, 10140, 14070, 18100,
  21070, 24920 and 28120 kHz. This is needed because a 6 kHz PSK window starting at 3570 would miss 3580.

60 m is left out, because it is national channels rather than one segment. 6 m has CW only. The regions differ on 160 m
in all three, on 15 m in R2, and on 80 m, 40 m and 30 m in R3.

## Background Decoding

**Rows.** `rtty-skim-24/48/96` are new profiles. They run on the decoder host's `rttyskim` and feed the same IQ path as
`cw-skim`. Their summary card is `rttyskim` and they use the listener's worker count.

**Gate.** `Params::skimWhyNot` stops a skimmer when the owner has switched its mode off, when the radio is an Icom, or
when the mode is out of reach (`vsDecoderWhyNot`). The VFO is then shown paused with the reason, and it never runs. The
Setup page and the Android screen only offer a skimmer where it can run.

**Band picker.** A CW, RTTY or PSK row picks its band, and the window placed by the rule. The last option, **Own
centre…**, shows the kHz box. A stored centre that matches a band's centre loads as that band; any other value loads as
the owner's own centre, with the box filled.

**Changing the decoder.** A wider window keeps its band (7024 → 7048). An owner's own centre is kept. Any other row lands
on the first band whose window fits the capture.

**Save.** The save still takes any centre inside the capture; no band is required.

## Listener decoder boxes

In the SKIMMER view of CW, PSK and RTTY there is a **BAND** row:

- On the web it is a select in each settings panel (`#cwBand`, `#pskBand`, `#rttyBand`).
- In the app it is chips in ModeSelector.

How it works:

- A pick tunes through the normal decoder tune (`decTune(clampTune(…))` on the web, `onTuneHz` in the app), with the
  skimmer's current width.
- The tune buttons still move the dial. The row then reads "40 m · tuned", or "—" when the window is on no band. It
  never snaps back.
- Only bands whose whole window this radio can tune are offered. On the web this means VFO B's window, the lock, the
  owner's ranges and the radio's reach. In the app it is `canReach`. When no band fits, the row is not drawn.
- The shared-dial rules are the same as for a skimmer row tap.

## PSK Reporter

- `adifMode` maps `RTTY` to RTTY, and the PSK skimmer's `BPSK31`/`BPSK63` to PSK31/PSK63.
- `DecoderHost::noteReporter_` handles both RTTY and PSK spots, listener and background alike, with the CW path's
  guards: a finite RF above 0, a NaN SNR treated as 0, and the slot set to now. The reporter's own rules are unchanged.
- The PSK Reporter card on the Setup page (`#pskrWhat`) and on the Android server screen says which decoders report.

## Tests

| Test | What it covers | Result |
|---|---|---|
| `vibeserver/test-bg-skim.cpp` | Table and rule (7012/7024/7048, RTTY/PSK examples, region differences), capture fit, JSON and its tunable filter, config/parse/validate (own centre kept, out of window refused), switched-off gate, and end to end: synthetic 40 m RTTY (2 stations) at 250 kS/s → BgRunner → `rttyskim` → RTTY summary card, with both spots reaching PSK Reporter | 44/0 |
| `vibeserver/test-spot-report.cpp` | +RTTY / BPSK mapping; an RTTY spot is kept when reporting is on and dropped when off | 64/0 |
| `vibeserver/test-rtty-skimmer.cpp` | Host path: the listener's RTTY skimmer spots reach the reporter | all passed |
| `scripts/test-setup-bg-skim.mjs` | Setup page picker, Own centre, load/save, width change, fit, gates | 27/0 |
| `scripts/test_bg_decode_model.ts` | Android screen model | 53/0 |
| `scripts/test_skim_bands_picker.ts` | Listener picker logic | 15/0 |
| `scripts/test-web-skim-band.mjs` | Real server and web page in headless Edge (`--mute-audio`): RTTY and CW SKIMMER BAND, rule-centre picks, "20 m · tuned" with no snap-back, ONE SIGNAL hides it; the band's windows select | 21/0 |

## RESUME HERE

- Nothing has been run on a device or against a live band. All tests use synthetic IQ and stand-in DOMs, plus one real
  server and web page under the fake rtl_tcp.
- For Stuart to decide:
  - **R2 40 m RTTY/PSK.** Answered: both are listed. The plan's 7040 comes first, then the labelled US practice windows.
  - **PSK convention.** The PSK starting points are convention, not taken from a plan.
  - **The one-signal PSK decoder** reporting: answered, keep it (Stuart: "report all spots when it is enabled").
- R36S handheld: no BAND picker yet. It would read the same skimBands from vibeserver.json.
