# SINGLE MODE: SATELLITES — the watch view and the linear-transponder hybrid (beta96, branch b96-satsingle)

Stuart 11-26 (via the coordinator). The server half of "Single mode: SATELLITES" was built on b96-keeprun
(docs/v12/KEEP-RUNNING.md → "Single mode: SATELLITES"): `singleMode:"sat"` in hwinfo, the scheduler runs the passes with
listeners on, `singleSatRefuses` holds listeners. This branch finishes it.

## RESUME HERE

| Piece | Where | State |
|---|---|---|
| Watch facts `sat_single` (pass now / next, the radio's timetable ≤ 12 in 24 h, last pass, scheduler state, the hybrid) | shim `satSingleJson` / `satSingleBroadcast`; sent in `sendFullState`, with every `sat_list` answer, at AOS and LOS | built, **e2e tested** |
| The pass as the room's pick (a watcher's box opens on it and joins its decoder) | `satSinglePassPick`, `satSingleTellPickAll` at AOS, `satTellRoomPick` for arrivals; LOS sends `sat OFF pickGone single` | built; the Meteor box joining the LRPT decoder is **headless-web only** (no real pass) |
| Linear-transponder passes on a SATELLITES radio | `vibe::sat::passHybrid` (vibe_sat_engine.h); the scheduler's `consider` takes them only when `singleSatNow()` | built, e2e tested (RS-44) |
| THE HYBRID: locked range over the passband ± Doppler | `satHybridBeginLocked` / `satHybridEndLocked` (g_vsLockedCentre / g_vsLockedRate saved and restored), `satPassWhyNot` lets the session run on it, the tick holds the radio still | built, e2e tested |
| Switching connected listeners | `sat_hybrid` + a notice, then their spectrum + audio sockets closed ~2 s later (`satHybridCycleTickLocked`); clients reconnect in 3 s through the ordinary accept path | built, e2e tested (the test reconnects by hand; the web / app reconnect timers are the existing ones) |
| A joiner / a reconnect during the hybrid | accept path: the ClientDsp at the passband centre; `satHybridJoin` → the ordinary per-listener `satPick` (AUTO DOPPLER, the transponder finder) | built, e2e tested |
| Refusals during the hybrid | `singleSatRefuses`: tune inside the window (and usb / lsb / cw), bandwidth, `sat` doppler / lock / pick of this bird pass; outside the window → notice with the range; the radio (gain …) still refused | built, e2e tested |
| Shared client rules | `src/services/satSingle.ts` | built, `test_sat_single` 32/32 |
| Web watch view | `web/client/src/satellites.ts` (paintWatch, setWatch, onSingle, onHybrid, exit refuses), `spectrum.ts` (sat_single / sat_hybrid / singleMode), `main.ts` (satSingleSync, toggleSatMode refuses, GALLERY) | built, `test-web-sat-single` 20/20 (headless Edge on the built page) |
| App watch view | `SatellitePanel.tsx` `watch` prop, `SDRScreen.tsx` (hwSingleMode, satSingle, satHybrid, satModeOff refuses), `VibeServerClient.ts` / `VibeServerWsClient.ts` / `sdrProtocol.ts` | built, **tsc only — not run on a device** |
| Owner's transponder picks | Setup page Unattended satellites → Linear transponders (`SAT_XPDR_DOWNLINKS`, only on a SATELLITES radio), `backgroundCapsJson` `satXpdrPasses`, the app's server screen (`BackgroundDecodingSetup satSingle`) | built; page tested (test-setup-satpass 23/23, test-sat-engine §13b); app screen **tsc only** |
| Test-only fake pass clock | `VIBESERVER_SAT_TEST_CLOCK` (a file of seconds added to the SATELLITE clock only; read ≤ 1/s) | built |
| Copy | About beta96 note, web tutorial's SATELLITE card | done; sdrTour / pickerTour / website do not describe SATELLITE's keys (checked) |
| `vibe_web_page.h` | rebuilt via the lock at the end of this branch | regenerate at the merge if web/ moves again |

## The watch view (Stuart's item 1)

- A listener on a SATELLITES radio (hwinfo `singleMode:"sat"`, not signed in as admin) is put into SATELLITE as soon as the
  page / app knows (`satSingleSync` on every hwinfo and admin change; the app's effect on `satWatch`).
- The box: the header "NEXT · RS-44 · Transponder · AOS 12:29 (in 0:04) · max 46°" or "LIVE · Meteor-M2-3 · LRPT · 11:59 left",
  a line saying what to watch (the picture builds below / packets / telemetry / between passes: the timetable and GALLERY),
  "Last pass: …" in the server's words, GALLERY, and THIS RECEIVER'S PASSES (the owner's picks, the running one marked).
- No way out: EXIT is not drawn ("SATELLITES only on this receiver" in its place, the why on hover / accessibility), the
  all-satellites list is not drawn, AUTO DOPPLER is hidden except during a hybrid, `exit()` / the mode key / the app's
  `satModeOff` refuse in words and send nothing. The server refuses anyway (`singleSatRefuses`).
- During a pass the server's `picked` (shared) opens the ordinary pick box: half-globe, countdown, the LRPT picture or the
  packets — the existing box, joining the server's decoder run.
- The admin: `setWatch(false)` — the ordinary SATELLITE, EXIT included.
- Pad: everything new is an ordinary button in the box (GALLERY) or text; the box's keys are reachable as before.

## The hybrid (Stuart's item 2) — the design and why

At AOS of a linear transponder pass on a SATELLITES radio with room for more than one listener (not an Icom / SpyServer):
1. The pass starts as any other (tune to the passband centre, the shared Doppler lock).
2. `satHybridBeginLocked`: the window = passband ± (2.7e-5·f + 2.5 kHz) — RS-44 435.596–435.684 MHz. If the capture does not
   hold it, the hardware is centred on it (or, wider than the capture, the hybrid is skipped and the pass runs on one dial).
   The shared lock stops. `g_vsLockedCentre` = the hardware centre, `g_vsLockedRate` = the rate (saved first) — so
   `perClientDsp()` is true and EVERYTHING per-listener is the existing locked-range machinery.
3. Every listener gets `sat_hybrid` + a notice, and ~2 s later their sockets are closed. They reconnect (3 s, the clients'
   own timers) and the accept path builds their ClientDsp at the passband centre; `satHybridJoin` gives them their own pick
   (`satPick` → tune, per-listener Doppler lock, the transponder finder). A joiner mid-pass is the same path.
4. They tune anywhere in the window (the lock follows: a transponder lock keeps the dial where they hear it, its nominal is
   the satellite frame); outside is refused with the range.
5. LOS (`satPassEndLocked` → `satHybridEndLocked`): every per-listener lock and pick ends, the locked centre / rate go back
   (0 on a shared-dial radio), `sat_hybrid` off + a notice, sockets recycled again → one dial, watch only.

**Why reconnect, not a live switch.** A listener's ClientDsp, and whether their audio socket feeds from the shared stream or a
channel, are decided in the accept path (~200 lines: adoption, the session VFO memory, the admin flag, the turn clock,
pending audio). Re-running that under live sockets would be a second copy of it. Closing the sockets makes the clients take
that path again; it is what a change of the radio's arrangement already means to every client, and old clients need nothing.
Cost: ~5 s without audio at AOS and at LOS.

**Why not "per-listener DSP always on" for a SATELLITES radio** (the coordinator's preferred option, weighed): on a locked
range the shared pipeline is not fed (`if (!perClientDsp()) rx.feed(...)` throughout the DSP loop), and every watch-only pass
decoder runs on the shared pipeline — Meteor LRPT, SSTV, APRS, telemetry, the ORBCOMM sky plot rider. Always-on would have
needed a second, listener-independent feed for them, and each watcher would pay a private channel all day for audio that is
the same for everyone. Measured below: the per-listener cost is real.

## CPU (Mac M5, fake rtl_tcp at 2.4 MS/s, the server process, 15 s each, 3 listeners with spectrum sockets, no audio sockets — tests are silent)

| state | CPU |
|---|---|
| hybrid: 3 own VFOs + per-listener Doppler + the transponder finder each | 5.0–6.7 % of one core (two runs) |
| watch-only, the same 3 listeners on the one dial | 1.9–3.5 % of one core |

Not measured: with audio sockets (Opus per listener), on a Pi / phone. The per-listener channel is the existing locked-range
cost (docs on locked ranges), so a Pi 2 with many listeners on a transponder pass costs what a locked range with that many
listeners costs.

## Tests (all silent, all synthetic)

| Test | Result | What |
|---|---|---|
| `scripts/test-server-sat-single.mjs` | 33/33 | **e2e on the Mac binary**, fake rtl_tcp + `VIBESERVER_SAT_TEST_CLOCK`: RS-44 found, the server placed under it; before AOS: hwinfo sat, sat_single (timetable, no pass now), a tune refused, sat_list → sat_single; AOS: the hybrid's log + window, sat_hybrid to both, sockets recycled, reconnected → own pick + own Doppler each at the centre, independent tuning (each lock's nominal its own dial, Doppler per listener), outside refused in words, gain still refused, a joiner mid-pass at the centre; CPU; LOS: hybrid off, pickGone, sockets recycled, watch-only again (last pass named), a tune refused |
| `scripts/test_sat_single.ts` | 32/32 | the shared rules |
| `scripts/test-web-sat-single.mjs` | 20/20 | the web box in headless Edge on the built page, desktop + 390 px |
| `vibeserver/test-sat-engine.cpp` | 208/0 | + §13b the page's transponder list = the engine's passHybrid; beacons / LRPT / FM are not hybrids |
| `scripts/test-setup-satpass.mjs` | 23/23 | + the transponder rows only on a SATELLITES radio, a tick stored in satPasses |
| `check-android-settings-parity` | 214/0 | (+ the newModesPol fix on this branch) |
| tsc (4 GB) | clean on src/ + web/client/src | the only errors are handheld-ui/test and scripts/wefax-world node types (environment) |

Not tested: a real pass of anything; the app's watch view on a device; the web page's own reconnect after the server closes
(the e2e reconnects by hand — spectrum.ts / audio.ts reconnect 3 s after any server close, unchanged); a SATELLITES radio on
Android (the engine is the same shim; gradle not run).

## Live checks owed (the coordinator, on Stuart's say-so)

1. A SATELLITES radio (Setup → Modes and decoders → Single mode: SATELLITES; Unattended satellites on, Meteor + RS-44
   Transponder ticked). A listener's page: lands in SATELLITE, NEXT counts down, no EXIT.
2. A Meteor pass: LIVE, the picture building in every watcher's box; at LOS "Last pass: …", the gallery has it.
3. An RS-44 pass with two listeners (web + app): both reconnect at AOS within ~5 s, each hears the passband centre, each
   tunes their own SSB contact with Doppler held; a tune outside says the range; at LOS both are back to watching.
4. The admin page during the hybrid: the scheduler line says "listeners have their own VFO across the passband until LOS".

## Open decisions for Stuart

- The switch costs ~5 s without audio at AOS and at LOS (the reconnect). OK, or worth a live switch later?
- Is the hybrid window passband ± ~14 kHz (RS-44) right, or should listeners be held to the passband itself?
- During the hybrid listeners may use USB, LSB or CW only — FM / AM are refused. OK?
- Between passes the box shows only the radio's own timetable (not every satellite). OK, or also the full list greyed?
