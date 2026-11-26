# METEOR LRPT: gaps squashed, fit / follow, north up (beta96, 2026-11-26)

Branch `b96-lrptgap` (off `b96-integrate`). Stuart, on a real Meteor-M2-4 pass (11-26, 14:40 and 14:49 near LOS):

1. the beta95 hatch worked, but a gap was drawn at FULL height (616 lines of no signal took as much room as the picture);
2. near LOS, thin noisy strips between short gaps each got a label ("NO SIGNAL 14:42" ×3) and pushed the clean cloud
   out of view;
3. with MAP OUTLINE on: a northbound pass is south-up — turn it so north is up (live, gallery, outline);
4. the saved "map outline" copy's grid labels read "N°55", "M°02" (see below);
5. the SAVED picture keeps gap rows plain black (Stuart's answer via the coordinator) — not touched here.

## What was built

| Where | What |
|---|---|
| `vibe_lrpt_run.h` `gapart::mergeGaps` | the preview's gaps, merged: a run of picture of ≤ 1 strip (8 lines) or under 5 s between two gaps makes ONE gap, `patchy`. Bounded at 64 (the closest pairs merge). |
| `vibe_lrpt_run.h` `previewGapsJson` | `lrpt_status` gains `"gaps":[[r0,r1,msA,msB,lines,patchy],…]` in PREVIEW rows (true geometry), only gaps whose rows were all sent. The preview rows themselves are unchanged (older clients see beta95's drawing). |
| `vibe_lrpt_overlay.h` `northbound()` | the pass's direction: the chosen METEOR's TEME velocity z sign at the pass time (same satellite choice as the outline; −1 = not known → left as scanned). |
| `vibe_lrpt_run.h` `decideHeading` | asked every 5 s until known, then kept for the pass; `lrpt_status` `"rot":180` / `"rot":0`. |
| `vibe_lrpt.h` `renderPictures(rx, rot180)` | a northbound pass's gallery PNGs are turned 180° ONCE (turned for the PNG and back, no extra copy); name "… · north up", PNG `tEXt` Comment `kRotatedNote`, `Picture::rot = 180`. |
| `vibe_lrpt_overlay.h` `makeOverlay(…, rot180)` | the gallery outline drawn in the TURNED picture's pixels: (x, y) → (W − x, rows − y), labels placed in turned coordinates, corners for the turned picture, `"rot":180`. The geo maths is untouched. |
| `vibe_png.h` | `encode(…, comment)` writes a `tEXt` chunk (the burn decoder skips unknown chunks). |
| `src/utils/lrptGaps.ts` (app + web) | parse, `squashPlan` (each gap one band of ≤ 32 rows; 2-line label ≥ 24 rows, 1-line ≥ 9, none below), labels "NO SIGNAL 14:40-14:41" / "616 LINES" / "PATCHY, 88 LINES", never repeated (same minute → seconds), our own 5×7 glyphs, `renderSquashed(…, rot)`, `mapOutline(…, rot)` (points turned, label TEXT upright at a turned anchor), `lrptFits` (fit while the fitted width ≥ half the box), tap toggle. |
| app | `DecoderImageCanvas` props `gaps`, `rot180`: the live SkImage built squashed/turned (buffers stay true geometry: PREV, SAVE); SATELLITE box (fullWidth) fits the whole pass when `lrptFits`, a tap flips fit/follow; turned = follow the TOP. Store: `lrptOutlineLive` `gaps`/`rot` from `DecoderClient` (primary client only — VFO B's LRPT shows unsquashed). |
| web | `main.ts` `blitToVisible`: segments via `drawImage` (under a 180° transform when turned), bands via `putImageData` at the turned place (upright); rows coalesced per animation frame; `#decImgWrap.lrptFit` in the SATELLITE box, click flips; follow = top when turned; outline through `mapOutline`. |

## The mirrored labels (item 4)
`vibe_lrpt_burn.h` draws label glyphs upright in pixel space (test: the 'N' of "5°N" has its diagonal on the left in
glyph row 2 and on the right in row 4). "M°02" for "20°W" is a 180° TURN (W turned is M; mirrored W is still W): the
saved picture was south-up with upright labels, and turning it round to look at it turned the text. Now the server
turns the picture FIRST and burns upright text into the turned one; the burnt copy of a turned picture is not turned
again (test). The live preview never turns text (bands are placed, not turned; outline label text is drawn at a
turned anchor).

## Air & Sea map
Nothing drops a METEOR picture onto a map (searched web/mapkit, the map services, community map): the outline is only
drawn over the picture, so nothing can rotate twice. Older gallery pictures (before this beta) stay as scanned.

## Tests (this Mac, silent)
- `test-lrpt --quick` 84/0 (single-file g++ -O2, nice 10): `testGapMerge` (two gaps stay two; a 12-gap fading tail = one
  patchy gap; 4.9 s merges, 6.2 s does not; bounded, no line lost), the 35 s live-pass case lists one gap whose rows are
  the hatched ones, and the gallery picture keeps true geometry (rows = decoded + gap).
- `test-lrpt-geo` 80/0 (run-tests' own flags, built through the lock): north/south from the orbit on Stuart's 23 Nov
  pass and a southbound pass found on the same elements; the turned outline (rot, corners: top is north); through Run:
  northbound pictures turned EXACTLY once (pixel for pixel), named, PNG Comment, outline rot, status rot 180; southbound
  as scanned, status rot 0; burnt label glyph upright; the burnt copy of a turned picture not turned again.
- `test-lrpt-host` 33/0.
- `npx tsx scripts/test_lrpt_app.ts` 56 ok (+the squash: 32-row band, label + count, decoded rows exact, true buffer
  byte-identical, outline mapped, three gaps / no repeated label, patchy tail, fit keeps the clean part, turned pixels,
  upright band at the turned place, turned outline).
- `tsc --noEmit` (4 GB): clean apart from the pre-existing `handheld-ui/test` and `scripts/wefax-world` node-type errors.
- NOT done: a headless-Edge check of the web live preview (the web gallery test needs a whole vibeserver; the live path
  needs LRPT frames). Synthetic only — nothing on a real pass.

## RESUME HERE
- On the next real pass: check the band height/labels, PATCHY near LOS, fit/follow tap in the SATELLITE box (web + app),
  north up on a northbound pass with MAP OUTLINE on, the gallery copy "north up" and its outline.
- Open: the fit rule (≥ half the box width) may want tuning on a desktop SATELLITE box; VFO B's LRPT is unsquashed.
