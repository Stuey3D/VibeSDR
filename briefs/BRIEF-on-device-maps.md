# Maps on the device — the shape Stuart specified, 2026-09-27

## ★★★ THE GOAL, IN HIS WORDS
*"That is what I want — maps stored on device so animations are smooth. That animation when it
works is awesome. My mate told me his 5 year old sat and watched it for ages on his phone."*
▶ An animation a child will watch is the bar. Judder is the defect, not a polish item.

## ★★★ TWO TIERS, AND WHAT EACH IS FOR
| | ships with the app | what it gives |
|---|---|---|
| **tier1** (+tier0) | YES, ~3.4 MB gz | coarse countries, large cities, airports. **Smooth and fast.** |
| **tier2 + relief** | downloaded on request | real detail — the Sahara's sand, snow on the mountains |

Stuart: *"if tier one maps only then coarse countries and large citys and airports but smooth fast
performance; tier 2 maps downloaded real detail."*

## ★★★ THE STORAGE NOTE — AT THE BOTTOM OF THE DIRECTORY
The shape already agreed for the old cached tiles, reused:
> **On Device Maps Storage in Use 200 MB.** Removing the higher-detail maps will restore 150 MB of
> space to the device. **Remove?**
▶ Disclosure plus a way back. ✗ An app that silently occupies 200 MB and offers no way to reclaim
it is the thing people uninstall over.

★★ **AND THE SAME PLACE OFFERS THE DOWNLOAD, FOR ANYONE WHO MISSED THE PROMPT.** Stuart,
2026-09-27: *"also a button in the main directory to download the higher detailed maps in case the
original prompt was missed."* So one control that reads the device's actual state and offers the
opposite action:
| detail maps installed? | the footer says |
|---|---|
| no | **Download higher-detail maps** — with the size it will take |
| yes | **On Device Maps Storage in Use 200 MB** — removing restores 150 MB. **Remove?** |
★★★ A first-run prompt is a ONE-TIME offer, and a one-time offer is one a person misses while
they are busy getting a radio working. A setting that is only reachable by reinstalling is not a
setting. ▶ The prompt is the convenience; THIS is the actual control.

## ★★★ THE ARCHITECTURE PROBLEM THIS SOLVES — AND IT IS CURRENTLY BACKWARDS
The WebView is loaded with the INSTANCE as its origin so `/addon/hfdl/aircraft` is same-origin
(most UberSDRs have CORS off). But a page on `https://someone.ubersdr.org` may not read local files
or loopback HTTP. So map data went over the RN BRIDGE — and that is the wrong way round:

| | size | cadence | today | should be |
|---|---|---|---|---|
| map data | ~15 MB | once, static | **the bridge** ✗ | on disk, local origin |
| aircraft/spots | a few KB | every 5 s | direct fetch | **the bridge** |

★★★ **PUT THE LARGE STATIC THING ON DISK AND THE SMALL FREQUENT THING ON THE BRIDGE.** Then:
- layers come off disk at native speed — no per-layer round trip, no judder;
- RN's own fetch is not bound by browser CORS, so an instance with CORS off still works — which is
  the very reason the instance origin was chosen in the first place.

## ▶ THE PIECES, IN ORDER
1. **Unpack tier0+tier1 to app storage on first run**, and load the map page from a LOCAL origin.
2. **Move the HFDL/spot fetches into RN**, posting the (small) results into the page.
3. **The tier2 + relief download**, into the same directory.
4. **The storage note and Remove** at the foot of the directory.

## ✗ WHAT WENT WRONG THE FIRST TIME, SO IT IS NOT REPEATED
- ✗ The bridge carried ~2.6 MB per layer (`tier1-roads`, `tier1-rail`). The agent that built it
  flagged the first-paint latency as UNMEASURED and I shipped it on that basis.
- ✗ Symptom Stuart saw: *"one second the map will be shown and then it will move slightly and the
  whole thing disappears just leaving floating city labels."* That is a THROW PART WAY THROUGH A
  REDRAW — layers added before it survive, everything after is never drawn — the same shape as the
  `bringToFront` TypeError that once killed the landcover. A WebView has no console, so an
  on-screen error box was added (`window.__mdErr`) rather than guessing again.
- ★ Relief was never embedded (~45 MB), which is why the app looks flatter than the directory. That
  is the tier2 download's job, not a bug.
