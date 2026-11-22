# Silent decoders — APRS and METEOR LRPT (beta91, 2026-11-22)

Stuart, 2026-11-22: *"i guess for meteor lrpt the audio needs muting too"*. APRS has been silent since 2026-11-10; Band
digital spots since 2026-10-27. LRPT now follows the same rule, and APRS and LRPT share ONE copy of it.

## The rule — `src/services/silentDecoder.ts` (`SilentMute`, `isSilentDecoder`)
- Muted on the way in; on the way out put back **exactly** as it was before (muted stays muted, unmuted comes back).
- A listener who unmutes meanwhile may — it is their speaker. Leaving still puts back the state from before the decoder.
- VFO B on: only the VFO the decoder runs on leaves the mix (the server's watch mute); the speaker carries the other VFO.
  B coming or going moves the mute between that VFO and the speaker.
- APRS → LRPT directly: the mute stays (no flicker); the state from before APRS comes back when the last one closes.
- The client's OWN mute — the web page's gain (`setMuted`, never `setQuiet`), the app's native mute in place (a VibeServer
  is external 'resume' mode: the socket stays, samples are dropped). The audio stream is never stopped: it keeps a
  background Safari tab alive (the ISM lesson) and is how the server counts the listener.
- PAGE (paging) is NOT here: its mute is the law's and cannot be undone while it runs (`prePagingMuteRef`).

## Where it runs
- **Web** `web/client/src/main.ts` `silentDecoderSync()` (was `aprsSilentSync`): from `syncDecButtons` (every decoder
  change, including a SATELLITE pick — `satFollow` sets `activeDec = 'lrpt'`) and from `abOnWatch` (B came or went).
  NFM ALL while APRS is open stays APRS's alone.
- **App** `src/screens/SDRScreen.tsx` the `silentMute` effect beside PAGE's: on `activeDecoder` (`satFollow → openDecoder`
  for LRPT), `abOn`, `decVfo` and every mute change. APRS was on PAGE's unbreakable mute before; it is now on this rule
  (the listener's unmute stands, A/B follows the decoding VFO — as the web page always did). `reassert`: a reconnect
  brings the audio back unmuted (every native start clears its mute), so a speaker unmuted by anything but the listener
  (`VibeMuted` false → `listenerUnmuted`) is muted again. The banner names the decoder and is the tap-to-unmute key.
- **R36S** `handheld-ui/`: no LRPT key (removed 2026-11-20 — SATELLITE needs orbit data and the R36S has no network), so
  nothing to do; its APRS mute is unchanged.

## Tested
- `scripts/test_silent_decoder.ts` — 40/0: the rule run every way (in/out, saved state, listener unmute, the app's
  reconnect re-mute, no speaker yet, APRS → LRPT, VFO B on A and on B, B off/on during LRPT, release) and both clients
  pinned to it in the sources. In `run-tests.sh`.
- Still green: test_paging_rec 39/0, test_ais_aprs_app 168/0, test_lrpt_app, test_satellites_app, test_satellite_mode 157,
  test_vfoAB 154/0. `tsc --noEmit` clean for the three changed files; the web page bundles (esbuild, via the lock).
- NOT run: a browser end-to-end of LRPT muting — LRPT needs a radio on 137 MHz and a SATELLITE pick, which the fake-RTL
  web tests (test-web-vfo-ab, HF) cannot reach; the web wiring is pinned in source instead.

## RESUME HERE
- Open: on iOS the app's mute pauses the player node in place; in the background iOS may then suspend the app (the
  long-standing caveat for Band / APRS / PAGE too — `checkLocalAudioAlive` on unmute). A whole LRPT pass in the
  background on an iPhone is the case to try on a real pass.
- Live check wanted (coordinator): a METEOR pass on a VibeServer — speaker muted from the METEOR pick (LRPT attached), back
  as it was after EXIT; with VFO B on, B plays on.
