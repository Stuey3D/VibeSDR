# BRIEF: V11 — one version across the ecosystem, and a server that never strands an app

**Project:** VibeSDR (app, Jr, VibeServer Linux/macOS/Android host)
**Author:** Stuart Carr (Stuey3D) with Claude, 2026-09-17
**Status:** BUILT 2026-09-17 (server 5.6.18, app/Jr builds after it) except §3 renumbering and the
recorded fixtures for the store Jr, V11 phone and V11 Jr (§8; `VIBE_COMPAT_RECORD` on a test
server captures them). The rules live in `docs/PROTOCOL.md`.

---

## 1. Why

A month of server work went out on APT while the App Store build stayed at 10.3.1 (17–18 Aug).
Old apps met rules they had never had to satisfy: a store user (xavxx, Discord #bugs-issues,
2026-09-17) got a blank spectrum, then an endless "Connection lost". The web client worked, Jr
connected but showed a wrong squelch bar, and Jr left the radio "in use" on watchOS 26.

Nothing told him why. Stuart never saw it because he always runs matched latest builds.

**Goal:** any app a user can still have installed gets either the core experience or a clear
instruction. Never a silent failure.

## 2. Decisions (Stuart, 2026-09-17)

1. **V11 everywhere.** The app, Jr and VibeServer (all hosts) move to major version 11 together,
   as Apple did with 26. The server jumps from 5.6.14 to 11.0.0.
2. **The core is locked.** From V11, server additions must not break older apps. The next release
   (decoders and scanners) is additive: older apps get the core experience without the new buttons.
3. **Breaking changes warn instead of failing.** If one is unavoidable, the server detects the old
   client and tells the user to update the app or use the web client meanwhile.
4. **Unsupported hardware is greyed out.** If a server's only radios need controls an app does
   not have, the app's directory greys that server out with **"Unsupported SDR — update VibeSDR"**.

## 3. Version numbers

- **Only the major is shared.** Minor and patch move independently per component, so an urgent
  Jr fix never forces a server or phone release (Apple ships 26.0.1 on one OS only).
- **Places to change** (all to 11.0 / 11.0.0):
  - `vibeserver/CMakeLists.txt`: `project(vibeserver VERSION …)`. This already feeds the `.deb`,
    the macOS build (`build-app.sh`) and `VIBESERVER_VERSION_STR`.
  - `app.json` `expo.version`, `src/constants/version.ts` `APP_VERSION`.
  - `android/app/build.gradle` `versionName`. `versionCode` keeps counting up.
  - `ios/VibeSDR.xcodeproj/project.pbxproj`: **all four** `MARKETING_VERSION` entries (phone and
    watch targets).
  - Jr's User-Agent strings. `check-versions.mjs` records Jr carrying three different versions
    at once.
- **Extend `scripts/check-versions.mjs`** to fail if the *major* of any of the above differs from
  the server's CMake major.

**TRAP — keep release version strings clean.** Going from 5.6.14 to 11.0.0 is a valid dpkg
upgrade. A `~beta` suffix sorts *below* the release, so never publish one to the `main` APT
component, which is currently the only component.

## 4. The protocol number: what the server actually checks

**Decision to confirm:** gate on an integer `proto`, not the marketing version. The marketing
version has shipped wrong three times (see `version.ts` and `check-versions.mjs`); gating on it
would turn a cosmetic slip into a refused connection. The app *version* is still reported and
shown in messages and logs; it just doesn't decide anything.

- **Starting value:** `proto = 1` is the V11 core contract (§5). Clients that send no `proto`
  (10.3.x, the current store Jr) are **legacy**, treated as `proto = 0`.
- **Client sends it:** `&proto=N` on every WebSocket upgrade (spectrum, audio, dxcluster, iq), on
  the `/connection` preflight, and on `/vibeserver/radios`, alongside the existing `&client=`.
- **Server publishes it:** `proto` (highest it speaks) and `minProto` (lowest it accepts) in
  `/vibeserver.json` and `/vibeserver/radios`, and the directory publisher forwards both.
- **When to bump it:** only when wire behaviour changes. Each bump gets a line in
  `docs/PROTOCOL.md` stating what changed and what a lower client still gets.
- **Support window:** the server supports at least `proto - 1`, and legacy for as long as §8's
  test says it works.

**TRAP — send it in the query string.** React Native's WebSocket does not reliably send a
User-Agent on the upgrade (see the note beside `client=` in the shim). A header-based gate would
misclassify V11 apps as legacy.

**TRAP — Jr counts separately.** Jr sends its *own* `proto`. It only rises when Jr is updated,
never because the phone app was.

## 5. The locked core (`proto = 1`)

Any client at `proto ≥ 1`, and legacy clients where §8 shows it works, must always get:

- **Discovery and access:** `/connection` preflight, `/vibeserver/auth`, `/vibeserver.json`,
  `/vibeserver/radios`, and `/r/<id>/…` routing (including the Cloudflare slash case from
  `498c49b9`).
- **Streams:** `/ws/user-spectrum` and `/ws/audio`.
- **Controls:** tune, mode, bandwidth, squelch, gain as each radio exposes it, and
  session/turn/TIME UP messages.
- **Station data:** base RDS (PS, RT, PI, stereo), the DAB service list and audio.

**Rules for everything after V11:**

1. **Additive only.** New message types and new JSON fields; old clients ignore what they don't
   know.
2. **Never change the meaning or units of an existing field or message.** If one must change, add
   a new one and keep sending the old one until `minProto` passes it.
3. **Advertise new features as capability flags** in `/vibeserver.json` or per radio (as `rdsx`
   and `rawIq` already are). A client draws a button only for a flag it both sees and understands,
   which is how "older apps just lack the new buttons" happens with no extra work.
4. **Defaults for silence.** When a client doesn't send a new parameter, the server behaves as
   it did before that parameter existed.

**TRAP — the squelch bug is Jr-side, not a server break.** xavxx's "sql bug" is an indication
fault: the gate works, but Jr feeds its dBFS needle scale with spectrum SNR, so the bar is always
full. That fix lives in `BRIEF-jr-vibeserver-display.md` §4. Still run the legacy fixture's
squelch round-trip (§8) against `97db4316` (per-listener squelch, 5.6.14) to confirm the *gate*
behaves the same for old clients, since squelch is part of the locked core.

## 6. When a breaking change cannot be avoided

1. Raise `minProto` on the server.
2. **Preflight refusal:** a client below it gets
   `{"allowed":false,"reason":"update-app","webUrl":"<receiver URL>"}`.
3. **App message (V11 onwards):** "This server needs a newer VibeSDR. Update the app, or open
   the receiver in your browser for now." with an **Open in browser** button to `webUrl`.
4. **Legacy clients:** they don't know `update-app`. Verify what 10.3.1 shows for an unknown
   preflight reason and choose wording that still reads sensibly there. Also post the message
   through the version-gated `/api/notice` endpoint (`docs/PENDING-NEXT-RELEASE.md` §3), which already
   reaches 10.3.1.
5. **Log it:** every refusal logs the peer, the `client` string, `proto` and the reason.

**TRAP — the refusal must happen before the socket opens.** A refused WebSocket upgrade reaches
the app as a bare 1006 with no body, indistinguishable from "server down" (see
`vibeserverRadios.ts`). The preflight and `/vibeserver.json` are the only places a reason can be
delivered.

**TRAP — the front door must refuse too.** Legacy clients can be stopped at the door before any
radio process sees them, so the door must apply the same `minProto` check and give the same
reason.

## 7. Unsupported hardware

- **Per radio:** each entry in `/vibeserver/radios` gains `minProto`, the lowest client protocol
  with controls for that radio. It is set from the driver in the server, not listed in the app, so
  new hardware needs no app change to be handled correctly.
- **App picker:** radios with `minProto` above the app's `proto` are shown greyed out with
  **"Unsupported SDR — update VibeSDR"** and cannot be tapped.
- **App directory:** a server whose radios are *all* unsupported is greyed out with the same
  label. A server with at least one usable radio stays normal.
- **Legacy clients** can't read `minProto`, so the server and the directory **omit** unsupported
  radios from what they send to a requester with no `proto`. A legacy app then never offers a
  radio it can't drive.

**TRAP — filter by the requester, not globally.** The directory API is shared by every client
version, so `minProto` filtering must be applied per request from the `proto` query parameter.
Filtering the published list once would hide new hardware from new apps.

**TRAP — Jr is stricter.** A radio Jr can't drive may be fine on the phone. Jr's greying uses
Jr's own `proto`.

## 8. The compatibility gate before every APT publish

- **`scripts/compat/`:** one fixture per shipped client (10.3.1, the current store Jr, V11 phone,
  V11 Jr). Each fixture holds the exact request sequence that client sends: preflight, radios,
  upgrade URLs with parameter order, trailing slashes and parameters left out.
- **Captured, not written:** record each fixture from a real session of that build. A fixture
  written from today's code only tests today's assumptions. Once recorded, a fixture is never
  edited.
- **What each fixture checks:** a 101 on spectrum and audio, at least one spectrum frame, at
  least one audio frame, and correct squelch, tune and mode round-trips, or else the §6 refusal
  with its reason.
- **Three paths:** direct radio port, the front door `/r/<id>`, and a real quick tunnel (the slash
  normalisation only happens in transit).
- **Enforced:** `publish-apt.sh` refuses to publish unless the gate passes.
- **Manual check:** one physical device stays on the current App Store build of the app and Jr,
  updated only through the App Store, and is tested against each server release.

**TRAP — TestFlight and Xcode builds don't count.** They are the latest code, not what users
run, so the manual device must hold the actual store build.

## 9. Carried in with V11

- **Jr display and squelch-bar fixes:** see `BRIEF-jr-vibeserver-display.md` (1024-bin
  oversampling, local zoom, `sig`-driven squelch bar, 5 fps everywhere). The V11 Jr fixture in §8
  is recorded *after* those land.
- **Server-side release of dead sessions:** release a client's occupancy after about 15–30 s with
  no heartbeat, whatever the client does. This covers Jr on watchOS 26 (xavxx), watchOS 27's
  no-quit behaviour, and dropped mobile links.
- **Door and child trust:** re-test `06b3a564` (child trusts the door's `X-Forwarded-For`) and
  `c4a5beeb` (shared-dial limit enforced on spectrum sockets) against the legacy fixture, with a
  leftover Jr session from the same public IP present.

## 10. Done when

- Every version location reads 11, and `check-versions.mjs` fails the suite on any mismatch.
- V11 clients send `proto`, and the server and directory publish `proto` and `minProto`.
- The legacy fixture either gets the full core or the §6 message on all three paths, never a
  bare 1006.
- A radio with a raised `minProto` is greyed out in the V11 picker and directory, and absent for
  a legacy requester.
- `publish-apt.sh` is gated on `scripts/compat/`.
- `docs/PROTOCOL.md` exists with the `proto = 1` core written down.
