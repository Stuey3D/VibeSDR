# BRIEF: VibeServer Protocol Foundations — Versioning, Multi-Client, Control Token, Admin Role

**Project:** VibeSDR (React Native / Expo — built from native projects, NO `expo prebuild --clean`)
**Author:** Stuart Carr (Stuey3D)
**Branch:** `experimental` (VibeServer surface currently identical to shipped `v8.0.0`/`main` — verified by diff)
**Files touched:** `android/.../local_sdr_shim.cpp` (primary), the Kotlin VibeServer service wrapper, `src/services/vibeServer.ts`, `src/services/vibeAuth.ts` (shared token fn — likely unchanged), `web/client/src/*` (auth.ts, main.ts, UI), the app's VibeServer client adapter, `ServerModeScreen.tsx`

---

## 1. Why now

VibeServer is about to outgrow "one phone, one client". The roadmap is: headless Raspberry Pi daemon → remote tunnelled receiver sites → an eventual public directory/network. The moment a server binary lives on someone else's shelf, client and server stop shipping together — and every SDR network that skipped protocol versioning (Kiwi fragmentation, SpyServer client/server mismatches) has paid for it in permanent support debt. The v8 wire format has **no version negotiation**. This brief adds the foundations while the installed base is one release old, plus the multi-user model the network will need:

1. **Protocol versioning + capability negotiation** (§3)
2. **Multi-client: up to 5 users on one DSP chain** (§4)
3. **Control token: first-connected user tunes; admin overrides** (§5)
4. **Admin role: separate secret, gates control seizure + server-side bookmark writes** (§6)

Everything here lands in the existing Android shim. The Pi daemon (out of scope) inherits it by recompilation — that is the point of doing it first.

## 2. Current state (v8, as read from source)

- **Single client slot**, single demod chain; the serving phone is silent; `vibeServer.ts:10-14`.
- **Auth:** `GET /vibeserver/auth` → `{required, nonce, lockedFor}`; token = `HMAC-SHA256(key=PIN, msg=nonce-as-hex-text)`; every WS carries `?vs_nonce=&vs_auth=`; nonce is a 1h session credential shared across sockets/reconnects; brute-force lockout with server-reported `lockedFor` (shim `:370, :1622`; `web/client/src/auth.ts`).
- **Audio wire:** magic byte, format byte (0 raw int16 / 1 ADPCM mono / 2 ADPCM mid/side), rate, ADPCM self-seeded blocks (`local_sdr_shim.cpp` `sendAudioPcm:1057`; decoder `src/services/imaAdpcm.ts`, shared verbatim with web).
- **Bookmarks:** persistence lives in the shim (`setBookmarksPath`), saved atomically on change (`vibeServer.ts:130-142`). The server-side save button is currently **hidden client-side unless admin** in the public build — there is no server-side enforcement.
- **Config:** `VibeServerConfig` — name, PIN ('' = open), maxBandwidthHz, maxFftRate, compressAudio, webServer toggle, lockedRate, advertise, autoRestore.

## 3. Protocol versioning

### 3.1 `GET /vibeserver/info` (new, unauthenticated)

```json
{
  "proto": 2,
  "protoMin": 1,
  "name": "<server name>",
  "caps": ["multiclient", "control-token", "admin", "bookmarks-rw"],
  "clients": 2,
  "maxClients": 5,
  "controlHeld": true,
  "adminPresent": false
}
```

- `proto` = highest version served; `protoMin` = oldest still accepted. v8-as-shipped is retroactively **proto 1**.
- `caps` is a string list — clients ignore unknown entries (forward compatibility rule, §3.4).
- `clients`/`maxClients`/`controlHeld` also solve a UX gap: discovery UIs (mDNS list, instance picker, future watch client) can show "3/5 · in use" instead of letting a user connect blind. No auth required — this is lobby information, and it must not leak the PIN, nonce, or client addresses.

### 3.2 WS hello

First client→server message on the control WS (new, JSON): `{"t":"hello","proto":2,"caps":[...]}`.
Server replies `{"t":"hello","proto":2,"role":"controller"|"listener"|"admin","clients":n,"maxClients":n,"session":"<hex>","window":{"centre":..,"span":..}}` — `session` is the reconnect-reclaim credential (§5.2), issued on every server (PIN or open). Clients may present `"resume":"<hex>"` in hello to reclaim a parked role.

- Client proto outside `[protoMin, proto]` → server closes with **WS close code 4426** ("upgrade required") and a human-readable reason string. Clients must surface this as "server needs a newer/older app", never a generic connection error.
- **No hello within 3s of WS open → legacy proto-1 client.** Accepted (§3.3), flagged internally.

### 3.3 Legacy (proto 1 / shipped v8) clients

Accepted as ordinary participants in the token model: they occupy a slot, they receive audio/waterfall, and they may hold the control token by the normal rules. What they lack is role awareness — a legacy listener's tune commands are rejected server-side (§5.4) and their UI won't explain why. This is acceptable: the proto-2 app update ships in the same release as the server change, the installed base is one version deep, and the alternative (refusing legacy clients) is worse. Document in release notes.

### 3.4 Forward-compatibility rules (bake into both ends, permanently)

- Unknown JSON message types: **ignore silently**, never error.
- Unknown fields in known messages: ignore.
- Unknown `caps` entries: ignore.
- Binary frames keep their magic/format-byte discipline; new binary types get new magic values, never reinterpretations.
- `proto` bumps ONLY for changes that break these rules; capabilities carry everything else. This is what keeps a 2027 Pi daemon and a 2026 watch client conversational.

### 3.5 Reserved for the future (not implemented in this brief)

- **Directory/registry:** cap string `"registry"`, message type `"t":"register"` — the future heartbeat-to-directory mechanism.
- **Multi-receiver hosts (the Pi daemon):** cap string `"multi-sdr"`; an optional `"rx"` field in the client hello (receiver id, or `"auto"` for lobby assignment); a `receivers` array in `/vibeserver/info` (per-receiver: id, name, subtitle, suggested-use tags, allowed frequency ranges, current window, clients/maxClients, controlHeld). Single-receiver servers today simply omit all of it — a proto-2 client MUST treat an absent `receivers` array as "one implicit receiver", which is why this reservation costs nothing now and saves a proto bump later. Per-receiver semantics (independent token/roster/grace per receiver, auto-assignment of idle receivers, range enforcement incl. FFT-bin masking of disallowed spectrum) are specified in the Pi daemon brief, not here.

No behaviour in this brief for either reservation — they exist to prevent future collisions.

## 4. Multi-client (up to 5)

### 4.1 Model

**One capture, per-client VFO channels, one shared waterfall.** The dongle captures a fixed window (centre + rate, e.g. 2.4 MHz). Each client gets an INDEPENDENT listen VFO within that window — its own frequency offset, mode, bandwidth, and squelch — meaning a per-client demod chain in the shim (frequency shift → decimate → demod → per-client ADPCM encode), the KiwiSDR shared-front-end model. The waterfall is the one capture-wide FFT, identical rows to every client (per-client FPS decimation only), so everyone sees the same window while hearing their own signal.

Global (shared, token-gated §5): the RF **centre**, capture sample rate, and anything touching the hardware (RF gain, bias-T, direct sampling). Per-client (free): VFO frequency within the window, mode, bandwidth, squelch, mute, FPS tier.

**VFO clamp invariant (server-enforced):** a client VFO must always lie within the captured span — the shim clamps any out-of-window VFO command to the window edge and echoes the clamped state (same invariant class as the OWRX panSpan rule in the scanner brief). When the controller MOVES the centre, every VFO that falls outside the new window is clamped to the nearest in-window edge and that client receives a state push explaining its VFO moved.

**CPU reality check:** five simultaneous shift/decimate/demod chains from a 2.4 Msps capture is real NEON work. VibeDSP should share the first coarse decimation stage where offsets permit, but the acceptance tests (§8) include a sustained thermal run on the weakest host (Motorola G35) — if it can't hold 5, the honest fix is the host lowering `maxClients`, not silent audio degradation.

### 4.2 Config

- `maxClients: number` (1–5, default 5) added to `VibeServerConfig` + the Server Mode screen (a simple stepper). `maxClients: 1` reproduces today's behaviour exactly.
- `controlGraceSec: number` (0–300, default 60) — the controller reconnect grace window (§5.2). 0 = immediate succession.
- `maxSampleRate: number` (0 = uncapped) — a CEILING on the capture rate, distinct from the existing `lockedRate` pin: the controller may choose any rate at or below it, and client rate pickers hide options above it. This is the host's load dial — per-client demod cost scales with capture rate, so a weak host set to 1 MHz + 3 clients runs at roughly a quarter of the 2.4 MHz × 5 load. The Server Mode screen presents `maxClients` and `maxSampleRate` together as the "weak device" pair. Precedence: `lockedRate` (if set) wins; a `lockedRate` above `maxSampleRate` is a config-screen validation error.
- `hardwarePolicy`, `gainMin`/`gainMax`, and owner-editable idle defaults (§5.4) join `VibeServerConfig` + the Server Mode screen.
- `/vibeserver/info` additionally advertises `maxSampleRate`, the current window, and the hardware-policy summary, so joiners see what they're getting before connecting.
- Slot exhaustion: WS upgrade rejected with HTTP 503 + `Retry-After`, and `/vibeserver/info` already shows the count — clients should check info first and present "server full (5/5)".

### 4.3 Fan-out mechanics (shim)

- Audio: **per-client** demod + ADPCM encode (each client hears their own VFO); per-client send buffers with a drop-oldest policy per socket — one stalled hotel-WiFi listener must never backpressure the other four (mirror the app's own queue lesson from the RTL-TCP work, but drop-OLDEST here: stale audio is worthless). ADPCM encode ×5 is negligible; the demod chains are the cost (§4.1).
- Waterfall: ONE capture-wide FFT at the server rate, identical rows broadcast; honour **per-client FPS tiers** by decimation (a `quarter` client gets every 4th row). Tier requested in hello (`"fps":"half"`) or via the existing rate message; `maxFftRate` config still caps globally.
- Status broadcast: on any join/leave/role change/centre change, broadcast `{"t":"roster","clients":n,"you":"listener","controlHeld":true,"adminPresent":false,"window":{"centre":..,"span":..}}`. **Anonymous by design** — counts, the client's own role, and the shared window only; no addresses, no names, nothing identifying between strangers.

## 5. Control token

### 5.1 Rules (product spec, verbatim intent)

**"Control" means the shared front end only** — RF centre, capture rate, hardware settings. Every client, controller or not, freely tunes their own listen VFO within the captured window at all times (§4.1). The token decides who may move the window itself.

1. One user connected → that user has full control (centre + their VFO).
2. Two or more → the **first-connected** user holds the centre-control token; later joiners are listeners (VFO-free, centre-fixed).
3. A client authenticated as **admin** (§6) overrides everyone: admin takes the token on demand — including from a parked token during a grace window — and holds it until release or disconnect.

### 5.2 Succession and the reconnect grace window

- **Controller disconnects → the token PARKS for `controlGraceSec` (config, default 60, range 0–300)** rather than passing immediately. A network blip or app crash must not cost a user control of the receiver they set up. During the park: the centre is simply pinned — listeners retain full VFO freedom within the current window, so nobody is meaningfully frozen; the roster broadcasts `"controlHeld":"parked"` so clients can show "controller reconnecting…".
- **Reclaim identity: a server-issued session token.** The hello reply carries `"session":"<hex>"` (issued to every client, PIN or open server alike — deliberately NOT the auth nonce, so open servers get reclaim too). A reconnecting client presents it in hello (`"resume":"<hex>"`); a valid resume within the grace window restores controller role and queue position. Session tokens die on clean disconnect (a deliberate leave is not a blip) and at grace expiry.
- **Grace expires → token passes to the longest-connected remaining client** (join-order queue), roster broadcast, promoted client's UI enables centre control.
- Admin releases or disconnects → token returns to the longest-connected non-admin (a parked non-admin claim does not survive an admin seizure — admin action supersedes the grace window). Join order is the single source of truth for succession; keep it explainable.
- A reconnect WITHOUT a valid resume token = a new join, back of the queue.

### 5.3 Token-gated vs per-client vs open commands

| Command class | Who |
|---|---|
| RF centre, capture rate (where not `lockedRate`), RF gain, bias-T, direct sampling — anything global/hardware | Controller or admin only |
| Listen-VFO frequency (within window — clamp §4.1), mode, bandwidth, squelch | **Any client, always** — their own channel |
| Per-client FPS tier, local mute | Any client — affects only their own tap |
| Bookmark reads, EiBi/band data | Any client |
| Bookmark writes (server-side store) | **Admin only** (§6) |
| Server settings changes (future) | Admin only |

### 5.4 Hardware control policy & idle restore

Hardware settings are global (they affect every client's window), so they are token-gated at minimum (§5.3) — but the owner may restrict further, per control:

- **Three access levels per hardware control** — `open` (the controller may change it), `admin` (admin only), `locked` (fixed at the owner's configured value; nobody at runtime). Config: `hardwarePolicy: { gain: 'open', biasT: 'admin', directSampling: 'locked', ... }`. **Default everything `open`** to preserve v8 behaviour; restriction is opt-in. Motivating example: an RTL feeding a powered antenna — bias-T set to `admin` or `locked` so no user can depower the masthead amp, while gain stays `open`.
- **Gain range clamp:** `gainMin`/`gainMax` config. The server clamps out-of-range commands (echoing the clamped value — same discipline as the VFO clamp) and advertises the permitted range in the hello reply, so client sliders render only the allowed span: the user sees the owner's range as THE range, not an invisible wall.
- **Idle restore:** when the last client disconnects AND all grace windows have expired (client count truly 0), the receiver reverts to owner-defined defaults — gain, bias-T, centre, capture rate, the lot. Defaults are the launch-configuration snapshot unless the owner explicitly edits them. The next visitor finds the receiver as the owner intended, not as the last stranger left it, and a powered-antenna/gain setup self-heals without intervention.
- Denials use the existing message: `{"t":"denied","cmd":"biasT","reason":"admin-required"|"locked"}`. Locked controls render as read-only state in clients (visible truth), not hidden.
- `/vibeserver/info` and the hello reply carry the policy summary (which controls are open/admin/locked, gain range) so clients build the correct UI before the first command is ever refused.

### 5.5 Enforcement is SERVER-SIDE

The shim rejects out-of-role commands with `{"t":"denied","cmd":"centre","reason":"listener"}` — hiding buttons in clients is UX, not security, and legacy clients have no buttons to hide. The denial message lets proto-2 clients show a one-line explanation ("Another user controls the receiver window") instead of dead controls. VFO commands are NEVER denied for role reasons — only clamped (§4.1).

### 5.6 Client UX (app + web; the future watch client inherits this contract)

- Listener mode: VFO tuning fully live within the window; only centre/hardware surfaces disabled/greyed, with a compact "SHARED · window set by another user" indicator (or glyph, per the watch-fixes Part C convention); waterfall panning clamps to the window edges; bookmark save hidden. A parked token shows "controller reconnecting…".
- Promotion to controller: brief "You now control the receiver window" notice; centre/hardware controls enable.
- Admin: a "Take control" action appears when authenticated as admin and not holding the token.

## 6. Admin role

- **Second secret**, `adminPassword`, in `VibeServerConfig` + Server Mode screen ('' = no admin role on this server). MUST be distinct from the access PIN; the config UI enforces that.
- **Auth mechanism: reuse the existing nonce/HMAC scheme unchanged** — the client computes a second token with the admin password as key and presents `?vs_admin=<token>` alongside the normal pair, or upgrades in-session with `{"t":"admin","token":...}` (so an admin can elevate without reconnecting). Same brute-force lockout machinery, same 1h nonce.
- Admin grants: control seizure (§5), server-side bookmark writes, future settings surface. The bookmark-write gate goes into the shim's bookmark message handler — the currently-hidden button becomes hidden AND rejected. `denied` reason: `"admin-required"`.
- `/vibeserver/info` exposes only `adminPresent: bool` — never whether an admin password is configured (that invites targeted brute-forcing of the second secret).

## 7. Out of scope

- The Pi/headless daemon itself (this brief is its foundation; port brief follows).
- Directory/registry service and heartbeat behaviour (cap string reserved only, §3.5).
- Per-client demod chains — permanently out for phone hosts; revisit for Pi-class hosts only if the network ever demands it.
- Any change to the audio/waterfall binary formats, the auth HMAC construction, mDNS advertising, or `sdr://`/deep-linking behaviour.
- Watch client work (`BRIEF-vibesdr-jr-*`, separate).

## 8. Acceptance criteria

1. **Version negotiation:** proto-2 client ↔ proto-2 server exchanges hello, role assigned. Artificially lowered client proto → clean 4426 close, client shows "update required" (not a generic error).
2. **Legacy compat:** a shipped-v8 app build connects to the new server, occupies a slot, receives audio + waterfall, and can tune when it is the sole/first client.
3. **Five clients:** 5 mixed clients (app, web, legacy) stream simultaneously, each on a DIFFERENT VFO; audio clean on all; per-client FPS tiers verifiably different; 6th connect refused with a "server full" surface, not a hang. **Thermal/CPU:** sustain this for 15 minutes on the Motorola G35 — if it cannot, document the measured safe maxClients for low-end hosts rather than shipping degradation.
4. **Token rules:** client A (first) moves the centre; B–E cannot (server `denied`, UI explains) — but B–E each tune their OWN VFO to different signals inside the window and hear independent audio simultaneously.
5. **Grace window:** kill A's app (crash, not clean leave) → token parks, roster shows "parked", centre pinned, B–E VFOs still free. A relaunches and resumes with its session token inside 60s → A is controller again. Repeat but wait past 60s → B promoted, A rejoins as listener. Clean disconnect by A → immediate succession, no park.
6. **Clamp on centre move:** with B's VFO near the window edge, A moves the centre so B's VFO falls outside → B's VFO clamps to the new edge, B receives the state push, audio continues without a stall.
7. **Admin:** admin token elevates mid-session, seizes control from A (and from a PARKED token during a grace window), moves the centre; on admin disconnect, token returns to longest-connected. Wrong admin password triggers the lockout path independently of the access PIN.
8. **Bookmark enforcement:** non-admin bookmark write is rejected server-side (verify with a raw WS message, not just the hidden button); admin write persists via the shim's atomic save.
9. **Stalled-listener isolation:** throttle one client's network to near-zero — the other four must show no audio disturbance (drop-oldest per-socket buffer working).
10. **Info endpoint:** `/vibeserver/info` correct through the full lifecycle (counts, controlHeld, adminPresent), and leaks nothing sensitive.
11. **Hardware policy:** with `biasT:'admin'`, a controller's bias-T command is denied with reason `admin-required` and the client renders it read-only; admin toggles it successfully. With `gainMin/gainMax` set, an out-of-range gain command is clamped and echoed; the client slider shows only the permitted span.
12. **Idle restore:** controller changes gain, centre, and bias-T (as admin), then all clients disconnect; after the grace window fully expires, the shim's state matches the launch defaults exactly — verified via `/vibeserver/info` and a fresh connect.
13. **Single-client regression:** `maxClients: 1` behaves byte-identically to v8 for a proto-1 client (the shipped web client passes untouched).
