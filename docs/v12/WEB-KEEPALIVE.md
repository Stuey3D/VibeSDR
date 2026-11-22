# WEB KEEP-ALIVE — a hidden, silent tab keeps its session (beta91, branch b91-keepalive)

## RESUME HERE

- **Built and committed** on `b91-keepalive` (off v12-experimental 691508071): the page's 5 s ping now runs on a
  **Worker's clock** (`web/client/src/keepAlive.ts`); the **audio Worker pings its own socket** every 10 s; the spectrum
  watchdog **re-arms instead of closing** when its own check came late (the page was throttled or suspended); and while
  **MUTED** an inaudible presence (a 2^-15 DC constant, −90 dBFS) goes to the output beside the gain.
- **Not committed:** `android/app/src/main/cpp/vibe_web_page.h` — rebuilt locally for the tests. The coordinator
  rebuilds the page once at merge (`node scripts/build-web.mjs` via the lock), as for every beta91 branch.
- **Server unchanged.** The 30 s rule (`kAppLivenessMs`) is right; the page was the side that broke it.
- **Owed: Stuart's live check in Safari** — see "Live check" below. No WebKit engine can be driven silently on this Mac
  (no Playwright WebKit; safaridriver opens a visible, audible Safari window), so the Safari half is from WebKit's
  source, and the automated tests run in headless Edge.

## The fault (Stuart's Pi 500, 2026-11-22, Safari, LAN and Ethernet)

The page froze and reconnected about once a minute. The V4L worker logged, each minute (14:42 → 14:46):
`spectrum WS: no application message from session 85ba3a02 for 30 s (proto 1) — releasing the dead session`, then
`closed by peer`, `held session … returned after 3 s`. Each reconnect was followed at once by `client asked for 5 fps`
— the page's hidden-tab rate (`throttleHidden()` on `visibilitychange`), so the page **was hidden** at every drop
(Stuart was in Discord / screenshot tools). The speaker was muted by the silent-decoder rule (APRS, METEOR LRPT —
`setMuted`, the gain at 0). The waterfall "bands" he saw are the hidden spells at 5 fps and the reconnect gaps.

The server releases a proto ≥ 1 session that sends no **application** message (a text frame) on any of its sockets for
30 s — the rule that frees single-occupant slots a forgotten Jr or watch used to hold. A pong does not count, by design.
The page's only application message while nobody touches it is the ping, and that ran on `window.setInterval(…, 5000)`.

## What the browsers actually do (cited)

### Safari / WebKit (source: github.com/WebKit/WebKit, main, read 2026-11-22)

- `Source/WebCore/page/Page.cpp` `Page::updateTimerThrottlingState()`: throttling is **off** unless the page is
  `IsVisuallyIdle` (hidden / occluded). Then, if `hiddenPageDOMTimerThrottlingAutoIncreases` is off **or** the page is
  `IsVisible | IsAudible | IsLoading | IsCapturingMedia`, the state is `Enabled` — a **1 s** alignment
  (`DOMTimer::hiddenPageAlignmentInterval() = 1_s`, `DOMTimer.h`). Otherwise `EnabledIncreasing`:
  `Page::updateDOMTimerAlignmentInterval()` sets the alignment to *the time since throttling began*, re-armed after the
  same again ("alignment interval increases exponentially"), up to the client's limit
  (`setDOMTimerAlignmentIntervalIncreaseLimit`). WebKit's own default for AutoIncreases is false
  (`UnifiedWebPreferences.yaml`); Safari turns it on (`WKPreferences _setHiddenPageDOMTimerThrottlingAutoIncreases:`) —
  the 30 s gaps in Stuart's log are that state. ★ So a hidden page that is **not audible** has its window timers
  stretched to a minute and beyond; an audible one keeps 1 s.
- **Audible** for Web Audio: `Modules/webaudio/DefaultAudioDestinationNode.cpp` `render()` →
  `setIsSilent(destinationBus.isSilent())`, and `updateIsEffectivelyPlayingAudio()` needs `!m_isSilent`. The bus's
  silent flag is a FLAG (`platform/audio/AudioChannel.h`), set by `zero()`: `GainNode::process` zeroes its output when
  the gain is 0 ("If the gain is 0 just zero the bus"). So **a muted Web Audio graph is silent to WebKit**.
  `ConstantSourceNode::process` with a non-zero offset fills the bus and calls `clearSilentFlag()` — not silent, whatever
  the level. The page-level mute (`m_muted`) is applied after `setIsSilent` and does not change it.
- **Audible** for a media element: `html/HTMLMediaElement.cpp` `mediaState()` → `IsPlayingAudio` when playing and
  `computeCanProduceAudio()`: not suspended, `volume() > 0`, not `muted()`, has audio. **Content is not inspected.**
  The Safari output path here is gain → `MediaStreamAudioDestinationNode` → `<audio srcObject>` (audio.ts
  `_useMediaStream`), so by the source the gain mute should have left Safari's tab audible. Stuart's log says it was not
  — something else took the element out (a tab mute in Safari, a paused element, or a WebKit path not read here). This
  is the one thing the live check settles; the fix below does not depend on it.
- **Workers:** `dom/ScriptExecutionContext.cpp` `domTimerAlignmentInterval()` returns
  `DOMTimer::defaultAlignmentInterval()` = **0**; the hidden-page alignment is the Document's (it reads the Page's).
  `workers/WorkerGlobalScope.cpp` does not override it. So **a dedicated Worker's timers are not hidden-page throttled.**
- **App Nap:** `WebKit/WebProcess/WebPage/WebPage.cpp` `isThrottleable()` — App Nap may engage when the page is not
  loading / audible / capturing / window-active and is visually idle. App Nap coalesces the process's timers (Worker
  ones included) but is not the minute-scale stretch; the 5 s Worker ping has 25 s of slack against the 30 s rule.
  Not measured on Safari here — the live check covers it.
- **iOS** suspends background WebContent processes outright; nothing in a page survives that, and the session is
  released after 30 s exactly as for a Jr — intended.

### Chromium (Edge, Chrome)

Hidden pages: timers at most once a second; after 5 min hidden, silent for 30 s, chain count ≥ 5 and no WebRTC,
**intensive throttling** — once a minute (developer.chrome.com/blog/timer-throttling-in-chrome-88; "a silent audio track
doesn't count"). The post does not mention workers; the throttles are the page's frame scheduler's, and dedicated
workers have their own — not verified from Chromium source here, and the test models it that way. The post's test
command carries an `AllowAggressiveThrottlingWithWebSocket` flag, i.e. a page with an open WebSocket may be kept out
of intensive throttling by default — so Chromium may never have dropped this page; Safari is the case that did. An
inaudible signal does not make a Chromium tab "noisy" — the Worker ping is what covers Chromium, not the presence.

### Firefox

Background-tab timer budget throttling for main-thread timers; workers not throttled the same way. Covered by the Worker
ping; not separately tested.

## What changed

1. `web/client/src/keepAlive.ts` (new): `heartbeat(everyMs, fn)` — one Blob Worker per page ticking every 1 s, each
   subscriber run when its period is up (measured on `performance.now()`; a late tick sends one heartbeat, not a burst).
   Falls back to `window.setInterval` if a Worker cannot start or errors (never worse than before).
2. `web/client/src/spectrum.ts`: the 5 s ping uses `heartbeat(5000)` (guarded to the socket that opened it).
   The 2 s watchdog: a check that comes **more than 5 s after the one before** ran in a throttled or suspended page and
   proves nothing about the socket — it re-arms (`lastRxAt` no older than 5 s) and the next on-time check decides.
   ★ Consequence, accepted: while a tab is hidden and throttled, a truly dead socket is found when it is shown again
   (≤ 15 s), not while hidden; reconnecting a hidden tab buys nothing.
3. `web/client/src/audio.ts` WORKER_SRC: the audio Worker (it owns the audio socket when worklet + WebCodecs exist) sends
   `{"type":"ping"}` every 10 s on its socket — liveness that needs no main thread at all. The server answers `pong`,
   which the Worker ignores. The server counts the newest application message on **any** socket of the session.
4. `web/client/src/audio.ts` `_syncPresence()`: while **muted** (and not quiet) a `ConstantSourceNode` at 2^-15 is
   connected to the output (`streamDest` or `ctx.destination`) beside the gain. One 16-bit step of DC — no speaker
   reproduces it, never audible. It keeps WebKit's Web Audio destination from being flagged silent (the
   ctx.destination path: Chromium, Firefox, Safari `#nomediastream`). Stopped on unmute, on quiet, on close.
   ★★ **Not in the quiet modes** (ADS-B, ISM, AIS, NDB, datalink, sondes — `setQuiet`): there the output is released on
   purpose because the AirPods' automatic switching followed a page "playing" silence (Stuart, 2026-10-25). A running
   inaudible output is exactly that, so the brief's "keep it playing in audio-less modes" was **not** done — those modes
   are kept by the two Worker heartbeats, which need no audio. No new Now Playing controls: nothing new plays in a quiet
   mode, and while muted the same element that already played keeps playing.
5. Server: **unchanged.** `kAppLivenessMs = 30000` is right for web clients once they ping from a Worker.

## Tests

- `scripts/test-web-keepalive.mjs` (new) — real server, fake rtl_tcp, the built page in headless Edge `--mute-audio`.
  The page is put in Safari's hidden state: `document.hidden`/`visibilityState` 'hidden' with a `visibilitychange`
  (the page drops to its 5 fps idle rate), and the **main thread's** timers aligned to 60 s (each interval at most once a
  minute, timeouts to the next minute, animation frames held). Worker timers untouched — as both engines do. Two
  hidden spells of 100 s: NFM with the speaker muted (gain 0), and ISM (quiet output).
  `VIBESERVER_BIN=… node scripts/test-web-keepalive.mjs` · control: `EXPECT_DROP=1` against a server with the old page.
- `scripts/soak-web-satellite.mjs` (new) — SATELLITE mode soak: server under ISS, pick with AUTO DOPPLER, ALL
  SATELLITES unfolded; longest task, timer lateness, ping gaps, JS heap, DOM nodes every 30 s.

Results: see the "Numbers" section (filled from the runs on 2026-11-22).

## Numbers

(filled below)

## Live check for Stuart (Safari, Pi 500)

1. Open the page, start the radio, open APRS (the speaker mutes), switch to Discord for 5 minutes. Come back: no
   reconnect, and the server log has no `no application message` for that session.
2. The same in ISM (nothing to hear) — and the AirPods must NOT be pulled over when switching to the Mac.
3. Optional: in Safari's Web Inspector (Develop → the page → Timelines), the Memory timeline over 10 min with the
   SATELLITE window open — the 1.2 GB WebContent is not JS heap in Chromium (5–11 MB there) and is worth one look
   (canvas backing stores at 2× DPR, decoded images).

## Optional, not built: the hidden spell on the waterfall

A hidden tab asks for 5 fps, so the time it was away draws as a thinner band of rows — which reads as a fault. What I
would do: on `visibilitychange` → visible, mark the boundary on the waterfall with one dim rule and a small "away 2:14"
label in the scale-stamp layer (the same layer the time stamps use), so the band is explained rather than hidden. Not
stretching rows to fake the missing time — that would draw signals that were never received.
