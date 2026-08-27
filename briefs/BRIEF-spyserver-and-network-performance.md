# VibeSDR — Network Streaming Performance + SpyServer Protocol (Server & Client)

**Handoff brief for Claude Code — July 2026**
Repo: `Stuey3D/VibeSDR` (main). Targets the existing local-hardware / rtl_tcp infrastructure.

---

## Background & goals

VibeSDR (Android) can already share a USB RTL-SDR over the LAN as an rtl_tcp server
(`android/app/src/main/cpp/rtl_tcp_server.cpp`, exposed via `VibeLocalSdrModule.kt` /
`RtlTcpServerService.kt` / `src/services/rtlTcpServer.ts`), and can consume rtl_tcp as a
client via `LocalSdrShim::startTcp()`.

Field testing (Moto G35 serving rtl_tcp over an iPhone personal hotspot, 5 GHz) showed
audio breakup caused by WiFi power-save stalls on the server phone: the service holds a
`PARTIAL_WAKE_LOCK` but **no WifiLock**, so the radio naps between beacons, the 4 MB
drop-newest client queue overflows, and IQ discontinuities reach the client.

This brief covers four phases:

- **Phase 0** — network performance fixes for the existing rtl_tcp paths (WifiLock,
  socket buffers, drop visibility). Ship independently; small and low-risk.
- **Phase 1** — a shared SpyServer protocol module used by both server and client.
- **Phase 2** — SpyServer-compatible **server** mode (alongside rtl_tcp server).
- **Phase 3** — SpyServer **client** backend adapter (connect to any SpyServer).

Rationale for SpyServer: raw rtl_tcp at 2.4 MSPS is ~38 Mbit/s sustained; SpyServer's
server-side decimation brings a typical narrowband session down to single-digit Mbit/s.
Building the server first also gives us a local reference instance for developing the
client.

> **UPDATE 2026-07-09 (post-Phase-0).** The "makes phone-hotspot use viable" half of
> that rationale is WRONG and has been removed. Phase 0 shipped and full-rate
> 2.4 MSPS now runs cleanly over an iPhone personal hotspot: the limit on WiFi was
> *stalls*, not bandwidth, and the WifiLock + client jitter buffer fixed them. An
> early "15.8 MB dropped" reading was misread as a sustained deficit — `droppedBytes`
> is cumulative since client connect, and 15.8 MB is only ~3.3 s of stream across a
> whole session. Decimation remains strongly motivated by **cellular uplink** (the
> remote-node case), power, marginal links, and SDR#/SDR++ compatibility. See §2.6.

---

## Phase 0 — Network performance fixes (rtl_tcp, both directions)

### 0.1 WifiLock in the server service

In `RtlTcpServerService.kt`, alongside the existing `PARTIAL_WAKE_LOCK`
(`acquireWakeLock()` / `releaseWakeLock()`), acquire and release a WifiLock with the
same lifecycle:

- API ≥ 29: `WifiManager.WIFI_MODE_FULL_LOW_LATENCY` (explicitly disables WiFi
  power-save while held; this is the primary fix for the observed dropouts).
- API < 29: fall back to `WIFI_MODE_FULL_HIGH_PERF` (deprecated but functional).
- Tag: `"VibeSDR:RtlTcpServer"`. Use `setReferenceCounted(false)`. Release in the same
  paths that release the wake lock (`onDestroy`, stop action). Never leak on crash —
  wrap releases in try/catch as the wake lock code already does.

### 0.2 WifiLock on the Android client path

When `LocalSdrShim::startTcp()` is driving a session on Android (network IQ into the
local DSP pipeline), the same power-save exposure applies on the receiving side.
Acquire the identical WifiLock in the Kotlin layer for the duration of a TCP local
session (acquire on successful `startTcp`, release on `stop`). iOS needs nothing —
there is no equivalent power-save toggle and no API for it.

If the local-session code path also runs as a foreground service, put the lock there;
otherwise hold it from `VibeLocalSdrModule.kt` keyed to session lifecycle. Inspect the
current session ownership and pick the narrowest correct scope.

### 0.3 Socket buffer sizing in `net_shim.cpp`

`net_shim.cpp` already sets `TCP_NODELAY` on both accepted and outgoing sockets (keep
it). Add:

- Server (accepted client socket): `SO_SNDBUF` = 1 MB. A larger kernel send buffer
  rides through brief radio stalls before the userspace queue starts dropping.
- Client (outgoing socket in `startTcp` path): `SO_RCVBUF` = 1 MB.

Set before/immediately after connect/accept. These are requests, not guarantees
(kernel may clamp); do not treat failure as fatal, just log.

### 0.4 Drop visibility (server)

In `rtl_tcp_server.cpp`:

- Add `std::atomic<uint64_t> droppedBytes{0}` to `Impl`; increment by `len` in
  `fanoutIq()` where the queue-cap drop happens (the `return; // drop` branch).
- Extend `RtlTcpServer::Status` with `droppedBytes` (total since client connect; reset
  on new client). Plumb through the JNI in `vibe_localsdr_jni.cpp`,
  `VibeLocalSdrModule.kt` (`getServerStatus`), and the `ServerStatus` type in
  `src/services/rtlTcpServer.ts`.
- `RtlTcpServerScreen.tsx`: the screen already polls status. Show a warning row when
  `droppedBytes` is increasing between polls, e.g. "⚠ Network can't keep up — try a
  lower bandwidth setting or move closer to the router", plus a cumulative counter
  (human-readable, MB). Green/quiet when zero.

### 0.5 Acceptance criteria (Phase 0)

- Server survives ≥ 30 min screen-off streaming at 2.4 MSPS on a 5 GHz link with zero
  dropped bytes (previously failed within seconds/minutes without the WifiLock).
- Dropped-bytes counter demonstrably increments when the link is genuinely
  constrained (e.g. force 2.4 GHz at range) and the UI warning appears.
- No wake/WiFi lock leaks: locks released on stop, service destroy, and client detach;
  verify with `adb shell dumpsys power` / `dumpsys wifi`.

---

## Phase 1 — Shared SpyServer protocol module

### 1.1 Licensing ground rules (read first)

- The SpyServer **software** is closed-source freeware; its **wire protocol** is a
  functional interface with public, community-documented behaviour. We implement the
  protocol clean-room: original code written from protocol behaviour and public
  documentation, never from Airspy binaries.
- SDR++'s `spyserver_protocol.h` and client source (GPLv3) may be consulted **as
  protocol documentation only** — message IDs, setting enums, struct layouts, handshake
  order. **Do not copy code.** VibeSDR is GPL-3.0 **with `APPSTORE-EXCEPTION.md`**, and
  Stuart can only grant that exception for code he holds copyright to; incorporating
  third-party GPL code would break App Store / Play distribution rights. All protocol
  code must be original.
- Trademark: "SpyServer" and "Airspy" are Airspy's marks. All UI, store listing, and
  docs wording must be nominative: "SpyServer-compatible", "speaks the SpyServer
  protocol" — never present the feature as *being* SpyServer or imply endorsement.

### 1.2 Module layout

Create `android/app/src/main/cpp/spyserver/` (shared by server and client; the client
side must also build into the iOS static lib like the rest of `LocalSdrShim`):

- `spyserver_protocol.h` — protocol version constant, command IDs (hello, get setting,
  set setting), setting IDs (streaming mode, IQ format, IQ decimation, IQ frequency,
  gain, digital gain, etc.), message types (device info, client sync, IQ data, FFT
  data), stream format enums (u8 / int16 / float32 IQ), and the packed header/body
  structs. Derive names and values from observed protocol behaviour and public
  references; document each with a comment stating what it does on the wire.
- `spyserver_messages.cpp/.h` — serialization/deserialization helpers, endian-safe,
  with unit-testable pure functions (no sockets).
- Target the de facto protocol version spoken by current SDR# / SDR++ releases.
  Handshake must tolerate minor-version differences the way SDR++ does.

### 1.3 Acceptance criteria (Phase 1)

- Header compiles into both the Android JNI target and the iOS static lib build
  (`modules/vibe-local-sdr/build_ios.sh`).
- Round-trip unit tests: every message type serializes → deserializes to identity.
- A recorded handshake from a real SpyServer instance parses correctly with the
  deserializer. Capture this early: stand up the official SpyServer binary on the
  Raspberry Pi 5 (see Phase 3 test rig) at the START of Phase 1 and record a
  handshake + short IQ session with tcpdump/Wireshark — this pcap is the ground
  truth for the whole protocol module and prevents loopback-blind spec errors.

---

## Phase 2 — SpyServer-compatible server (Android)

### 2.1 Shape

Mirror the rtl_tcp server's architecture, which is proven: async USB reader thread →
fan-out → per-client bounded queue (drop-newest, never block the USB callback) →
dedicated writer thread per client. New files
`android/app/src/main/cpp/spyserver_server.cpp/.h`, registered through the same JNI /
`VibeLocalSdrModule.kt` / foreground-service plumbing as the rtl_tcp server. Default
port 5555. mDNS advertisement via the existing `src/services/mdns.ts` mechanism, new
service type alongside the rtl_tcp one.

The Phase 0 WifiLock and drop-counter work applies identically here — factor the
lock handling in `RtlTcpServerService.kt` so both server types share it (either one
service with a mode, or a shared base; prefer one service with a protocol mode enum to
avoid a second FGS declaration).

### 2.2 Core behaviour

- **Handshake:** accept client hello (protocol version + client name string), respond
  with device info (advertise device type appropriately for an RTL-SDR: sample rate,
  tunable range, gain stages, resolution) and client sync (control state, tuner
  ownership, current frequency/gain).
- **Decimation chain:** the heart of the feature. Client requests a decimation stage
  (power-of-two divisor of the device rate); server runs cascaded halfband decimation
  filters before transmit. Implement the halfband stages with VibeDSP NEON primitives
  (`android/app/src/main/cpp/vibedsp/`) — reuse existing filter kernels where they fit;
  add a dedicated halfband decimator if none exists. Budget: full chain at 2.4 MSPS
  input must be comfortably < 15% of one core on a Moto G35-class SoC.
- **Stream formats:** support at minimum u8 and int16 IQ output; float32 optional.
  Honour the client's requested format from settings.
- **Retuning:** IQ frequency setting retunes the dongle (serialize rtlsdr calls under a
  dev mutex exactly as `applyCommand()` does today).
- **Multi-client policy:** SpyServer is single-tuner. Phase 2 policy: first client gets
  tuner control; additional clients may connect read-only at the current
  frequency/decimation (matching stock SpyServer's model) — if that materially
  complicates v1, restrict to single client like the rtl_tcp server and document it.
  State the chosen policy in the UI.
- **FFT stream: REQUIRED in v1** (decided 2026-07-09, was "defer if acceptable").
  Stock SpyServer sends a separate spectrum stream — server-computed FFT, raw
  8-bit magnitude bins, NOT compressed and NOT decimated IQ — so the client draws
  a full-width waterfall over the whole 2.4 MHz while the IQ stays narrow. A
  2048-bin frame at 20 fps is ~40 KB/s. This is what makes the session *feel* like
  rtl_tcp to the user; without it a remote node is a keyhole and cannot be
  scanned, which defeats the point of a low-noise remote receiver. VibeDSP already
  has the FFT machinery. Note UberSDR compresses its spectrum stream and SpyServer
  does not — sending compressed bins would work in loopback and fail against every
  real SDR#/SDR++ client. Take the exact bin count, dB scaling and message layout
  from the pcap, not from inference.

### 2.3 UI

`RtlTcpServerScreen.tsx` becomes a "Share this SDR" screen with a protocol selector:

- **SpyServer-compatible (recommended)** — "Low bandwidth — works over hotspots and
  mobile data. For VibeSDR, SDR#, SDR++ clients."
- **RTL-TCP (maximum compatibility)** — "Raw full-rate IQ — needs a fast local
  network. Works with virtually all SDR software."

Keep the existing name, port, and bandwidth-override controls; add the drop/health
indicator from Phase 0 to both modes.

Also on this screen (agreed 2026-07-09):

- **Bind scope** — LAN-only by DEFAULT; "allow connections from any network" is a
  deliberate, explained choice. The SpyServer protocol has NO authentication:
  anyone who reaches the port gets the tuner, and since it is single-tuner they
  can lock the owner out. The UI must say so plainly rather than imply we handle
  it. A VPN is the authentication for most users.
- **Reachable addresses** — list the addresses the server is actually bound and
  reachable on, including any GLOBAL IPv6. Costs nothing and quietly answers the
  cellular question: carriers that hand out routable IPv6 make a remote node
  directly reachable, with no tunnel.
- **Persist toggle** — default is an ad-hoc, one-shot share (server dies with the
  screen, as today). The toggle makes it survive screen exit, WiFi changes and
  reboots for set-and-forget nodes. See §2.5.

**Explicitly OUT of scope: tunnels, DDNS, relays, accounts.** Each is an endless
support burden and none is VibeSDR's problem. Docs get a sentence instead:
"Serving over the internet requires a reachable address — a public IP, a VPN such
as Tailscale, or a tunnel. Mobile networks usually place you behind carrier NAT,
where inbound connections are not possible."

The picker's **existing manual add-server-by-address** path (the add-RTL-TCP modal,
`InstancePickerScreen.tsx` ~line 653: friendly name + `host` / `host:port`, Connect
or Save & Connect) is what makes remote nodes usable, and it must gain a SpyServer
option. mDNS is multicast and does NOT traverse layer-3 VPNs (Tailscale/WireGuard),
so a VPN-connected remote receiver is routable but never discoverable — manual add
is the only way in. It also serves static IPs and tunnels. Nothing new to build:
extend what predates mDNS.

### 2.5 Persist mode (set-and-forget nodes)

Motivating case: an old Android + RTL-SDR at the base of an antenna, solar
powered, remote (allotment), reached over WiFi or a VPN. Independent of the
SpyServer protocol — build and test it against rtl_tcp FIRST, then Phase 2
inherits a service that already survives.

- The server's lifecycle is currently tied to `RtlTcpServerScreen`'s mount
  (leaving the screen stops it and frees the dongle). Persist mode must break
  that coupling — a real refactor of ownership, not a flag.
- `RECEIVE_BOOT_COMPLETED` + boot receiver to restart the FGS.
- Battery-optimisation exemption, or Doze eventually kills it (reuse the v6 Moto
  background-restriction detection).
- Re-advertise mDNS when WiFi drops and returns — the IP may have changed.
- Rebind/recover on interface change rather than dying.

### 2.6 Why Phase 2, revisited (2026-07-09)

The brief's original rationale — that raw rtl_tcp is too heavy for a phone
hotspot — **did not survive testing.** After the Phase 0 WifiLock + client jitter
buffer, a full-rate 2.4 MSPS session runs cleanly over an iPhone personal
hotspot. Stalls, not bandwidth, were the reliability limit on WiFi.

What decimation is actually for:

- **Cellular uplink.** ~600 kbit/s all-in for a narrowband session + FFT stream,
  vs 38 Mbit/s for raw IQ. This is the case where decimation *enables* something
  rather than optimising it — the remote-node story above depends on it.
- **Power** on a solar node (pushing 4.8 MB/s over WiFi all day is not free).
- **SDR# / SDR++ compatibility**, which is a feature regardless of bandwidth.
- Marginal links generally (a mast-base node on a weak WiFi path).

Note the jitter buffer and WifiLock are INHERITED by, not superseded by, the
SpyServer path: decimation shrinks the stream but does nothing about a 270 ms
WiFi stall — you would just lose 270 ms of a smaller stream.

### 2.7 Acceptance criteria (Phase 2)

- **Cross-validation is mandatory — VibeSDR↔VibeSDR loopback is NOT sufficient** (a
  spec misreading would be invisible in loopback). The server must be exercised by:
  - SDR++ (desktop) as a client: connect, retune, change decimation, change format,
    stream ≥ 10 min clean audio.
  - SDR# (Windows) as a client: same pass.
- Bandwidth sanity: real-world stock SpyServer reference figures are ~120 KB/s for a
  WFM session and ~38 KB/s for narrowband modes per client (vs ~4.8 MB/s raw rtl_tcp
  at 2.4 MSPS). Our server should land in the same order of magnitude for comparable
  settings.
- CPU: decimation chain ≤ 15% of one core at 2.4 MSPS on the Moto G35; no thermal
  runaway over 30 min.
- Clean behaviour on client disconnect/reconnect and on USB detach mid-stream.

---

## Phase 3 — SpyServer client backend adapter

### 3.1 Shape

Follow the established adapter pattern (`src/services/KiwiAdapter.ts`,
`OwrxAdapter.ts`, `FmdxAdapter.ts` + `SDRBackend.ts` interface). Heavy lifting in
native: extend `LocalSdrShim` with a `startSpyServer(host, port, ...)` entry mirroring
`startTcp()` — IQ (decimated, from the network) feeds the existing FFT + demod
pipeline, so demodulation, decoders, NR, squelch, and the audio engine all work
unchanged on both Android **and iOS** (like `startTcp`, no USB dependency).

- Request decimation appropriate to the current view/mode rather than full rate;
  re-negotiate on user bandwidth changes (this is where the bandwidth win comes from).
- Handle server-side tuner ownership: if another client holds control, surface
  read-only state in the UI rather than failing silently.
- Instance picker / deep links: add `spyserver://host:port` handling alongside the
  existing schemes in `src/linking/DeepLinkHandler.ts`, mDNS discovery of
  SpyServer-compatible servers (including our own), and favourites support.

### 3.2 Test rig

- Primary reference target: official SpyServer ARM Linux binary on Stuart's
  **Raspberry Pi 5** with an RTL-SDR (RTL-SDR device type set in the config). The
  client must pass against the **real Airspy binary**, not only our Phase 2 server.
- Lightweight protocol validation (no local hardware needed): public SpyServer
  instances from the Airspy server directory — fine for handshake/tune/stream
  testing, but do NOT exercise gain, bias-T, or disconnect edge cases on someone
  else's public server; that testing belongs on the local Pi.
- Also validate against the Phase 2 server (which by then has itself been validated
  against SDR#/SDR++), closing the cross-validation triangle.
- Note: SDR++ Brown's built-in server speaks SDR++'s own protocol, NOT SpyServer —
  it is not a valid reference target for any phase.

### 3.3 Acceptance criteria (Phase 3)

- Connects to stock SpyServer (Pi/VM): device info parsed, tuning works, ≥ 10 min
  clean audio on NFM and AM, decimation changes applied without stream restart
  glitches beyond what stock clients exhibit.
- Connects to VibeSDR's own server (Phase 2) with identical behaviour.
- iOS build passes (static lib via `build_ios.sh`); adapter works on iPhone against
  both servers.
- Runs acceptably over an iPhone personal hotspot and over mobile data at moderate
  decimation — the motivating use case.

---

## Naming & store-listing guidance (applies to all phases)

- Feature names in UI: "SpyServer-compatible server", "Connect to SpyServer".
- Store listing phrasing: "Compatible with the SpyServer protocol used by SDR# and
  SDR++." Never "includes SpyServer" / "VibeSDR SpyServer".
- Add a line to the About/licences screen noting SpyServer is a trademark of its
  respective owner and VibeSDR is unaffiliated.

## Suggested implementation order

1. Phase 0 (ship immediately — fixes a live user-facing defect).
2. Phase 1 + 2 together (server needs the protocol module; validate against SDR#/SDR++).
3. Phase 3 (client, validated against the Pi/VM reference and the Phase 2 server).

73!
