# BRIEF: two VibeServer audio bugs — the app's NR/squelch/notch, and Opus on Edge

**Status:** not started. **10.0.1.** Both reported by Stuart 2026-07-31, both diagnosed.

---

## 1. ★★★ NR / SQUELCH / AUTO-NOTCH DO NOTHING FROM THE APP ON A VIBESERVER
> *"On the VibeSDR app connected to a VibeServer, the NR/Squelch/Auto-Notch controls don't do
> anything. They work on rtl_tcp so the controls are working, and they work in the web client so the
> server's controls work too. I suspect a command not wired in."*

★★ **Correct, and it is the DE-EMPHASIS BUG AGAIN** (fixed 2026-07-30 — see
[[vibeserver_wire_units_seconds]] and the adapter passthroughs). Why the symptom looks confusing:
- **rtl_tcp / local hardware** → the DSP is OURS, on-device, so the control works locally.
- **VibeServer / UberSDR** → the DSP is on the SERVER, so the control needs a WIRE COMMAND.

★★★ **The server implements all three, and its own comment says who they were for:**
```cpp
// local_sdr_shim.cpp:2437 — "…the on-device JS via JNI, so a REMOTE client (web or phone) had
// no way to touch them. Exposing them here gives BOTH remote clients the same audio controls
// the local app has — and keeps the DSP server-side, so the client stays a thin renderer."
```
**Built for both remote clients. Only the web one was ever connected.**

### The wire format, read from the server
```json
{"type":"squelch","db":<number>}          // db <= -100 means OFF (matches the app's own convention)
{"type":"nr","on":<bool>,"strength":0..1}
{"type":"notch","on":<bool>}
```

### The fix (small — the pattern already exists)
1. `UberSDRClient.ts` — three `_sendCtl` methods beside `setDeemph`/`setStereo` (line ~547).
   ★ **There is currently NO `setNr`, `setSquelch` or `setNotch` in the client at all.**
2. `UberSDRAdapter.ts` — three passthroughs. ★★ **THIS IS THE STEP THAT WAS MISSED LAST TIME**:
   `rc.setDeemph?.()` silently no-ops when the adapter lacks the method, so the fix LOOKS done and
   is not. Do not skip it.
3. `SDRScreen.tsx` — route the existing control handlers down the remote branch, exactly as
   `onHwDeemph`/`onHwStereo` were.

★ **Test on a VibeServer, not on rtl_tcp** — rtl_tcp passes without the fix, which is why this got
through.

---

## 2. ★★ OPUS AUDIO FAILS IN MS EDGE ON WINDOWS 11 (uncompressed works)
> *"On Windows MS-Edge I had to use uncompressed audio as the Opus audio didn't work. Windows 11,
> latest Edge."*

### ★★ A REAL MISMATCH — we probe STEREO and configure MONO
```ts
// audio.ts:468 — the pre-flight probe
AudioDecoder.isConfigSupported({ codec: 'opus', sampleRate: 48000, numberOfChannels: 2 })
// audio.ts:481 — what we actually configure
this.opusDec.configure({ codec: 'opus', sampleRate: 48000, numberOfChannels: ch });  // ch defaults to 1
```
★ So the capability we test is not the capability we use. Mono Opus is the likelier one for a
platform decoder to refuse, and **Edge on Windows routes through a different media stack than Chrome
on macOS** — a plausible culprit, unproven.

### ★★★ THE DEFECT THAT MATTERS REGARDLESS OF ROOT CAUSE — no RUNTIME fallback
```ts
error: (e) => console.warn('[audio] opus decode error', e),
```
**`isConfigSupported` is a PREDICTION, not a guarantee, and we treat it as final.** If configure or
decode fails at runtime the audio is silent for ever, the only evidence is a console warning nobody
sees, and the user must discover the uncompressed setting themselves — which is exactly what
happened here.

★★ **Fix:** on decode/configure error, fall back to PCM at runtime — ask the server for uncompressed
and carry on. We KNOW that works on the failing machine, because Stuart proved it by hand.
★ The existing comment claims *"the stream self-heals on the next key packet"*. True for ONE bad
packet; false when every packet fails. Correct the comment along with the code.

### ★ WHAT WOULD NAIL THE ROOT CAUSE
The Edge DevTools console on that Windows box, at the moment audio starts. The client logs
`[audio] requesting Opus (WebCodecs supported)` and then either `opus decode error` or
`opus enqueue failed`. **One line names the cause instead of choosing between three candidates.**
★ Fix the fallback either way — it is right even once the Windows cause is known.

★★ Related: [[web_media_controls_per_engine]] — Chromium and Safari already differ in ways that cost
us a release. Assume per-engine behaviour, and never treat a capability probe as proof.
