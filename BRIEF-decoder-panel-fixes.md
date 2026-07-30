# BRIEF: two decoder-panel bugs — first fixes of the next release

**Status:** not started. Stuart, 2026-07-30, deliberately NOT in v10. Both diagnosed below; neither
needs investigation, only doing.

---

## 1. ★★ You cannot see a whole SSTV image on an iPad

**Symptom:** the decoder box is too short on iPad — an SSTV picture has to be scrolled around
instead of being looked at.

**Cause:** the image height is a **hardcoded 200 points**, on every device.
- `DecoderPanel.tsx`: `<DecoderImageCanvas maxHeight={200} …>` and `dp.body: { maxHeight: 200 }`
- `DecoderImageCanvas.tsx` scales the image to the panel's WIDTH with aspect preserved:
  `scale = panelW / dispDims.w; drawH = dispDims.h * scale` — then puts it in a `ScrollView` capped
  at `maxHeight`.

★★★ **The wider the screen, the WORSE it gets.** A 320×256 SSTV frame on a phone at ~360 pt wide
draws ~288 pt tall — already over the cap. On an iPad at ~800 pt wide it draws ~640 pt tall into the
same 200 pt window, so you see under a third of the picture. The one device with room to spare shows
the least of it.

**Fix:** make the cap responsive instead of constant — a fraction of the window height (with a
sensible floor), or enough to show the full `drawH` when the space exists. The scroll should be the
fallback for a tall WEFAX roll, not the normal way to view a 4:3 SSTV frame.
★ Check WEFAX too: it grows continuously, so it legitimately wants scrolling — the cap must not
become "infinite" for that mode.
★ Check landscape and Split View on iPad, where height is scarce and width is not.

---

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
