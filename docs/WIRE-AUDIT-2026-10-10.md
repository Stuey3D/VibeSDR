# Wire audit — what a listener is sent, and what it costs (2026-10-10)

Stuart: *"make the connection as efficient as possible, minimal data rate possible"* · *"needs to be light everywhere —
light on CPU on client and server and light on data"* · *"audit it and do it properly please"*.

## Method

A silent listener (`wiremeter.mjs` — a spectrum socket that never sends anything but `ping`) counts every message by
type: rate, bytes, size. Measured live on the XCover (rc28, DAB, BBC National / Coventry) and on a local server built from
this tree, replaying the DAB bench clip (12D) through `fake-rtl-tcp.mjs`.

## What the XCover sent one listener, in DAB (before)

| message | rate | data | notes |
|---|---|---|---|
| **`dab` report** | 2.1/s | **32 KB/s (16 KB each)** | **74 % of everything** |
| spectrum frame (binary) | 10/s | 10 KB/s | the waterfall itself |
| `sig` | 10/s | 0.4 KB/s | the S meter |
| `adc` | 10/s | 0.4 KB/s | a figure recomputed once a second |
| `lx` | 10/s | 0.3 KB/s | lightning rate per *minute*, almost always 0 |
| everything else | <1/s | <0.2 KB/s | hwinfo, config, health, dial, users |

Inside one 16 KB report: the **station list was 12 KB**, the constellation (`iq`) 1.3 KB and impulse response (`ir`)
0.4 KB, the licensed sites 0.4 KB. The list cannot simply be "sent on change": each row carried `dlsAge`, an **age**,
so it differed in 55 of 58 consecutive reports (measured, 11D). Its fixed fields change only while the FIC is being read.

## What changed

### 1. The DAB report in pieces (`dab=2`)
A client that opens its spectrum socket with `&dab=2` gets:

- **`dab`** — the live report (`v: 2`, naming `listRev`/`dlsRev`), without the list, data services, licensed sites
  or scopes;
- **`dab_list`** — every service's fixed fields + data services + licensed sites + block names, **only when it changes**;
- **`dab_dls`** — every service's radio text (+ DL Plus for the tuned one) with its age, **only when a text changes**;
  the client ages it between sends (no clock shared between the machines);
- **scopes** (`ir`, `iq`) — **only while the client says its Signal pane is open** (`dab_scopes`).

`dab_resync` asks for the pieces again (a client whose revisions do not match, at most every 5 s); leaving DAB makes
every socket start the next session from a full list. Server: `vibe_dab_service.h` `ReportParts` (built in the SAME pass
as the legacy report — one builder), `local_sdr_shim.cpp` `sendDab`. Clients: `src/services/dabAssemble.ts`, shared by
the app (`VibeServerClient`) and the web (`spectrum.ts`); it hands back **exactly the legacy shape**, so no screen code
changed.

**Compatibility, both directions:** a socket without `dab=2` (Jr, every app and page before this) gets the legacy report
byte-for-byte; a new client on an old server receives full reports, which the assembler passes straight through.

Measured, local server, bench multiplex: **17.3 KB/s → 3.4 KB/s** for the report (the list and texts add a few KB only
when they change). On the XCover's real multiplexes, where the list was 12 KB of a 16 KB report, the cut is larger.

### 2. `adc` and `lx` on change, or every 2 s
Both were sent on every frame. Now on a change or every 2 s: **20/s → ~1/s** (adc) and **20/s → 0.5/s** (lx) per
listener. The 2 s heartbeat is required: the web client hides the ADC readout after 5 s without one.

### 3. Earlier the same night
- **Decoder socket** (`/ws/dxcluster`) only while a decoder / spots / chat runs; closed 8 s after the last (it was held
  for the life of the page, carried another listener's mirrored decoder output, and was cut by the tunnel every ~100 s).
- **Server bookmarks**: gzip'd (250 KB → 12 KB on the wire), ETag → 304 when unchanged, fetched only on load / search /
  opening the bookmarks — never on a timer.
- **Idle decoder sockets pinged** (25 s) so a tunnel does not cut them.
- **Dead logos forgotten page-wide** (the same 404s were re-requested every few seconds).

## Look and feel — nothing on screen changes (Stuart: "I dont want to compromise on look and feel")
- The DAB panel receives the identical report (the end-to-end test asserts the list, texts, blocks, data services and
  licensed sites equal the legacy report's).
- The scopes still refresh twice a second whenever they are on screen, and arrive the moment the Signal pane opens
  (~100 ms, measured) — the server answers `dab_scopes` at once instead of at the next tick, so the box is never empty.
- `sig`, the waterfall and every rate a listener chose are untouched; the ADC readout still updates on every change; a
  radio text's "x s ago" keeps counting between sends on the client.

## Deliberately left as it is

- **`sig` every frame** — it is the S meter; its responsiveness is a documented requirement (2026-08-05, "making the
  SNR bar more responsive"). ~0.4–0.8 KB/s.
- **The spectrum frame** — now the largest stream (≈1 KB a frame at the listener's own fps). It is the waterfall; its
  rate is already the listener's choice (data saver / waterfall rate).
- **The live DAB report (~1.6 KB)** still carries every diagnostic counter, most of which only the Signal pane shows. A
  second cut (counters only while the pane is open) is possible; it needs every reader of each field listed first
  (meter, VTS, station list, Buddy relay) — not done tonight.
- **Jr** does not send `dab=2`: it keeps the legacy report until its parser is taught the pieces.

## Tests
`test-dab-report-pieces.mjs` (real server + bench clip, 14 checks: legacy unchanged, reassembled == legacy, bytes,
scopes, resync, new session) · `test_dab_assemble.ts` (14) · `test_decoder_socket_idle.ts` (14) ·
`test-bookmark-import.mjs` (18) · `test-dx-keepalive.mjs` (3).
