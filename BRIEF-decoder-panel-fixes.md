# BRIEF: two decoder-panel bugs — first fixes of the next release

**Status:** not started. Stuart, 2026-07-30, deliberately NOT in v10. Both diagnosed below; neither
needs investigation, only doing.

---

## 1. ★★ You cannot see a whole SSTV image on an iPad

**Symptom:** the decoder box is too short — an SSTV picture has to be scrolled around instead of
looked at.

**Cause:** the image height is a **hardcoded 200 points** on every device
(`DecoderPanel.tsx`: `maxHeight={200}`, `dp.body: { maxHeight: 200 }`), while
`DecoderImageCanvas` scales the image to the panel's WIDTH with aspect preserved
(`scale = panelW / dispDims.w`).
★★★ **So the wider the screen, the worse it gets.** A 320×256 frame draws ~288 pt on a phone and
~640 pt on an iPad — both into the same 200 pt window. The device with the most room to spare shows
the least of the picture.

## ★★★ THE FIX ALREADY EXISTS AND ALREADY SHIPS — copy the Advanced RDS panel
Stuart, 2026-07-30: *"we keep the decode box small on purpose so the waterfall and spectrum remain
visible — well if a user wants to look at SSTV then THAT is the primary content, so the big button
could expand the window to show the full image."*

`AdvRdsPanel.tsx` solved this exact problem, and both hazards worth worrying about are already
handled there:
```js
const { height: winH } = useWindowDimensions();
const insets = useSafeAreaInsets();
const avail  = Math.max(180, winH - p.bottomOffset - insets.top - 16);  // never into the status bar
const maxH   = Math.min(avail, p.tall ? winH * 0.82 : winH * 0.34);     // BIG vs SMALL
```
★★ Its own comment records the bug not to repeat: *"LEAVE THE STATUS BAR ALONE. In BIG mode the
panel is anchored at the bottom and grew straight up past the notch, covering the clock and
battery."* That is already fixed here — inherit it rather than rediscover it.

**So:** a BIG/SMALL button on the decoder panel, same place and same behaviour as RDS's.
- **SMALL** = today's 200 pt. The waterfall and spectrum stay visible, which is why the box is
  small in the first place — that is a deliberate choice, not an oversight.
- **BIG** = grow to `avail`, so a whole SSTV frame is visible at once.
- ★ **The image SHRINKS TO FIT when the box cannot grow any further** (Stuart). Scale by
  `min(panelW / w, maxH / h)` rather than by width alone — that is the one change needed in
  `DecoderImageCanvas`, and it makes the picture fit on a phone in landscape too.
- ★ **WEFAX still scrolls.** It grows continuously, so it legitimately wants a scroll even in BIG —
  shrink-to-fit is right for a fixed-size SSTV frame, wrong for an endless fax roll.
- ★ Do NOT copy the RDS panel's other trick: there, SMALL renders *fewer fields*. Here both modes
  show the same one image; only the box changes.

## 2. ★★ SAVE opens a BLANK share sheet on macOS

**Symptom:** pressing SAVE on the Mac pops an empty share sheet.

**Cause:** we hand the share sheet a **`data:` URL**, and the code already predicted this would not
travel. `DecoderImageCanvas.save()`:
```js
await Share.share({ url: `data:image/png;base64,${b64}` } as any, …)
```
with the comment: *"iOS share sheet accepts data: URLs; Android may only take message —
expo-file-system temp-file fallback is the documented follow-up."*

**macOS is the third case nobody tested.** An iOS app running on Apple Silicon gets AppKit's share
services, which will not take a `data:` URL — so the sheet opens with nothing in it. It does not
throw, so the `catch` never fires and the status line still says "shared".

**Fix:** the follow-up that comment already names — write the PNG to a temp file and share a
`file://` URL. That is the path that works on iOS, macOS AND Android, so it removes the
platform-specific branch rather than adding one.
```
FileSystem.cacheDirectory + `${decoderName}_${ts}.png`  →  writeAsStringAsync(base64)  →  Share/Sharing
```
★ While in there: the status line reports success unconditionally. It should say what actually
happened, or say nothing — "shared" over a blank sheet is worse than silence.
★ Test on macOS specifically. Both bugs were found there, and it is the platform that gets the
least testing precisely because it is "the iPad app".
