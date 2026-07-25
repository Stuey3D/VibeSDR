# BRIEF: FM-DX Webserver Backend Adapter

**Project:** VibeSDR (React Native / Expo)
**Author:** Stuart Carr (Stuey3D)
**Status:** Draft — protocol verified against fm-dx-webserver source (NoobishSVK/fm-dx-webserver, cloned 2026-07-08). Exploratory; not yet scheduled against a release.
**Licence note:** fm-dx-webserver is GPL-3.0, same as VibeSDR. This brief is a protocol description; the adapter is a fresh implementation, no upstream code is copied. Courtesy ping to the FMDX.org Discord before shipping is recommended (their published rules govern server hosts, not clients, but goodwill matters in this community).

---

## 1. Goal

Add a new backend adapter, `fmdx`, connecting VibeSDR to any FM-DX Webserver instance (TEF668x / XDR-F1HD hardware tuners). This is a **single-channel hardware tuner** backend, not a wideband SDR:

- **No waterfall.** There is no spectrum/FFT stream in core. The UI presents a **tuner mode**: frequency display, S-meter, stereo indicator, and the existing RDS panel — plus the new `txInfo` transmitter-identification card, which is data no other backend provides.
- **Server-side demodulation and server-side RDS decode** (librdsparser). This slots into the existing *pre-decoded RDS* path used by the OWRX adapter — the adapter maps JSON fields into `StationMeta`; no client DSP involved.
- **Audio is MP3 over WebSocket** — decoded natively (AVAudioConverter / MediaCodec). No new codec libraries.

Primary user story: the worldwide FM-DX server map (servers.fmdx.org) becomes browsable in-app, exactly like the KiwiSDR directory — including the very active Brazilian FM DX community.

## 2. Architecture fit

- New `src/services/FmdxAdapter.ts` implementing the `SDRBackend` interface.
- Extend `BackendKind` with `'fmdx'` (also in the `vibesdr://` deep-link grammar: `backend=fmdx`).
- `BackendCapabilities` for this adapter: no waterfall, no zoom, no client mode list beyond FM/AM (band-dependent), tune step semantics per §4.2, antenna switching when advertised.
- RDS flows through the existing `StationMeta` callback (`onStationMeta` / OWRX-style path).
- Audio: new lightweight MP3 frame path into the native audio engine. iOS: `AudioFileStream`/`AVAudioConverter`; Android: `MediaCodec` (`audio/mpeg`). Frames arrive as clean MP3 chunks (server encodes with `-write_xing 0 -id3v2_version 0 -reservoir 0`, i.e. no headers/tags, frame-independent — safe to start decoding on any frame boundary).

## 3. Connection model

Given a base URL `http(s)://host[:port]`, the adapter uses:

| Endpoint | Transport | Purpose |
|---|---|---|
| `GET /static_data` | REST | Pre-connect info: `tunerName`, `tunerDesc`, `qthLatitude`/`qthLongitude`, `presets[]`, `ant{}` (antenna map), theming fields (ignore) |
| `GET /api` | REST | One-shot snapshot of current state (same shape as §5 JSON) — useful for the connect screen |
| `GET /ping` | REST | Liveness / latency check |
| `wss://…/text` | WebSocket | Control commands out (plain text), state JSON in |
| `wss://…/audio` | WebSocket | 3LAS audio: one JSON handshake out, binary MP3 chunks in |

Not used (exists, for reference): `/chat`, `/rds` + `/rdsspy` (raw RDS group stream — we don't need it, decode arrives in the main JSON), `/data_plugins`.

**Connection hygiene (server-enforced, adapter must respect):**
- Per-IP connection cap (`MAX_CONNECTIONS_PER_IP`) — open exactly one `/text` + one `/audio` socket; tear down before reconnect.
- Rate limiting: server tracks command timestamps; **≥8 commands within 20 ms** triggers antispam. Adapter should coalesce/debounce tuning-drum spam to ≤1 command per ~50 ms (matches drum-wheel debounce we already do for Kiwi).
- Max `/text` payload 16 KB; commands containing `'` are silently dropped by the server.
- Banned IPs get close code `1008`.

## 4. Control protocol (`/text`, client → server)

Plain UTF-8 text commands, one per message (server appends `\n` and forwards to xdrd/serial). Verified command set:

| Command | Format | Meaning | Notes |
|---|---|---|---|
| `T` | `T<kHz>` | Tune | e.g. `T87500` = 87.5 MHz; `T504` = 504 kHz (AM/LW where supported). Server clamps to configured `tuningLowerLimit`/`tuningUpperLimit` and silently ignores out-of-range. `T0` = special "reset/off" used by the web UI |
| `Z` | `Z<n>` | Antenna select | n = 0…3, only when `/static_data.ant` advertises multiple antennas |
| `A` | `A<n>` | AGC | values mirror the web UI dropdown; expose as advanced control only |
| `W` | `W<Hz>` | IF bandwidth | `W0` = auto. **Gated:** silently rejected when server `bwSwitch === false` |
| `F` | `F<n>` | Legacy bandwidth index | Sent by web UI alongside `W` for old firmware; adapter should send both, `F` then `W`, as the web client does |
| `G` | `G<eq><ims>` | cEQ + iMS filters | two digits, e.g. `G11`, `G00` |
| `B` | `B0` / `B1` | Force mono / stereo | |
| `X`, `Y` | — | **Admin only — never send** | Server logs these as dangerous-command attempts from non-admin sessions |

**Tune authority:** the server forwards commands only when `(publicTuner && !lockToAdmin) || isAdminAuthenticated || isTuneAuthenticated`. There is **no explicit "read-only" notification** — a locked server just ignores your `T` commands while the JSON keeps updating with whatever the admin tunes. Adapter heuristic: if `freq` in incoming JSON doesn't converge to our commanded frequency within ~1.5 s of a tune, enter **Spectator mode** (banner + disable tuning input, like the OWRX profile-locked case). Re-probe on user retry.

**Session auth (Phase 2, optional):** tune-password login is an HTTP session (`POST` to the login form → session cookie applies to the `/text` upgrade). Defer unless users ask.

## 5. Data protocol (`/text`, server → client)

Server pushes a JSON object (whole-state, not deltas) on every tuner/RDS update. Verified field set:

```jsonc
{
  "freq": "87.500",        // MHz, string with 3 dp
  "sig": 34.2,              // signal, dBf (TEF scale); smoothed
  "sigRaw": "…",           // raw meter line, ignore
  "sigTop": 41.0,           // peak-hold
  "bw": 110000,             // current IF bandwidth, Hz (0 = auto)
  "st": true,               // stereo pilot detected
  "stForced": false,        // mono forced (B command state)
  "rds": true,              // RDS sync
  "pi": "C201",            // PI code, hex string ('?' when none)
  "ps": "BBC R1  ",        // programme service, space-padded
  "ps_errors": "0,0,1,…",  // per-char error weights (see §5.1)
  "rt0": "…", "rt1": "…", // RadioText A/B banks
  "rt0_errors": "…", "rt1_errors": "…",
  "rt_flag": "0",          // which RT bank is current
  "pty": 8,                 // programme type (European RDS table)
  "tp": 0, "ta": 0, "ms": -1,
  "ecc": 226,               // extended country code (null until received)
  "country_name": "United Kingdom",
  "country_iso": "GB",
  "af": [89100, 90300],     // alternative frequencies, kHz, grows over time
  "ims": 0, "eq": 0,        // current G-command state
  "agc": 0, "ant": 0,       // current AGC / antenna
  "users": 3,
  "txInfo": {               // transmitter ID via maps.fmdx.org lookup
    "tx": "Wrotham",       // transmitter name
    "city": "…", "itu": "G",
    "erp": 250,             // kW
    "pol": "h",            // polarisation
    "dist": 63,             // km from server QTH
    "azi": 118,             // degrees from server QTH
    "id": 1234, "reg": false, "pi": "C201"
  }
}
```

Mapping into VibeSDR:
- `ps` → `StationMeta.name`, `rt0`/`rt1` (selected by `rt_flag`) → radiotext field, `pi`/`pty`/`ta`/`tp`/`af` → existing RDS detail rows. Source badge: `'RDS'`.
- `txInfo` → **new UI element** (tuner-mode card): "Wrotham · 250 kW · 63 km @ 118°". Distance/azimuth are relative to the *server's* QTH (from `/static_data`), not the phone — label accordingly.
- `sig` → S-meter. Unit is dBf on TEF hardware; render the raw number with "dBf" label rather than converting to S-units (matches community convention).
- On tune, server resets the RDS state and `af` empties — clear `StationMeta` when `freq` changes.

### 5.1 Error-weighted RDS text (optional polish)

`ps_errors` / `rt*_errors` are comma-separated per-character confidence weights from librdsparser; the reference web UI renders uncertain characters at reduced opacity. Nice-to-have: thread an optional per-char confidence array through the RDS display and grey uncertain chars. Skip for v1 if it touches too much shared UI.

## 6. Audio protocol (`/audio`, 3LAS)

1. Open `wss://…/audio`.
2. Send exactly one text frame: `{"type":"fallback","data":"mp3"}` (`"wav"` also exists server-side; ignore — MP3 only).
3. Receive binary frames: raw MP3 chunks (48 kHz source, stereo unless server configured mono, bitrate per server config). Feed straight into the platform MP3 decoder; no framing/priming bytes.
4. No keepalive required from us; server drops the socket on its own restart — treat close as "reconnect with backoff", same policy as UberSDR audio.

Latency note: server encodes with LAME `-reservoir 0` and flush-per-packet, so per-chunk decode works; expect ~0.3–0.8 s end-to-end which is fine for a tuner backend. Do **not** run this through the zombie-socket-sensitive `URLSessionWebSocketTask` path on iOS 27 — build on the `NWConnection` WebSocket layer from the audio-regression workstream.

## 7. Server discovery (in-app browser)

`https://servers.fmdx.org/api/` backs the public server map (the webserver POSTs keepalives with a token; status `1` = public, `2` = locked/private). **Action item:** the *read* API shape for listing servers isn't in the webserver repo (that's the map site's side) — Claude Code should fetch `https://servers.fmdx.org/api/` GET and the servermap repo (NoobishSVK/fm-dx-servermap) to confirm the list schema before building the browser. Fields expected per the map UI: name, coords, URL, status, tuner type. Model the browser on the existing KiwiSDR directory screen; filter status=1 by default.

## 8. UI: tuner mode

- Reuse the connect flow; on `backend === 'fmdx'`, mount a **TunerScreen** variant instead of the waterfall stack: large frequency readout, S-meter (with peak-hold from `sigTop`), stereo/RDS pills, RDS panel, txInfo card, presets row (from `/static_data.presets`), antenna selector (when `ant` has >1 entry), BW selector (hidden when commands are rejected), user count.
- Tuning input: the existing VFO drum maps naturally (10 kHz steps ≥30 MHz, 1 kHz below — mirrors the reference UI's step logic). Frequency entry and bookmarks work unchanged.
- VTS could later be driven by `af[]` + presets, but out of scope for v1.

## 9. Out of scope (v1)

- Chat socket, admin functions, tune-password auth, Spectrum Graph plugin scans, `ps_errors` rendering (unless cheap), rdsspy raw group output, FMLIST logging (`/log_fmlist`).

## 10. Acceptance checklist

- [ ] Connect to a public server from the map; audio within 2 s; RDS PS/RT populate.
- [ ] Tune via drum, entry, bookmark, and `vibesdr://…&backend=fmdx` link; out-of-band tunes are rejected gracefully (server clamp) without UI desync.
- [ ] Spectator mode engages on a tune-locked server and recovers when unlocked.
- [ ] Antenna/BW controls appear only when advertised/permitted.
- [ ] Rate limiter never trips during aggressive drum spinning (server debug log shows no antispam warnings).
- [ ] Backgrounding: audio continues, `MPNowPlayingInfoCenter` shows PS name as track title.
- [ ] Reconnect storm test: airplane-mode toggle → clean single reconnect (per-IP cap respected).

---
73!

---

## ★★ FM-DX IS A SEPARATE SCREEN, AND THAT IS WHY IT KEEPS FALLING BEHIND (2026-07-25)

Stuart, testing keyboard control: *"FM-DX is the ugly stepchild at the moment — as soon as you enter
it, it drops all keyboard controls and it switches back to drums even though I have the buttons set."*
Then, separately: *"FM-DX has no Android Auto controls."*

★ These are not three bugs. They are one structural fact: **FM-DX runs on `TunerScreen`, not
`SDRScreen`.** Every capability added to the main screen has to be added to it AGAIN, deliberately —
and each time it has not been, FM-DX has quietly lost a feature the rest of the app gained. It is
the only backend with its own screen, so it is the only one that can drift like this.

★★ **THE RULE THIS IMPLIES:** anything added to `SDRScreen` needs a conscious decision about
`TunerScreen` — even if the decision is "not there, and here is why". The failure mode is silence,
not breakage, so it will not show up in testing unless someone goes looking on FM-DX specifically.

### Outstanding on FM-DX, all from the same cause

1. **No keyboard or controller support at all.** `TunerScreen` has no `VibeKeyDown` listener, so
   entering FM-DX drops every shortcut. Includes: **no way to dismiss the FM-DX warning by keyboard.**
2. **Ignores the drums/keys preference.** It hardcodes the drums rather than reading the shared
   setting, so a user who has chosen the tuner keys gets drums back on this one backend.
3. ★ **Stuart's design for the fix:** a simple header control — a picture of a drum which, tapped,
   becomes a picture of buttons. **ONE toggle swapping both at once**, no menu. Worth doing properly
   because *"some users will be using our app ONLY for FM-DX"* — for them this is not a corner of the
   app, it is the whole app.
4. **No Android Auto controls.** The main screen publishes media controls; FM-DX does not, so it is
   missing in the car — which is precisely where an FM-DX user would want it.

★ Sequencing note: (2) and (3) are the same change and should land together. (1) shares the PanelNav
machinery already built (`BRIEF-controls-keyboard-and-gamepad.md`), so it is plumbing rather than
design. (4) is its own piece and touches the native media session.
