# Share ADS-B with flight trackers, and the ADS-B Beast feed radio (V12 beta96)

Stuart, 2026-11-26: feed ADS-B to flight trackers, built "like we did for PSKReporter". Paul (PE1RJV) has a dedicated
ADS-B radio and wants to feed aggregators such as map.flights. Stuart runs PiAware on a Raspberry Pi 3 and must keep his
FlightAware Enterprise account, which depends on feeding FlightAware. Then (same day) Stuart added the **ADS-B Beast
feed radio**: a radio entry of its own that reads another receiver's Beast output, so his Lenovo VibeServer can show the
aircraft his PiAware Pi hears (192.168.86.12:30005) without touching that Pi.

Branch `b96-adsbfeed`, off v12-experimental (beta95).

## RESUME HERE

- **Built, tested and committed:**
  - the Beast encoder and reader
  - the local Beast server
  - push feeds with presets, UUID, reduce and the loop guard
  - the Beast input client
  - the Beast feed radio (`driver: "beast"`), server side
  - the Setup page: the card, ADD RADIO and the Beast radio's own tab
  - the admin page rows
  - the Android server-screen card and its plumbing
  - the About note

  The test list is below.
- **Not yet built or verified:**
  - **Gradle / Android build not run.** The JNI, Kotlin and TSX changes were type-checked and covered by the parity
    script only.
  - **The web client and the app with no spectrum at all.** The Beast radio sends no waterfall frames. Listeners land on
    the ADS-B map through the existing *only mode* (hwinfo `onlyMode:"adsb"`, which hides EXIT). The server side is
    proven by `test-server-beast-feed`: the reports arrive, EXIT is refused, and a tune changes nothing. The browser
    itself has **not** been driven headless against a Beast radio. Check whether the web client's or the app's spectrum
    watchdog reconnects when no binary frames ever arrive.
  - **The Linux systemd path.** A `vibeserver@beast-1` unit starts the radio by serial, the same as any radio, and
    main.cpp's `beastRadioFor` takes it before any USB detection. This was tested with a front door plus spawned
    processes on macOS, not under systemd.
  - **A save from a Beast radio's tab.** `stashRadio` copies the SDR form fields into the entry. They are harmless keys
    for a Beast radio, but a full Setup save from that tab has not been run end to end.
  - **The SIMPLE-mode path** (one process, primary radio = Beast) is coded (`beastRadioFor`) but not run. The tests ran
    the Full layout.
- **Never sent to a real aggregator or read from a real receiver** (by the rules). Every test uses 127.0.0.1 sockets.
  The live checks a person needs to make are under "Live checks" at the end.

## What is sent (Beast)

`android/app/src/main/cpp/vibe_beast.h`. The framing is the Mode-S Beast binary format. Each frame is:

- `0x1A`;
- a type byte: `'1'` Mode A/C (2 bytes), `'2'` Mode S short (7), or `'3'` Mode S long (14);
- a 6-byte big-endian 12 MHz timestamp;
- one signal byte;
- the message.

Every `0x1A` inside the timestamp, the signal byte or the message is doubled. This is our own code, written from that
description.

- **Frames sent:** every reply the Tracker believed (`vibe_adsb.h Tracker::accept`), as corrected:
  - DF17/18 with CRC remainder 0, or one repaired bit (sent with that bit fixed: `Tracker::lastBytes`);
  - DF11;
  - DF0/4/5/16/20/21 from an aircraft already heard (address/parity);
  - plus CRC-clean DF18 TIS-B/ADS-R relays. The map leaves those out, but aggregators read them.

  Mode A/C is not demodulated, so no type `'1'` is ever sent.
- **Signal byte:** the amplitude relative to full scale, `255·10^(dBFS/20)` (our level is a power).
- **Timestamp: honest, not MLAT-grade.** Our demodulator places each reply on a 125 ns grid counted in samples from the
  start of the ADS-B run (`Frame::sample`), sent ×1.5 as the 12 MHz counter. It is a sample clock. It jumps when the
  decoder queue drops a block on a slow box, and it restarts with every ADS-B run. **MLAT is NOT supported**, and the
  card and docs say so.

  For the Beast *input*, the far receiver's counter is carried through (÷1.5 then ×1.5, ±1 tick).
- **Never sent:** anything about listeners, or this receiver's position. A site knows where you are only if you enter it
  in your account there.

## The local Beast server

- Off by default. Default port **30005**, owner-settable from 1024 to 65535.
- **LAN only by default.** It accepts loopback, 10/8, 172.16/12, 192.168/16, 169.254/16, fe80::/10, fc00::/7 and the
  box's own IPv6 prefixes (`vibe_ipclass.h`, the same rules as RAW IQ OUT's "Local only"). Any other address is refused
  and counted. "Allow any address" opens it.
- At most 8 readers. A port already in use is reported in words ("port 30005 is in use by another program (or another
  radio)") and retried every 30 s. Give each ADS-B radio on a machine its own port.
- Readers get **every** frame (not reduced). Anything they send is read and ignored.

## Push feeds

Presets (`AF_PRESETS` in the page, `ADSB_FEED_PRESETS` in `src/utils/adsbFeed.ts`).

**Sources.** Read 2026-11-26 by a research agent from each project's own feed scripts or pages. adsb.fi and
airplanes.live refused automated fetches of their websites, so their facts come from their official GitHub feed scripts.

| Site | Host:port | Identity | Source |
|---|---|---|---|
| adsb.lol | feed.adsb.lol:30004 | UUID (we make one when ticked, as their manual set-up does) | github.com/adsblol/feed README; adsb.lol/docs/get-started |
| adsb.fi | feed.adsb.fi:30004 | UUID (their script generates one) | adsbfi/adsb-fi-scripts setup.sh |
| airplanes.live | feed.airplanes.live:30004 | UUID (their script generates one) | airplanes-live/feed setup.sh |
| map.flights | feed.map.flights:30004 | **the station UUID from the owner's map.flights station page** (refused without it) | read from map.flights' own JS bundle (the station generator), not from a written page |
| ADS-B Exchange (added 11-26, Stuart) | feed1.adsbexchange.com:30004 | UUID (we make one when ticked; their client keeps one in `adsbx-uuid`) | github.com/ADSBexchange/feedclient `configure.sh` (last commit 2026-05-13): `TARGET="--net-connector feed1.adsbexchange.com,30004,beast_reduce_out,feed2.adsbexchange.com,64004"`, `REDUCE_INTERVAL="0.5"`, readsb `--uuid-file` |

Rows were left out on purpose:

- **ADS-B Exchange is a preset since 11-26** (Stuart asked; it was first left out because they publish their own
  client). Their client is readsb sending reduced Beast (`beast_reduce_out`, 0.5 s) to feed1.adsbexchange.com:30004 with
  the UUID from `--uuid-file`, which readsb announces the same way as the others (0x1A 0xE4 + the UUID): exactly what our
  push sends. Their failover (feed2.adsbexchange.com:64004) is not used. **Their MLAT (feed.adsbexchange.com:31090) and
  their feeder stats page need their own client**; the card says so, and that client can read our Beast port instead.
- **map.flights is s.r.o. in Prague**, not Dutch.
- **MLAT** for all of these is a separate mlat-client on port 31090, which we do not run.

How the push works:

- **The stream is "reduced"**, as the aggregators' own scripts send it (readsb's `beast_reduce_plus_out`: ordinary Beast
  frames with the same data not repeated within 0.5 s). Per aircraft and message kind, at most one every 0.5 s. The key
  is DF, plus the extended squitter's type code, plus a position's CPR odd/even bit, so a CPR pair is never split. This
  was written from the published description; readsb's code was not copied.
- **Station UUID:** sent once, straight after connecting: `0x1A 0xE4` + the 36 characters, as readsb's receiver-id
  ingest reads it. It is sent only when one is set.
- **Push links connect only while ADS-B runs** on that radio, plus a 2-minute grace, so an aggregator never sees a
  silent feeder from us. They reconnect with 5 s backoff, doubling to 5 min with ±20 % jitter, and reset after a minute
  up.
- **Bounded:** 256 KB queued per link. When a link stalls, the oldest whole chunk goes (never one half-sent) and is
  counted ("N dropped (link too slow)"). There are 64 KB between the decoder and the network thread. Worst case is
  about 4.2 MB.
- **Threads:** the decoder thread only appends under a short lock. All sockets are on the hub's own thread (Network),
  never the audio or DSP threads.
- **The loop guard:** a push destination whose host equals the host a Beast feed radio reads from is never connected.
  It shows as `skipped` ("not sent — this radio's ADS-B comes from there"). Push defaults OFF everywhere. The comparison
  is the host as typed, case-insensitive, so a different name for the same machine is not caught.

## Sharing runs while ADS-B runs

A radio's sharing sends while ADS-B is running on it. To feed all day, use **ADS-B as the landing** with **Keep ADS-B
running when nobody is connected**; the card says this until it is ticked. A Beast feed radio always runs.

## Settings

| Where | |
|---|---|
| `RadioConfig::adsbFeed` | one JSON blob per radio, as `dxScan`: `{"serve","port","open","push","feeds":[{"host","port","on","uuid"}]}`. Empty = off. Checked at the save (`vibe::beast::parseConfig`, refused in words), applied live (`LocalSdrShim::setAdsbFeed`, main.cpp's re-read like dxScan). |
| `RadioConfig::beastIn` + `driver:"beast"` | the Beast feed radio: "host:port" (`parseHostPort`: IPv4, `[IPv6]:port`, a name; no port = 30005). |
| Setup page | radio tab → **Share ADS-B with flight trackers** (`#adsbFeedCard`, drawn only where `adsbLandOffer(...).hw`); top, under the detected radios and before the tabs (Stuart's layout 11-26) → **Add ADS-B feed from another receiver** (`#beastAddBox`: Address host:port, Name, How many users, ADD → POST `/vibeserver/sdr-change {"action":"addbeast","beastIn","label","users"}`); a Beast radio's tab → its own card (`#beastCard`: address via `{"action":"beastaddr"}`, status line with aircraft, REMOVE); every SDR card hidden there (`beastTabApply`) except the flight-tracker card. |
| Admin page | `GET /r/<serial>/vibeserver/admin/adsbfeed` (admin-gated) → `admin.ts renderAdsbFeed` rows. |
| Android | `vibe_android_setup.h` (afServe afPort afOpen afPush → adsbFeed), `vibeServer.ts adsbFeed`, `serverSettingsMirror`, `ServerModeScreen` + `components/AdsbFeedSetup.tsx`, `VibeServerBoot` → `VibeLocalSDR.setAdsbFeed` (JNI), loopback status `GET /vibeserver/adsb-feed`. A Beast feed radio is not offered on the phone (it serves the one radio it holds). |

## The ADS-B Beast feed radio

Decided by Stuart, 11-26:

- It is a radio entry of its own, added by hand (nothing to detect). It has a host:port and a label, nothing else.
- In the radio list and to listeners it behaves like an ADS-B-only radio. It can sit beside real SDRs.

How it is built:

- **Server** (`vibeserver/main.cpp beastRadioFor` → `LocalSdrShim::startBeast`): no hardware is opened or detected.
  The listener and housekeeping run as on an Icom, with no DSP loop. The housekeeping thread sends the 1 fps report.
- **Decoding:** `beastEnter` starts `vibeadsb::Service` with rate 0 (no demodulator). `BeastInput` replies go through
  `Service::injectFrames`, into the **same Tracker**, which applies its own CRC checks and drops and counts failures.
  From there they reach the same report, range outline, shared aviation table (and so the community map), and the
  flight-tracker sink.
- **Listeners:**
  - hwinfo `onlyMode:"adsb"`, always. The existing only-mode clients hide EXIT, and a new listener lands on the map.
  - A listener's EXIT is refused in words, even the admin's, because there is nothing else.
  - Radio caps (`vibe_radio_caps.h`, driver "beast") give `modes:["adsb"]` and every SDR setting off, so there is no
    gain, rate, waterfall, tuning or satellite control (AGENTS.md).
- **The input** (`BeastInput`):
  - TCP keepalive (30 s idle);
  - an 8 s connect deadline;
  - reconnect with backoff, 2 s doubling to 60 s, back to 2 s after a minute up, so a Pi that reboots is picked up again
    by itself;
  - a link silent for 2 minutes is dropped and reconnected (a half-open TCP connection after a power cut);
  - garbage resynced and counted;
  - one 16 KB read at a time, and up to 30 000 replies waiting in the service, oldest dropped and counted.
- **Errors, in the owner's words:**
  - "nothing is listening on 192.168.86.12:30005 — is dump1090's Beast output on?"
  - "no answer from … — is that machine on and on this network?"
  - "… closed the connection (restarting?)"
  - "cannot find piaware.local (…)"

### When the receiver goes away

Stuart, 11-26: his PiAware sometimes drops for hours.

- **Detected both ways.**
  - TCP closed or refused, or no answer at connect.
  - A **stall**: connected, but no bytes for `kStallSec` = **90 s**, after which it reconnects. It is not 30 s because on
    an empty night sky dump1090 may send only its heartbeat, about once a minute. A live feed at ~1,500 msg/s is never
    quiet for even a second.
  - It is *shown* as "stalled" after `kQuietSec` = 20 s without a byte.
- **Reconnects forever.**
  - 2 s doubling to a 60 s cap, ±20 % jitter, for hours or days.
  - A reconnect that delivers data resets the backoff. A peer that accepts and then closes does not reset it.
  - The rules are the pure `vibe::beast::Outage`, which is tested over a 5-hour outage on a fake clock.
- **Logs, with no flood.** One line at the loss ("Beast feed …: lost: … — reconnecting"). At most one every 10 minutes
  while down ("still reconnecting, down 2 h 14 min (N attempts; …)"). One at recovery ("back after 5 h 7 min").
- **Listeners see a red banner over the map, from the server.** Through the existing `feedWarn`, which both clients
  already draw:
  - when lost: "Connection to the receiver lost — reconnecting… (lost 2 h 14 min ago)", plus `feedLost` seconds;
  - when stalled: "Nothing from the receiver for … — its feed has stalled".

  The banner clears itself when data flows. Aircraft age out as always (60 s), so no frozen planes are shown as live.
  It is not on the health pill, because the server is not struggling (feedback_health_pill_genuine_only).
- **The tab and the admin page** show: state (connected / stalled / reconnecting), `downSince`, `attempts` and the last
  error, in the line.
- **Not done:** the directory and community map showing the feed as "receiver offline". Today a down feed shares
  nothing, and the community map's own "nothing on the map right now" covers it. A `receiver offline` flag in the
  community share is the next step.

### Cost

`test-adsb-feed` part 9 measures this. A synthetic dump1090 in a child process sends **1,516 msg/s**. This process runs
the Beast input, the Tracker and report, a local Beast server with a reader, and a push link. It used **0.19 % of one
core** on Stuart's M5 Pro, with peak RSS about 4 MB for the whole test process.

A Pi 3 core is very roughly 8–15× slower, so we expect about 2–3 % of a Pi 3 core. **This has not been measured on a
Pi 3.**

## Running VibeServer beside PiAware

Stuart's case: keep FlightAware fed, and do not take the dongle.

The safe arrangement is that **PiAware keeps the dongle**. VibeServer reads dump1090-fa's Beast output, on the same
machine or anywhere on the LAN. VibeServer never opens that SDR, so PiAware, dump1090-fa, FlightAware MLAT and the
Enterprise status are untouched.

1. On the PiAware Pi, change nothing. dump1090-fa serves Beast on **port 30005**:
   - its package config is `NET_BEAST_OUTPUT_PORTS=30005` (`flightaware/dump1090` `debian/dump1090-fa.default`);
   - its start script passes no bind address, and its listener resolves with `AI_PASSIVE` and no address
     (`anet.c anetTcpServer`), which listens on **every interface**, so the LAN can reach it.

   Verified from FlightAware's dump1090 source on GitHub (2026-11-26), not from a docs page. A firewall on the Pi
   (ufw, say) would still have to allow 30005.
2. Give the Pi a fixed address (Stuart's: 192.168.86.12).
3. On the VibeServer's Setup page (the Lenovo in Stuart's case), in the box **Add ADS-B feed from another receiver**
   under the detected radios, enter Address `192.168.86.12:30005`, a Name and How many users, then press **ADD**.
   VibeServer restarts and the new radio is on air as a tab of its own.
4. Its tab shows "Now: Beast feed from 192.168.86.12:30005: connected, N messages a second". If it shows "nothing is
   listening on …", check that dump1090-fa is running on the Pi and that nothing blocks 30005.
5. Leave **Send to flight-tracking sites** off for this radio unless you mean it: the Pi already feeds FlightAware. The
   loop guard stops a feed back to the Pi itself, not a feed to another site.

If VibeServer runs **on the Pi itself**, use `127.0.0.1:30005`. Do **not** turn on "Serve a Beast feed" on port 30005
on that radio: dump1090-fa already holds that port.

## Feed FlightAware from a VibeServer

For an owner whose dongle VibeServer itself runs. Steps are from FlightAware's own pages, read 2026-11-26:

- https://www.flightaware.com/adsb/piaware/install
- https://www.flightaware.com/adsb/piaware/advanced_configuration
- github.com/flightaware/piaware

1. On VibeServer's Setup page, on the radio's tab, open **Share ADS-B with flight trackers** and tick **Serve a Beast
   feed** (port 30005). Make ADS-B that radio's landing with **Keep ADS-B running when nobody is connected**, or there is
   nothing to read while nobody listens.
2. Install PiAware from FlightAware's instructions. Officially that is Raspberry Pi OS (their apt repository package,
   then `sudo apt install piaware`). On other Debian systems only a source build (piaware_builder) is described. It can
   run on the VibeServer machine or another one on the LAN. **dump1090-fa is not needed**: "If you don't already have
   ADS-B decoder software such as dump1090-fa installed…".
3. Point PiAware at VibeServer. `other` is the receiver type that works on a package install: PiAware connects to
   `receiver-host:receiver-port` itself. `relay` is for SD-card images only and reads localhost.

   ```
   sudo piaware-config receiver-type other
   sudo piaware-config receiver-host <the VibeServer's address, e.g. 127.0.0.1 or 192.168.1.20>
   sudo piaware-config receiver-port 30005
   sudo systemctl restart piaware
   ```

4. After about 4–5 minutes, read the feeder id: `cat /var/cache/piaware/feeder_id`. Claim it at
   `https://www.flightaware.com/adsb/piaware/claim/<feeder-id>`, then set the location on your FlightAware stats page.
5. **Enterprise.** FlightAware says feeders "automatically qualify for a free upgrade to an Enterprise Account" when they
   "first start feeding data". No uptime threshold is published; forum posts guess 3 days to a month offline before it
   lapses. So keep ADS-B running.
6. **MLAT: leave it off** (`sudo piaware-config allow-mlat no`). mlat-client wants Beast with a 12 MHz clock
   consistent with the receiver's real sample clock. Ours is not verified for that (see above), and bad timestamps
   would only feed FlightAware's MLAT bad data.

**What is verified:** the piaware-config keys and the claim steps, from FlightAware's pages and its piaware source.
Our Beast stream against our own reader and a test client.

**Not verified:** PiAware actually reading our port. A person has to run it once (see Live checks).

## Listener bandwidth on the ADS-B map (for "How many users")

**Before beta96.** Stuart measured the Sony (beta95): 60 s as one listener on its ADS-B map with 45–66 aircraft used
**12.7 KB/s** in total.

- 11.75 KB/s of that was the `adsb` report, which re-sends the full aircraft list every second;
- 0.38 KB/s was `adsb_info`.

That is about **0.2 KB/s per aircraft per listener**.

**Beta96: the DELTA protocol** (docs/v12/ADSB-DELTA.md, built on branch `b96-adsbdelta`). The app and the web page ask
for `adsbd=1` and get the whole table once, then only what changed each second, with a keyframe every minute. This was
measured through the real server on a synthetic Beast sky of about 150 aircraft, with both clients side by side for
60 s:

- **Older client: 32.13 KB/s.**
- **Delta client: 2.42 KB/s.** That is about **0.015 KB/s per aircraft per listener**.
- **100 listeners at 150 aircraft:** about 1.9 Mbit/s of upload, instead of about 26 Mbit/s.

Older apps and pages (and the watch, Jr and the R36S, which do not ask) still get the full list, at the old cost.

The Setup page's "How many users" hint quotes the new figure: about 2.5 KB/s per listener with 150 aircraft, and about
1 KB/s with 60.

## Tests

| Test | Result | Covers |
|---|---|---|
| `vibeserver/test-adsb-feed.cpp` | **107/107** | Hand-worked Beast bytes with escapes; 4,000 random frames round-tripped in odd TCP-sized pieces with junk and a broken frame; signal byte; 48-bit counter; settings and every refusal; LAN rule; the local server with a test reader (all 300 frames in order; a taken port named; switched off closes readers); push to a 127.0.0.1 endpoint (UUID first, reduce, reconnect after a drop, backoff with nothing listening, the bounded queue dropping and counting on a stalled reader, ADS-B stopping takes the link down after its grace); the service end to end on modulated IQ (a 1-bit error sent corrected, counter spacing within 0.25 µs); host:port parsing; the Beast input against a synthetic dump1090 (owner's words when nothing listens, aircraft + positions + callsign through the Tracker, junk and bad CRC counted, reconnect when the feeder returns, a silent link dropped); the loop guard; the cost at ~1,500 msg/s. |
| `vibeserver/test-config-adsbfeed.cpp` | 11/11 | Config round trip; off by default; an older file; an oversized blob dropped; Beast radio caps (adsb only, no SDR setting) and its entry round trip. |
| `scripts/test-server-beast-feed.mjs` | **28/28** (and 23/23 ×3 before the outage checks were added) | The REAL binary: (A) a server whose only radio is a Beast feed (starts with no SDR, aircraft + position + callsign, onlyMode adsb, EXIT refused, tune ignored, admin status connected and counting noise, the feeder going away said); (B) a Full server with a fake RTL beside a Beast radio (both up, status through the door, the RTL ordinary); (C) add (a bad address refused) and remove through POST `/vibeserver/sdr-change`. |
| `scripts/test-setup-adsb-feed.mjs` | 50/50 | The page's real `afRowsFrom`, `afBlob`, `afRender`, `beastAddrWhy` and `beastTabApply` against stand-ins, plus the shared fixture. |
| `scripts/test_adsb_feed.ts` | 17/17 | The app's copy on the same fixture; admin rows (input, skipped). |
| Checks | | `test-android-setup` all passed; `check-android-settings-parity` 206/0; `check-remote-keys` 486; `check-setup-page` green; `tsc` clean for these files. |

## Live checks (a person, not an agent)

1. **PiAware reading our port** on a VibeServer with an ADS-B radio: the steps above. Check that
   `piaware-status` / the FlightAware stats page shows messages and aircraft, with MLAT off.
2. **The Lenovo reading Stuart's Pi** (192.168.86.12:30005). On the Lenovo's Setup page: Add an ADS-B Beast feed. Then
   check:
   - the tab's line shows connected and messages a second;
   - the map in the web client and the app: **no waterfall, nothing reconnecting**;
   - the community map shows the aircraft;
   - the Pi's FlightAware feed is unchanged.
3. **One aggregator push** with a real UUID (adsb.lol is the simplest; its feeder page shows the UUID's status) from a
   radio with ADS-B kept running. Check their map within a few minutes.
4. **Pi 3 CPU:** a VibeServer on the Pi 3 with only a Beast feed radio at ~1,450 msg/s. Watch `top`; we expect a few % of
   a core.

## Open questions for Stuart

1. ~~Offer ADS-B Exchange as a preset?~~ Done 11-26 (Stuart): a preset, with MLAT and stats left to their client.
2. Put the push feeds on the phone's server screen too? They are there now. Is that wanted for phones on mobile data?
3. MLAT: worth a future job (our own mlat-client-compatible timing)? Not promised anywhere.
