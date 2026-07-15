# BUGFIX — VibeServer web client reports squelch as an audio fault

**Status:** Ready to implement. Self-contained; no dependencies.
**Component:** `web/client` (VibeServer web client)
**Author:** Stuart Carr (Stuey3D), with Claude
**Size:** Small — three files, one new health state, one CSS rule.

---

## Symptom

With squelch enabled and no signal breaking the threshold, the signal meter is replaced by a
**pulsing red** banner reading:

```
NO SOUND — IS THE TAB MUTED?
```

The tab is not muted. The squelch is working exactly as designed. The warning is wrong,
visually alarming, and — worst of all — it **hides the S-meter**, which is the one thing you
want to watch while waiting for a signal to come up.

---

## Root cause — three parts

### 1. The audio engine does not know squelch exists

`web/client/src/audio.ts`:

```ts
get health(): 'ok' | 'suspended' | 'no-stream' | 'muted' | 'silent' {
  if (!this.ctx) return 'no-stream';
  if (this.suspended) return 'suspended';
  if (!this.streaming) return 'no-stream';
  if (this._muted) return 'muted';
  // Frames arriving, context running, not muted — but nothing has been heard
  // for a while. Most likely the browser is muting us at the tab level.
  if (this.lastAudibleAt && performance.now() - this.lastAudibleAt > 5000) return 'silent';
  return 'ok';
}
```

Squelch is applied **server-side** (`spec.setSquelch()` → server DSP), so from the audio
engine's point of view a closed squelch and a muted tab are indistinguishable: frames are
arriving, the context is running, and nothing is audible. It picks the wrong explanation
because it was never given the other one.

### 2. The message

`web/client/src/main.ts` (~line 824), in `updateStatus()`:

```ts
case 'silent': fault = 'NO SOUND — IS THE TAB MUTED?'; break;
```

### 3. The flashing is literal, and the meter is deliberately hidden

`web/client/index.html` (~line 230):

```css
#sig.fault { height: 1.6em; background: rgba(224,80,80,0.18); border: 1px solid var(--red); }
#sig.fault #sigFill, #sig.fault #sigPeak { display: none; }   /* ← meter vanishes */
#sigFault {
  ...
  color: var(--red);
  animation: recPulse 1.6s ease-in-out infinite;              /* ← the flashing */
}
```

Both behaviours are *correct for a genuine fault* and *wrong for squelch*. A fault should grab
attention and the meter is meaningless when audio is broken. Squelch is neither: it is expected
behaviour, and the meter is actively useful.

---

## Fix

Teach the audio engine the squelch **setting**, add a distinct `squelched` state, and render it
as **information, not a fault**: amber, steady, **meter stays visible**.

### `web/client/src/audio.ts`

```diff
   private _muted = false;
+  /** Server-side squelch threshold, dB. <= -100 means OFF (matches the app's convention). */
+  private _squelchDb = -100;
+  set squelchDb(db: number) { this._squelchDb = db; }
+  get squelchActive() { return this._squelchDb > -100; }
@@
-  get health(): 'ok' | 'suspended' | 'no-stream' | 'muted' | 'silent' {
+  get health(): 'ok' | 'suspended' | 'no-stream' | 'muted' | 'squelched' | 'silent' {
     if (!this.ctx) return 'no-stream';
     if (this.suspended) return 'suspended';
     if (!this.streaming) return 'no-stream';
     if (this._muted) return 'muted';
-    // Frames arriving, context running, not muted — but nothing has been heard
-    // for a while. Most likely the browser is muting us at the tab level.
-    if (this.lastAudibleAt && performance.now() - this.lastAudibleAt > 5000) return 'silent';
+    // Frames arriving, context running, not muted — but nothing heard for a while.
+    // If squelch is ARMED, that is the squelch doing its job, not a fault. Only with
+    // squelch OFF is silence genuinely suspicious, and then a tab-level mute is the
+    // likeliest cause (and one no in-page control can override).
+    if (this.lastAudibleAt && performance.now() - this.lastAudibleAt > 5000) {
+      return this.squelchActive ? 'squelched' : 'silent';
+    }
     return 'ok';
   }
```

### `web/client/src/main.ts` — `updateStatus()`

```diff
   let fault = '';
+  let info = '';
   switch (audio?.health) {
     case 'suspended': fault = 'AUDIO PAUSED — CLICK THE PAGE'; break;
     case 'no-stream': fault = 'AUDIO DISCONNECTED'; break;
     case 'silent':    fault = 'NO SOUND — IS THE TAB MUTED?'; break;
+    case 'squelched': info  = 'NO AUDIO — SQUELCH ENABLED'; break;
   }
-  $('sig').classList.toggle('fault', !!fault);
-  $('sigFault').textContent = fault;
-  $('sigFault').classList.toggle('show', !!fault);
+  // A fault replaces the meter and pulses, to grab attention. Squelch does neither:
+  // it is expected behaviour, and the meter is exactly what you want to watch while
+  // waiting for a signal to break the threshold.
+  $('sig').classList.toggle('fault', !!fault);
+  $('sig').classList.toggle('squelched', !fault && !!info);
+  $('sigFault').textContent = fault || info;
+  $('sigFault').classList.toggle('show', !!fault || !!info);
+  $('sigFault').classList.toggle('info', !fault && !!info);
```

### `web/client/src/main.ts` — keep the mirror in sync (BOTH places)

The squelch slider:

```diff
   slider('sql', 'sqlVal',
     (v) => (v <= -100 ? 'OFF' : `${v} dB`),
-    (v) => spec!.setSquelch(v),
+    (v) => { spec!.setSquelch(v); if (audio) audio.squelchDb = v; },
     'squelch');
```

And settings restore, `pushSettingsToServer()` — **easy to miss, and missing it means the bug
persists across a page reload with squelch already saved:**

```diff
-  const sql = num('squelch');       if (sql !== undefined) spec.setSquelch(sql);
+  const sql = num('squelch');
+  if (sql !== undefined) { spec.setSquelch(sql); if (audio) audio.squelchDb = sql; }
```

### `web/client/index.html`

```diff
 #sig.fault { height: 1.6em; background: rgba(224,80,80,0.18); border: 1px solid var(--red); }
 #sig.fault #sigFill, #sig.fault #sigPeak { display: none; }
+/* Squelch closed is not a fault. Amber, steady, and the meter STAYS VISIBLE so you can
+   watch a signal climbing towards the threshold — which is the whole point of squelch. */
+#sig.squelched { border: 1px solid var(--amber); }
 #sigFault {
   display: none; position: absolute; inset: 0; align-items: center; justify-content: center;
   color: var(--red); font-size: 0.75em; letter-spacing: 0.12em; white-space: nowrap;
   animation: recPulse 1.6s ease-in-out infinite;
 }
+#sigFault.info { color: var(--amber); animation: none; opacity: 0.85; }
 #sigFault.show { display: flex; }
```

---

## Acceptance criteria

1. Squelch enabled, no signal → **amber, steady** `NO AUDIO — SQUELCH ENABLED`. No pulsing.
2. **The S-meter remains visible** while squelched.
3. Squelch enabled and a signal breaks the threshold → message clears, audio plays.
4. Squelch **OFF** and the tab genuinely muted → the original red pulsing
   `NO SOUND — IS THE TAB MUTED?` still appears. **No regression** — this warning was correct,
   it was just being shown in the wrong circumstances.
5. Reload the page with squelch already saved in prefs → shows the squelch message, **not** the
   tab-mute one. (This is the `pushSettingsToServer()` path; forget it and the bug survives a
   refresh.)
6. `AUDIO PAUSED` and `AUDIO DISCONNECTED` unchanged.

---

## Notes for whoever picks this up

**`_squelchDb` is a cache of a *setting*, not live gate state.** The client cannot know whether
the server's squelch gate is open or shut right now — only that it is armed. That is sufficient
for this fix, but it means the message appears after the 5-second silence timeout rather than
immediately.

**`// TODO` — the proper fix is a protocol event.** If VibeServer gains a server→client
`squelch-open` / `squelch-closed` message, the 5-second timeout can be dropped entirely and the
indication becomes instant. **Worth designing now rather than later:** the airband scanner
(see `BRIEF-airband-geo-frequencies-v2.md` §8) needs carrier-open events anyway, so this is one
protocol addition serving two features. Coordinate with the VibeServer protocol brief.

**Check the native RN client for the same bug.** If this "no audio" health logic was ported
across to the React Native app, it will have the identical false positive — and **squelch is far
more likely to be enabled in the native app than in the web one**, so the blast radius is
bigger. It may well be sitting there unreported precisely because it only fires when the user is
doing the thing that makes the warning look plausible.
