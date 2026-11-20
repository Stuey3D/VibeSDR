# SATELLITE MODE — the server half (branch `sat-server`, 2026-11-20)

The spec and THE CONTRACT are `docs/v12/SATELLITE-MODE.md`. This is what the server does, exactly as built, for the
client agents and the next person.

## Wire — as implemented

### `{"type":"sat_list"}` (unchanged request, free — not gated)
The reply is the old one plus:

```
source: { …, "synced": <unix s of the last good /api/sats 200/304, 0 = never>, "fetching": true|false }
          ★ both ABSENT where the server has no supply (iOS, the R36S handheld) — never a made-up 0
sats[i].inRangeFor     s to LOS of the pass in progress — only while el ≥ 0, else absent
sats[i].tx[k].use      "voice" | "image" | "packet"
sats[i].tx[k].sstv     true on an FM downlink whose label or note names SSTV (ISS 145.800 "Voice / SSTV"); else absent
sats[i].tx[k].why      beside every reach:false — the reason in words (grey it, show this)
why (top level)        "Fetching orbit data…" while the on-open fetch runs; else the old NO SAT DATA / NO TIME words
```

`use`: consumer `lrpt` (or mode `lrpt`) → image; consumer `ax25-*` → packet; everything else → voice — FM repeaters,
linear transponders, CW beacons, and AO-73 / MO-122 **bpsk-telemetry** (no decoder of ours exists: heard, not decoded).

`tx.why` words: "Satellites are not on the handheld — it has no network to fetch the orbit data" · "Not on an Icom — …"
· "This receiver cannot tune to 145.800 MHz" · "Outside the range the owner has locked this receiver to (a–b MHz)".

**On-open fetch:** a `sat_list` on a server whose engine has no element sets AND `synced == 0` AND a radio that reaches a
listed downlink calls the supply's `kick()`. It starts a fetch only when one is allowed now (no hold, not too soon, or
another process on the machine is already asking) and then answers `fetching:true`; inside a hold it answers
`fetching:false` with the hold's reason in `why`. Clients re-ask every 3 s while `fetching`.

### `{"type":"sat","op":"pick","norad":N,"tx":i,"doppler":true|false}` (gated like `sat lock`)
`doppler` absent = true (AUTO DOPPLER defaults ON). One server-side step:
1. any earlier pick's lock ends quietly and its decoder is detached (only if still the pick's);
2. the tune: `{"type":"tune","frequency":<tuneHz>,"mode":<listenMode>}` through the ORDINARY handler (`handleControl`)
   — single channel = its frequency, passband = its centre; mode = nfm/usb/lsb/cw/am, `lrpt` → nfm;
3. `doppler` → the old lock body (`satLockOp`) on exactly that downlink (`Engine::start(…, wantTx)`); its outcome is
   told in the usual `{"type":"sat","state":"TRACK|ARMED|COAST|OFF",…}` words;
4. the decoder, through the SAME `DecoderHost` a decoder socket's `audio_extension_attach` uses (`decoders_.hostFor`
   → `host->start`), with the same gates (owner switch, reach, slots + background-decoding yield):
   - image (LRPT) → `lrpt` `{"modulation":"auto","diff":"auto","symbol_rate":72000|80000}` (from the entry's baud)
   - packet → `aprs` `{"packet":"1200"|"9600","baud_rate":1200}`
   - voice with `sstv` → `sstv` (auto-sync)
   - voice → none
5. reply (to the listener; on the one-pipeline/shared dial to EVERY spectrum socket with `"shared":true`):

```
{"type":"sat","state":"picked"[,"shared":true],"norad":N,"tx":i,"decoder":"lrpt|sstv|aprs|","doppler":<lock active now>,
 "hz":<tuned>,"mode":"nfm|usb|…","use":"voice|image|packet","name":"ISS","label":"Voice / SSTV"
 [,"decoderWhy":"<why the decoder did not start>"],"time":"ok|system|none"}
```
   `decoder` is "" when nothing was attached (voice, or refused — then `decoderWhy` says why; the pick still stands).

Refusals (nothing changed): `{"type":"sat","state":"OFF","op":"pick","why":"…","time":…}` — not in the list, reach
(the `tx.why` words), or "Another whole-radio mode has the receiver — leave it first, then pick again" (DAB / ADS-B …
ignored the tune).

★ A client that later sends `audio_extension_attach` for the SAME decoder with the SAME parameters JOINS the pick's
decoder (DecoderHost: same ext + paramsOf → Joined, nothing restarted). The clients do not need to attach at all: a
decoder socket that is open for the session is a peer of that host and receives the output. A different set of
parameters (e.g. the old LRPT key's AUTO without `symbol_rate`) restarts it as the client's own — drop then leaves it.

### `{"type":"sat","op":"doppler","on":true|false}` (gated)
On the current pick: on → the lock (exactly that downlink); off → the lock ends with the dial frozen where the channel is
(brief §9). Then a fresh `picked` message with the new `doppler`. No pick → `OFF`,`op:"doppler"`,"Pick a satellite first".

### `{"type":"sat","op":"drop"}` (gated)
Lock ends (frozen, config sent) and the decoder the pick attached is detached — only if that host still runs the very
decoder the pick started (`DecoderHost::startKey`). Exactly ONE `{"type":"sat","state":"OFF",…}` answers it ("Satellite
mode left", or an empty `why` with `op:"drop"` when there was no lock).

### Housekeeping
A listener's pick (and its decoder) goes when the listener has gone for good (no socket, not held) — the house thread
(`holdTick` → `satPickHousekeep`). The room's pick goes when no spectrum socket is left. Locks keep their own rules.

### Old clients
`sat lock` / `sat unlock` unchanged (`satLockOp`), test-server-sat §2/§3 still pass.

## Orbit data supply

- **Linux / Mac** (`vibeserver/satcat.cpp`): the old daily sync, now a runner thread (`vssat::startDaily`) with
  `kick()` (the on-open fetch) and `fetching()` / `lastSynced()`. Unchanged with a copy (1 h doubling to a day + 0–60 min).
  **With NO copy** (SATELLITE MODE item 9): holds of 15 → 30 → 60 min (then hourly) + 0–5 min, attempts may be 15 min
  apart. **Retry-After** honoured (lengthens a hold up to a day; never shortens it below the floor). The flock (one fetch
  per machine), the provisional hold on disk before the request, and "one request, no retry" are unchanged. A kick
  while another process holds the lock waits ≤ 60 s for its copy. URL: `https://vibeserver.vibesdr.net/api/sats` —
  never CelesTrak. `VIBESERVER_SATS_TEST_URL` (tests only) is accepted only as `http://127.0.0.1:…`.
- **Android app server + Lite**: `satcat.cpp` compiled into `libvibelocalsdr` (the same policy file); its transport is
  `VibeMapGL.fetchSats` (OkHttp, the real status + Retry-After; no redirects, 15 s / 45 s). `VibeMapGL.start` →
  `nativeInitSats(filesDir, UA)`: copy + state in `filesDir/sat/` (a restart publishes the copy and does not refetch),
  the runner with a 30–330 s start-up delay, the on-open kick. Both apps reach it (VibeMapGL.kt is in Lite's SHARED list;
  `VibeMapGL.start` runs on every server start in both). r8-jni.py keeps `fetchSats` (looked up inside a
  `Java_com_vibesdr_app_VibeMapGL_…` function).
- **R36S handheld** (Stuart 2026-11-20 "r36s cant have the satellite mode as no network connection to sync the orbit
  data"): `main.cpp` starts no supply for `--handheld-radio`; `vsSatReachWhy` makes every downlink unreachable there →
  no `doppler` in hwinfo / vibeserver.json, every tx greyed with the words above, picks refused, nothing fetched.

## Tests (2026-11-20, M5 Pro, synthetic)
- `test-sat-engine` **123/123** (§11 new: list fields parsed with a JSON parser — order-independent; uses / sstv / why /
  inRangeFor / synced+fetching; what each pick tunes / demodulates / decodes; the lock on exactly the picked downlink).
- `test-satcat` **68/68** (§10 no-copy holds + Retry-After, §11 Retry-After parsing, §12 the runner + kick: 503 then 200).
- `scripts/test-server-sat.mjs` **96/96** through the real server (fake rtl_tcp, a fake mirror on 127.0.0.1): §5 shared
  dial pick/doppler/drop with SSTV / LRPT / APRS attached + detached and a socket attach JOINING; §6 a listener's own pick
  on a locked window, reach in words; §7 on-open fetch 503 (Retry-After 900 → 15–20 min hold, one request) → 200
  (synced, passes); §8 the handheld offers nothing.
- Android native lib: built once for arm64-v8a (standalone NDK CMake of `android/app/src/main/cpp`) — see the report.

## RESUME HERE
1. **iOS** — the prebuilt DSP lib owes a `build_ios.sh` rebuild (the shim + engine changed). Not built here.
2. **Not tried on air**: a real ISS / Meteor pass through `pick` (the coordinator / Stuart).
3. **Not run on a phone**: the Android fetch (OkHttp → satcat) — check `adb logcat | grep -i "satellite data"` after a
   server start, then open SATELLITE on a phone server with no data.
4. **AX.25 9600 from a pick** (SONATE-2 telemetry): `aprs` `packet:"9600"` reads the listener's channel IQ; the NFM
   channel width a pick tunes is the mode's default — check it is wide enough for G3RUH 9600 on a real pass.
5. **bpsk-telemetry** (AO-73, MO-122) is `use:"voice"` with no decoder — a decoder would make it `packet`.
6. The `picked` message is sent AFTER the lock's own messages (TRACK/ARMED/OFF) — clients should not reset the
   readout on `picked`.
