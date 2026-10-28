# The VibeServer protocol number

**Status:** in force from VibeServer 5.6.18 / app and Jr builds after 2026-09-17.
**Companion:** `docs/BRIEF-v11-compatibility.md` (why), `scripts/compat/` (the gate).

## The number, not the version

A client sends `&proto=N` on every WebSocket upgrade (`/ws/user-spectrum`, `/ws/audio`, and the
dxcluster and iq sockets), on the `POST /connection` preflight, and on `GET /vibeserver/radios`.
A client that sends nothing is **legacy** and counts as `proto = 0`.

A server publishes `proto` (the highest contract it speaks) and `minProto` (the lowest it
accepts) in `/vibeserver.json` and `/vibeserver/radios`; the directory forwards both per server,
and each radio in `/vibeserver/radios` carries its own `minProto` (see §Unsupported hardware).

The marketing version (`version` in `/vibeserver.json`, `APP_VERSION`, `JrVersion`) is still
reported and shown, and decides nothing.

| Where | Sends | Reads |
|---|---|---|
| App (`src/constants/version.ts` `APP_PROTO`) | `proto` on every request above | `proto`, `minProto`, per-radio `minProto` |
| Jr (`JrVersion.proto`) | its **own** `proto` — rises only when Jr is updated | the same |
| Web client | `proto` on both sockets | nothing (it is served by the server it talks to) |
| Server (`VS_PROTO`, `VS_MIN_PROTO` in the shim) | `proto`, `minProto` | the requester's `proto` |

## Refusal

When a client's `proto` is below the server's `minProto`, `POST /connection` answers

```json
{"allowed":false,"reason":"update-app","proto":1,"minProto":1,"webUrl":"/"}
```

before any socket is opened (a refused upgrade is a bare 1006 to an app, indistinguishable from
"server down"). The app shows *"This server needs a newer VibeSDR. Update the app, or open the
receiver in your browser for now."* with **Open in browser** to `webUrl` resolved against the
server's origin. Every refusal is logged with the peer, its `client` string and its `proto`. The
front door forwards a bare `/connection` to the primary radio, which applies the same check.

## Liveness (dead-session release)

A client at `proto ≥ 1` is judged alive by its **application messages** — any text frame (`ping`,
`tune`, anything) on **any** socket of its session — within 30 s. WebSocket pongs do not count:
they prove a kernel and a process, not a listener. Legacy clients keep the old rule (any frame,
including a pong, within 20 s).

So a `proto ≥ 1` client sends `{"type":"ping"}` every 10 s when nothing else is happening. Jr sends
it on every open socket while its process runs — which includes wrist-down background listening
(the audio keeps the process alive) and excludes a Jr that stopped playing and was forgotten.

## proto = 1 — the locked core

Any client at `proto ≥ 1`, and legacy clients while `scripts/compat/legacy-10.3.1.mjs` passes,
always get:

- **Discovery and access:** `POST /connection`, `GET /vibeserver/auth`, `GET /vibeserver.json`,
  `GET /vibeserver/radios`, and `/r/<id>/…` routing (including the trailing-slash case a tunnel
  produces).
- **Streams:** `/ws/user-spectrum` (SPEC binary8 frames, `config`, `hwinfo`, `sig`, `dial`,
  session messages) and `/ws/audio` (Opus, always stereo since 5.6.1; ADPCM/PCM for older asks).
- **Controls:** `tune`, `mode`, `bandwidth` (`low_cut`/`high_cut`), `squelch`, `zoom`, `fftRate`,
  `set_rate`, gain as each radio exposes it (`gain`, `agc`, the RSP and Airspy controls), and the
  session/turn/TIME UP messages.
- **Station data:** base RDS (`rds`: PS, RadioText, PI, stereo), the DAB service list and audio.

Rules for everything after this number:

1. **Additive only.** New message types and new JSON fields; old clients ignore what they don't know.
2. **Never change the meaning or units of an existing field or message.** Add a new one and keep
   sending the old until `minProto` passes it.
3. **Advertise new features as capability flags** (`dab`, `rdsx`, `rawIq`, `modes`, …). A client
   draws a button only for a flag it both sees and understands.
4. **Defaults for silence.** A parameter a client does not send behaves as it did before the
   parameter existed.

## Unsupported hardware

Each radio in `/vibeserver/radios` carries `minProto`: the lowest client protocol with controls
for it, set from the **driver** in the server (never listed in an app). Every driver shipping
today (`rtlsdr`, `sdrplay`, `airspyhf`, `hackrf`) is 0.

- A V11 app greys such a radio out with **"Unsupported SDR — update VibeSDR"** (Jr: "… update
  VibeSDR Jr", against Jr's own `proto`).
- A **legacy** requester (no `proto`) is not told about it: the server and the directory omit it
  from the list. The directory filters **per request** from its `proto` query parameter.

## Additions under proto 1 (additive — no bump)

| Date | Addition | Older clients |
|---|---|---|
| 2026-09-17 | `{"type":"battery","level":N,"charging":b,"pauseAt":P,"resumeAt":R,"paused":b}` on the spectrum socket every minute and on change; `batteryLevel`/`batteryCharging`/`batteryPaused`/`batteryPauseAt`/`batteryResumeAt` in `/vibeserver.json`; `battery` in the admin status and the directory listing; preflight reason `battery-paused` (`level`, `resumeAt`) and a 503 with the same body on a bare upgrade while suspended; VTS notices at pauseAt+10, pauseAt+5 and pauseAt | Ignore the message and the fields; show the reason string |
| 2026-10-14 | `squelch` takes an optional `"on": true\|false`; when present it decides on/off and `db` is the threshold however low. hwinfo carries `"sqlOn":true` on a server that honours it. (Without `on`, `db <= -100` still means off — and that sentinel sat inside real channel levels: an Airspy HF+'s quiet airband channel reads ≈ -110, so auto squelch's threshold went out as OFF.) Clients send `db:-100,on:false` for off | Unchanged: no `on` → the old rule. A new client against a server without `sqlOn` treats `db <= -100` as off itself |
| 2026-10-28 | **The full-capture row, SPWB** . A spectrum socket that adds `&wide=1` to its URL, while its view is narrower than the capture (< 98 %), receives after every 8th SPEC frame it is sent one binary frame: `'S','P','W','B'`, `1` (version), `0` (flags), u64 LE timestamp ns, u64 LE centre Hz (the capture's physical centre), u32 LE span Hz (the capture width), then 1024 bins **low → high** (natural order, not SPEC's DC-at-0), u8 = dBFS + 256, max-held from the wide FFT, 0 = no data. Never raises the frame rate (it rides the listener's own frames); dropped first while the listener's audio is backed up. `hwinfo` carries `"wide":1024` on a server that sends it. Cost: +1.4 KB/s per zoomed listener at 10 fps (+13 %), +2.7 KB/s at 20 fps; nothing while not zoomed | Never ask, never receive it: the URL, the SPEC stream and its rate are unchanged. A new client against an older server simply gets no SPWB (and draws black outside its view, as before) |

## Changing the number

Bump `proto` only when wire behaviour changes; raise `minProto` only when a change cannot be
made additive. Each bump gets a dated line here stating what changed and what a lower client
still gets. The server supports at least `proto − 1`.

| proto | Date | What changed | What a lower client still gets |
|---|---|---|---|
| 1 | 2026-09-17 | The locked core above; application-message liveness; `update-app` refusal | Legacy (0): the core, judged alive by any frame within 20 s |

## The gate

`scripts/compat/` holds one fixture per shipped client — the exact request sequence that client
sends, **captured** from a real session (`VIBE_COMPAT_RECORD=<file>` on a test server writes the
lines a fixture is made from), never typed from today's code, and never edited once recorded. Each
checks a 101 on both sockets, a spectrum frame, an audio frame, and the squelch/tune/mode
round-trips — or the `update-app` refusal. `scripts/compat/run.sh <url>` runs them all;
`publish-apt-remote.sh` (and the backup `publish-apt-docker.sh`) refuse to publish unless they pass against
`VIBE_COMPAT_TARGET`.
