# BRIEF: `sdr://` URI Scheme Association (SpyServer one-tap connect)

**Project:** VibeSDR (React Native / Expo — built from native projects, NO `expo prebuild --clean`)
**Author:** Stuart Carr (Stuey3D)
**Branch:** `experimental` (builds on the SpyServer backend + `vibesdr://` deep linking already merged there)
**Status:** Ready for implementation

---

## 1. Goal

Register the de-facto SpyServer URI scheme `sdr://` (as used by SDR#, gqrx, and the Airspy directory at <https://airspy.com/directory/>) so that VibeSDR becomes an OS-level handler for it. Tapping an `sdr://host:port` link anywhere — Discord, Reddit, forums, a browser — opens VibeSDR and connects to that SpyServer via the existing `connectSpy()` path.

**Product context (why, and why this shape):** SpyServer support exists but the public directory was deliberately removed from the instance picker — too many listed servers are dead or unreliable, and we are NOT advertising SpyServer as a feature. This brief is the compromise: no directory in the app, but if a user finds a server themselves (Airspy's map, a friend, a forum post), the path from link to listening is one tap or one paste. Discovery stays external; VibeSDR just honours the link.

**Known caveat (do not "fix"):** the Airspy directory map may present its `sdr://` URLs as copy-to-clipboard text rather than tappable anchors. That is why §4.4 (paste support in the manual connect panel) is IN scope and not optional — copy → open VibeSDR → paste must work even where tapping cannot.

---

## 2. URL Grammar

```
sdr://<host>[:<port>]
```

| Part | Required | Format | Notes |
|------|----------|--------|-------|
| `host` | yes | hostname or IPv4 literal | Charset `[A-Za-z0-9.-]`, 1–253 chars, no leading/trailing dot or hyphen-only labels. Bracketed IPv6 is **rejected gracefully** in Phase 1 (toast, no-op). |
| `port` | no | integer 1–65535 | **Defaults to 5555 when omitted** — the Airspy list omits it for default-port servers. |

No path, query, or fragment. If any are present after the authority, ignore them silently (some sites append tracking junk); do not fail the parse because of them. Total URL length cap 2048 (match `MAX_URL_LEN`).

Examples that MUST parse:

```
sdr://k2zn.ddns.net:5555      → host k2zn.ddns.net, port 5555
sdr://92.51.53.87             → host 92.51.53.87, port 5555 (default)
SDR://Example.COM:5000        → scheme case-insensitive; host lowercased
```

Examples that MUST be rejected (toast "Invalid SpyServer link", never crash, never reflect raw input in the toast):

```
sdr://                        (no host)
sdr://host:99999              (port out of range)
sdr://[2001:db8::1]:5555      (IPv6 — Phase 1 out of scope)
sdr://host:5555/../../etc     (junk after authority is ignored, but a host containing / % or whitespace is rejected)
```

---

## 3. Scope

### In scope

1. Register `sdr` scheme natively on both platforms (§4.1, §4.2) + keep `app.json` in sync (§4.3).
2. Parser `parseSdrUrl()` in the linking layer (§4.5).
3. Wire into `useDeepLinks` cold/warm-start flow with a **mandatory confirmation dialog** (§4.6).
4. Route into the existing `connectSpy()` via a new `autoSpy` route param on InstancePicker (§4.7).
5. Paste support: manual TCP/SpyServer connect field accepts a full `sdr://host:port` string (§4.4).

### Out of scope — do NOT implement

- ❌ Any SpyServer directory, server list, or discovery UI. The public directory card stays hidden (commit `6929733`).
- ❌ Advertising SpyServer anywhere new in the UI, README, or store listings.
- ❌ Emitting/sharing `sdr://` links from VibeSDR's share button (future brief if ever).
- ❌ Registering `spyserver://` as an OS scheme. (`spyserver://host:port` remains an internal pseudo-URL for favourites plumbing only — see `instancesApi.ts` comment. The paste parser MAY tolerate it, §4.4.)
- ❌ Universal Links / App Links for `airspy.com` — not our domain, not possible.
- ❌ IPv6 literal hosts.
- ❌ Any changes to the `vibesdr://` grammar, `parseVibeSdrUrl()`, or `resolveRequest()` semantics.
- ❌ New native modules. The `sdr://` path reuses `NativeModules.VibeLocalSDR.startSpyServer` untouched.

---

## 4. Implementation Plan

### 4.1 Android — `android/app/src/main/AndroidManifest.xml`

Add a sibling intent-filter next to the existing `vibesdr` one on MainActivity (keep them as **separate** intent-filters; do not add a second `<data>` to the existing one, which would cross-product the schemes with any future host rules):

```xml
<!-- sdr:// SpyServer links (Airspy directory / SDR# convention) -->
<intent-filter>
  <action android:name="android.intent.action.VIEW"/>
  <category android:name="android.intent.category.DEFAULT"/>
  <category android:name="android.intent.category.BROWSABLE"/>
  <data android:scheme="sdr"/>
</intent-filter>
```

MainActivity is already `exported` (it has the LAUNCHER filter), so no attribute changes. If another installed app also claims `sdr://` (e.g. SDR Touch), Android shows a disambiguation chooser — correct behaviour, nothing to do.

### 4.2 iOS — `ios/VibeSDR/Info.plist`

Append `sdr` to the existing `CFBundleURLSchemes` array (alongside `com.vibesdr.app` and `vibesdr`). One line. Note for release planning: this ships in the **next** binary — it cannot be added to the build currently in App Store review. Scheme name policing is not a rejection risk.

### 4.3 Expo config — `app.json`

Change `"scheme": "vibesdr"` → `"scheme": ["vibesdr", "sdr"]`. This is documentation/consistency only (we build from the native projects), but keeps anyone running config plugins later from silently dropping the scheme.

### 4.4 Paste support — `InstancePickerScreen.tsx`, `parseTcpEntry()`

Before the existing host/port split, strip a recognised scheme prefix and let it drive the proto toggle:

- If the host field value matches `/^(sdr|spyserver):\/\//i` → strip the prefix, and if `tcpProto !== 'spyserver'`, call `setTcpProto('spyserver')` and default the port field to 5555 (mirror the existing toggle handler's behaviour).
- Then proceed with the current `host[:port]` parsing unchanged.

Result: user copies `sdr://host:5555` from the Airspy map, opens the manual connect panel, pastes into the host field, taps connect. Zero extra UI.

### 4.5 Parser — `src/linking/SdrLinkHandler.ts` (new file)

Keep it out of `DeepLinkHandler.ts` — that file's header documents itself as the `vibesdr://` contract with Nathan and shouldn't grow a second grammar. New module, same style (pure logic, no UI):

```ts
export interface SdrLinkTarget { host: string; port: number }
export function parseSdrUrl(raw: string): SdrLinkTarget | null
```

Rules per §2: length cap, scheme case-insensitive, host charset/length validation, host lowercased, port default 5555, port range check, silently drop anything after the authority. Return `null` on any failure — caller toasts a fixed string, never the input.

### 4.6 Routing — `src/linking/useDeepLinks.ts`

1. `handle()`: widen the accept filter from `/^vibesdr:\/\//i` to `/^(vibesdr|sdr):\/\//i`. Dedup + queue-until-ready + `markDeepLinkActive()` logic applies identically — an `sdr://` cold start must also suppress the picker's default-instance auto-connect (the `deepLinkState` race documentation applies verbatim).
2. `process()`: branch on scheme.
   - `vibesdr://` → existing path, untouched.
   - `sdr://` → `parseSdrUrl()`. On null: toast `'Invalid SpyServer link'`, return. On success: **always** show a confirmation alert — not only when on the SDR screen. Rationale: `sdr://` links originate from arbitrary third parties and connecting opens a raw TCP socket from the user's device; the user must see host:port before we dial it.

     ```
     Title:  'Connect to SpyServer?'
     Body:   '<host>:<port>\n\nSpyServer links come from third-party sources — only connect to servers you trust.'
             (+ '\nThis will disconnect your current session.' when current route is 'SDR')
     Buttons: Cancel (style: cancel) / Connect
     ```

     Host and port here are the **validated, parsed** values, never the raw URL.
3. On Connect: navigate with a stack reset (same `CommonActions.reset` pattern as `goToTarget`) to:

   ```ts
   routes: [{ name: 'InstancePicker', params: { autoSpy: { host, port } } }]
   ```

   Single-route reset — the SDR screen is pushed by `connectSpy()` itself, because the native shim must start (and can fail) before navigation. Do NOT try to synthesise a `ResolvedTarget` / reuse `goToTarget`; the resolve pipeline intentionally rejects non-URL backends (see the `rtltcp` rejection comment in `DeepLinkHandler.ts`) and SpyServer navigation params (`localPort`, `wsBaseUrl`, `localGen`, …) only exist after the shim answers.

### 4.7 Auto-connect — `InstancePickerScreen.tsx`

Accept optional route param `autoSpy?: { host: string; port: number }`. In an effect, once the screen is mounted and not already `connecting`:

```ts
connectSpy(autoSpy.host, autoSpy.port, `${autoSpy.host}:${autoSpy.port}`);
```

- Fire **once** per param delivery (guard with a ref or clear the param via `navigation.setParams`), so a failed connect leaves the user on the picker without a retry loop — `connectSpy`'s existing failure `Alert` is the error UX, unchanged.
- No interaction with the default-instance auto-connect is needed beyond what `markDeepLinkActive()` already suppresses, but verify ordering against `whenInitialLinkChecked()` on cold start (§5).
- `connectSpy` itself: zero changes.

---

## 5. Cold Start / Warm Start Matrix

| Scenario | Expected |
|---|---|
| App closed, tap `sdr://host:5555` | App launches → splash → confirm dialog → Connect → picker mounts with `autoSpy` → shim starts → SDR screen. Default instance auto-connect MUST NOT fire (deepLinkState flag + `whenInitialLinkChecked` gate). |
| App on picker, tap link | Confirm dialog → connect in place. |
| App on an SDR session, tap link | Confirm dialog including the "disconnect current session" line → reset → connect. Cancel = session untouched. |
| Same link tapped twice < 2 s | Second delivery deduped (existing `lastUrl`/`lastAt` window). |
| Shim connect fails (dead server — the common case for Airspy-listed servers) | Existing `connectSpy` Alert ('Could not connect to host:port'), user left on picker. No crash, no retry loop. |
| Link arrives before `ready` | Queued in `pending`, drained once ready (existing mechanism). |

---

## 6. Security / Validation

- All `sdr://` input is untrusted. Validation lives entirely in `parseSdrUrl()`; downstream code only ever sees a validated `{host, port}`.
- Never auto-connect without the confirmation dialog (§4.6). This is deliberately stricter than `vibesdr://` (which auto-connects on cold start): `vibesdr://` targets pass through UUID/collector resolution or an https/wss allowlist, whereas `sdr://` dials an arbitrary raw TCP host.
- Never reflect the raw URL in any toast/alert — fixed strings + validated fields only (matches the existing handler's stated policy).
- No scheme changes to what the shim receives: `startSpyServer({ host, port, … })` exactly as the manual path sends today.

---

## 7. Testing Plan

**Parser unit tests** (`SdrLinkHandler.test.ts`, mirror existing test style): every example in §2, plus length-cap, whitespace host, `%`-containing host, port `0`, port `65536`, uppercase scheme, trailing slash, trailing `?junk#frag`.

**Android (device or emulator):**

```
adb shell am start -a android.intent.action.VIEW -d "sdr://<known-good-host>:5555"
adb shell am start -a android.intent.action.VIEW -d "sdr://<known-good-host>"        # default port
adb shell am start -a android.intent.action.VIEW -d "sdr://bad host:5555"           # reject toast
```

Cold start (app swiped away) and warm start for each. Also tap a real `sdr://` anchor from a test page in Chrome, and from a Discord message.

**iOS:**

```
xcrun simctl openurl booted "sdr://<known-good-host>:5555"
```

Plus on-device: tap an `sdr://` anchor in Safari (expect the "Open in VibeSDR?" sheet), cold and warm.

**Paste path:** copy `sdr://host:5555` → manual connect panel → paste in host field → proto flips to SpyServer, port fills 5555, connect works. Also paste bare `host:5555` (regression: existing behaviour unchanged) and `spyserver://host:5555`.

**Regression:** full `vibesdr://` matrix from BRIEF-deep-linking-uri-scheme §7 still passes — especially the cold-start default-auto-connect race, since `handle()`'s filter is the one shared line touched.

**Devices:** iPhone 17 Pro Max, iPhone SE, Motorola G35, Galaxy Tab A9.

---

## 8. Future Work (separate briefs — do not implement now)

- Emit `sdr://` from the share sheet when connected to a SpyServer.
- IPv6 literal support if anyone ever asks.
- QR: an `sdr://` QR scanned by the phone camera already routes through `Linking` for free once the scheme is registered — verify, but no in-app scanner work.
