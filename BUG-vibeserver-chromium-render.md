# ★ BUG (PRIORITY 1): the web client is unusable on Chromium

**Raised:** Stuart Carr, 2026-07-22 — *"this is the biggest vibeserver bug that needs fixing as
priority 1"*
**Status:** ★ FIXED — and the cause was OUR OWN CODE, added the same day.

**The culprit: the silent looping `<audio>` "anchor"** added hours earlier so Chromium would show
Now Playing (it refuses to attach Global Media Controls to a MediaStream `srcObject`). Chromium
appears to re-decode the clip on every loop and never release it — 3,600 loops an hour.

MEASURED, same machine, same page, same server:

| | With anchor | Without |
|---|---|---|
| Edge CPU | 38% | **3.1%** |
| Edge memory | climbing ~700 MB/s, peaked **13.1 GB** | flat |
| CPU temp | 89°C | 56°C |

3.1% is LOWER than Safari's 4.9%. Everything else reported — the heat, the hangs, the stall when
opening a tab, the browser becoming unusable — was downstream of the leak and went with it.

★★ THE LESSON WORTH KEEPING: the JS heap stayed at **5 MB** throughout. A leak of 700 MB/s was
completely invisible to every JavaScript-level measurement, because the memory was in the browser's
media pipeline, not in our objects. The self-profiler is what proved our drawing was free (0% of
wall on Chromium) and turned "it must be our rendering" into "it cannot be our rendering" — which
is what finally pointed at a subsystem rather than a draw call.

Now Playing on Chromium is therefore OFF by default. It is a nicety; the leak was catastrophic. The
anchor survives behind `#anchor` for experimenting with a leak-free version (now served as a blob
URL rather than a data: URI — UNVERIFIED). Any retest must watch memory for MINUTES, not seconds.

---

## Why this outranks everything else

Chromium is Chrome, Edge, and effectively every Android browser. It is the majority of anyone who
will ever open a VibeServer. **A web client that only behaves in Safari is broken, not quirky** —
and it undermines the whole "VibeServer is also a desktop SDR" story, because most people's desktop
is not a Mac.

## What was measured (M4 MacBook Air, same page, same server, same 96.6 MHz WFM)

| | Edge | Safari |
|---|---|---|
| Browser process CPU | **38.7%** | 4.9% |
| CPU clock / temp | 3.74 GHz / **89°C** | 2.23 GHz / 59°C |
| GPU | 0.62 GHz @ **89%** | 0.58 GHz @ 66% |
| Data rate | ~121 KB/s | ~123 KB/s |

Also reported on Edge: **long hangs**, the browser becoming unusable, a stall when opening a new tab
in the same window, and audio pitch wobbling for a few seconds at start. The same audio wobble was
seen previously against the ANDROID server on a work laptop, so this is not macOS-specific.

★ Both CPU **and** GPU are high, and both clocks rise. That rules out a software-rasterisation
fallback (which would leave the GPU idle) — Chromium is doing genuinely more work, not different
work.

## ★ The diagnostic that actually told us something

**At 5 fps the CPU was unchanged (~38%).** The render loop ran at 60 fps via `requestAnimationFrame`
regardless of the incoming frame rate, so lowering the server's rate saved the SERVER work and the
client none at all. The cost is per-RENDER, not per-FRAME-RECEIVED.

## Tried, measured, did not fix — kept anyway where they stand on their own merits

1. **Waterfall self-copy → ring buffer.** It scrolled by `drawImage(canvas, 0, 1)` onto itself every
   frame. Genuinely wrong and now fixed, but not the cause.
2. **Scale + band strip redrawn 60×/s.** Both render TEXT and only change on tune/zoom/resize. Now
   redrawn on a view key. Also right, also not the cause.
3. **Render loop tied to the chosen rate** (3× the waterfall rate, floored 20, capped 60). Correct,
   and it makes the listener's rate control mean something — but the numbers stayed high.
4. **`desynchronized: true`** on the visible canvas — no effect, REVERTED.
5. **MediaStream → destination for audio** on Chromium (it refuses Global Media Controls for
   `srcObject` anyway). Fixed Now Playing and removed the WebRTC playout path; did not fix CPU.

## Next steps, in order

1. **A PROFILE, not another hypothesis.** Edge DevTools → Performance → record ~5s → Bottom-Up
   sorted by Self Time. That names the function burning the CPU instead of guessing at it. Blocked
   so far because the browser is too unresponsive to drive the profiler.
2. **Cheap decisive test:** set RESOLUTION to STANDARD (dpr 1 = a QUARTER of the pixels). If the
   numbers collapse, the problem is fill-rate/pixel count and the rewrite below will certainly fix
   it. If they do not move, it is elsewhere and a rewrite might not help — worth knowing BEFORE
   committing to one.
3. **The likely real fix: move the waterfall to WebGL.** A 2D canvas repeatedly blitting megapixels
   is the wrong tool, and it is exactly what the phone app already avoids — it uses an SkSL GPU
   shader (see the `project_sksl_waterfall` note). The web equivalent: keep history in a ring
   TEXTURE, upload each new row with `texSubImage2D` (one row, not a canvas), and draw one
   full-screen quad whose shader applies the scroll offset and the palette. Per frame that is a tiny
   upload plus one quad, instead of blits and readbacks. It should collapse the cost on every
   browser, not just Chromium.
4. Re-test the hangs and the audio wobble afterwards — they may be downstream of GPU stalls rather
   than separate faults.

## Workaround for now

Safari on macOS is unaffected (4.9%). There is no workaround on Windows or Android, which is
precisely why this is priority 1.
