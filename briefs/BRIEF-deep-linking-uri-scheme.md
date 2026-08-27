# BRIEF: Custom URI Scheme Deep Linking (`vibesdr://`)

**Project:** VibeSDR (React Native / Expo)
**Author:** Stuart Carr (Stuey3D)
**Collaborator:** Nathan (MadPsy / M9PSY) — UberSDR host-side QR/link generation
**Status:** Draft for sign-off — URL grammar below is the contract between app and UberSDR overlay. Nathan: please review §2 before either side implements.

---

## 1. Goal

Register a custom URI scheme `vibesdr://` so that tapping a link (or scanning a QR code) opens VibeSDR and connects it to a specific SDR instance — optionally restoring frequency, mode, and zoom.

Primary use case (Nathan's proposal): an UberSDR instance displays a QR code and/or "Open in VibeSDR" button on its on-load overlay. The encoded URI carries the instance UUID; VibeSDR resolves the UUID via the instance collector it already fetches, then connects.

Secondary use case: VibeSDR's own **share button** gains the ability to emit a `vibesdr://` link alongside the existing web URL, so users can share a complete tuning state app-to-app.

---

## 2. URL Grammar (the contract)

### 2.1 Scheme and host

```
vibesdr://connect?<params>
```

`connect` is the only action in Phase 1. Unknown actions MUST be ignored gracefully (toast + no-op), never crash.

### 2.2 Parameters

| Param  | Required | Format | Meaning |
|--------|----------|--------|---------|
| `uuid` | one of `uuid`/`url` | UUIDv4, lowercase hex with dashes | UberSDR instance UUID, resolved via instance collector |
| `url`  | one of `uuid`/`url` | percent-encoded absolute URL (`https://` or `wss://`) | Direct backend address for non-collector backends |
| `backend` | required with `url`, ignored with `uuid` | `ubersdr` \| `kiwi` \| `owrx` \| `rtltcp` | Which adapter to use for a direct URL |
| `freq` | optional | integer, Hz | Frequency to tune after connect |
| `mode` | optional | `usb` \| `lsb` \| `cw` \| `cwr` \| `am` \| `sam` \| `fm` \| `nfm` \| `wfm` \| `iq` | Demodulator to select |
| `zoom` | optional | integer 0–14 | Waterfall zoom level |

### 2.3 Examples

```
vibesdr://connect?uuid=550e8400-e29b-41d4-a716-446655440000
vibesdr://connect?uuid=550e8400-e29b-41d4-a716-446655440000&freq=7074000&mode=usb
vibesdr://connect?url=wss%3A%2F%2Fkiwi.example.com%3A8073&backend=kiwi&freq=14074000&mode=usb
```

### 2.4 Rules

- If both `uuid` and `url` are present, `uuid` wins and `url` is ignored.
- `freq`/`mode`/`zoom` are applied **after** the connection is established and the backend reports ready. If the backend rejects the mode (unsupported), fall back to the backend's default and toast.
- All params are untrusted input — see §6.

**Nathan's side:** for Phase 1 the overlay only needs to emit the `uuid`-only form. `freq`/`mode` variants are optional extras if the overlay wants a "open current tuning in app" button later.

---

## 3. Scope

### In scope (Phase 1 — TypeScript + Expo config only)

1. Register `vibesdr` scheme in Expo config.
2. Deep link handler: parse, validate, resolve, connect.
3. Cold-start and warm-start handling.
4. In-app confirmation dialog when a link arrives while already connected.
5. Share button: add "Copy VibeSDR link" option emitting current instance + tuning state.
6. Graceful failure paths (unknown UUID, collector unreachable, malformed URL).

### Out of scope (explicitly — do NOT implement)

- ❌ Universal Links / Android App Links (needs hosted `apple-app-site-association` + `assetlinks.json`; future brief)
- ❌ QR code **scanning** inside VibeSDR (camera permissions; Nathan generates QR host-side, phone camera app handles scanning)
- ❌ QR code **generation** in the share sheet (possible later; link copy only for now)
- ❌ Any UberSDR host-side/overlay work (Nathan's side)
- ❌ New native modules — `expo-linking` only

---

## 4. Implementation Plan

### 4.1 `app.config.ts` / `app.json`

```jsonc
{
  "expo": {
    "scheme": "vibesdr"
  }
}
```

⚠️ **This is native config.** Requires a new prebuild / dev client — the scheme will NOT work in an existing dev build. Rebuild both iOS and Android dev clients before testing.

### 4.2 New file: `src/linking/DeepLinkHandler.ts`

Single module owning all deep-link logic:

- `parseVibeSdrUrl(url: string): DeepLinkRequest | null`
  - Uses `Linking.parse()` from `expo-linking`
  - Validates action === `connect`
  - Validates UUID against `/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/`
  - Validates `url` param is `https:`/`wss:` only; validates `backend` against known adapter IDs
  - Clamps `freq` to sane range (0 – 2 GHz), `zoom` to 0–14, `mode` to whitelist
  - Returns `null` on any validation failure (caller toasts "Invalid VibeSDR link")

- `resolveUuid(uuid: string): Promise<InstanceInfo | null>`
  - Looks up UUID in the already-fetched instance collector data
  - If collector data is stale/absent, trigger a fresh fetch, then retry once
  - `null` → toast "Instance not found in collector"

- `executeDeepLink(req: DeepLinkRequest): Promise<void>`
  - If currently connected to a different instance → show confirmation dialog ("Switch to <instance name>?") before disconnecting
  - If currently connected to the **same** instance → skip reconnect, just apply `freq`/`mode`/`zoom`
  - Drives the existing connection store — no new connection code paths

### 4.3 New hook: `src/linking/useDeepLinks.ts`

Mounted once at app root:

```ts
// Cold start
const initialUrl = await Linking.getInitialURL();

// Warm start
Linking.addEventListener('url', ({ url }) => ...);
```

**Cold-start ordering (important):** the deep link must be **queued** until the app's stores and audio stack report initialised. Add a simple `pendingDeepLink` ref that is drained once the app reaches its ready state. Do not attempt to connect during the splash/init phase.

**Dedup:** `getInitialURL()` and the event listener can both fire for the same URL on some Android launch paths — track the last-handled URL + timestamp and ignore duplicates within ~2s.

### 4.4 Confirmation dialog

Reuse the app's existing modal/dialog component. Copy:

> **Open instance from link?**
> Connect to *<instance name / host>*?
> This will disconnect your current session.
> [Cancel] [Connect]

Skip the dialog when the app is cold-starting (nothing to interrupt) — connect directly.

### 4.5 Share button extension

In the existing share flow, add a second option:

- "Copy web link" (existing behaviour, unchanged)
- "Copy VibeSDR link" → builds `vibesdr://connect?uuid=...&freq=...&mode=...&zoom=...` from current session state. Only offered when the current backend has a collector UUID (UberSDR); for other backends emit the `url`+`backend` form.

### 4.6 Expected file changes

| File | Change |
|------|--------|
| `app.config.ts` | Add `scheme: "vibesdr"` |
| `src/linking/DeepLinkHandler.ts` | **New** — parse/validate/resolve/execute |
| `src/linking/useDeepLinks.ts` | **New** — cold/warm start wiring + queue |
| App root component | Mount `useDeepLinks()` |
| Share flow component | Add "Copy VibeSDR link" option |
| Connection store | Expose (if not already) a single `connectToInstance()` entry point the handler can call |

No changes to native Swift/Kotlin code. No changes to audio stack, waterfall, or adapters.

---

## 5. Cold Start / Warm Start Matrix

| State | Behaviour |
|-------|-----------|
| App not running, link tapped | Launch → queue link → drain after init → connect (no dialog) |
| App backgrounded, not connected | Foreground → connect (no dialog) |
| App running, connected to same instance | Apply `freq`/`mode`/`zoom` only, toast "Tuned via link" |
| App running, connected to different instance | Confirmation dialog → connect on accept |
| App not installed | Link fails silently (platform behaviour) — accepted limitation, Universal Links fix this later |

---

## 6. Security / Validation

Incoming URLs are **untrusted input**:

- UUID path: only connect to hosts resolved from the instance collector. Never treat the UUID as a hostname.
- URL path: `https:`/`wss:` schemes only; reject anything else including `http:`/`ws:` (decide: allow plaintext for LAN rtl_tcp? → **Phase 1: reject**, revisit if users complain).
- Length-cap the whole URL (e.g. 2 KB) before parsing.
- Never surface raw URL contents in error toasts (avoid reflected junk); use generic messages.
- Deep links can only *connect and tune* — no settings changes, no writes.

---

## 7. Testing Plan

**iOS (Simulator + iPhone 17 Pro Max / iPhone SE):**

```bash
xcrun simctl openurl booted "vibesdr://connect?uuid=550e8400-e29b-41d4-a716-446655440000"
```

**Android (Moto G35 / Tab A9):**

```bash
adb shell am start -a android.intent.action.VIEW -d "vibesdr://connect?uuid=550e8400-e29b-41d4-a716-446655440000"
```

Cases to cover:

1. Cold start with valid UUID → connects after init
2. Warm start, different instance → dialog shown, both branches
3. Warm start, same instance + `freq`/`mode` → retune only
4. Malformed UUID → toast, no crash
5. Unknown UUID (not in collector) → toast
6. Collector unreachable → fetch retry then toast
7. `url`+`backend=kiwi` form → connects via Kiwi adapter
8. Duplicate delivery (Android) → handled once
9. Share button → copied link round-trips correctly back into the app

**Joint test with Nathan:** once dev builds are up, Nathan adds a QR/link to a test instance overlay; Stuart scans with the phone camera app → VibeSDR opens and connects.

---

## 8. Future Work (separate briefs — do not implement now)

- **Universal Links / App Links:** hosted association files on a VibeSDR domain → links work in more contexts and fall back to store/web page when the app isn't installed.
- **QR generation in share sheet:** show a QR of the `vibesdr://` link for cross-device sharing.
- **Additional actions:** e.g. `vibesdr://bookmark?...` for importing bookmarks.

---

*73! — Stuey3D*
