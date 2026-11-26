# Aircraft feeds, flight trackers and the standalone AIR & SEA map (V12 beta96)

Stuart, 2026-11-26, in steps:

1. Feed ADS-B to flight trackers, built "like we did for PSKReporter" (branch `b96-adsbfeed`).
2. The ADS-B Beast feed radio: another receiver's Beast output as a radio entry (`b96-adsbfeed`).
3. **"Feeds are NOT radios"** (branch `b96-sbsfeed`): aircraft feeds become SERVER-LEVEL SOURCES — Beast or BaseStation
   (SBS) text, connecting out or listening, labelled ADS-B / HFDL / VDL2 / ACARS — feeding the machine's shared aircraft
   table directly. His case: his UberSDR's dumphfdl sends HFDL aircraft as BaseStation text, and his PiAware Pi 3 serves
   Beast; both should show on the Lenovo's AIR & SEA map and the community map.
4. **VibeServer as a middle man**: the flight-tracker destinations are server-level too, fed by every radio's own ADS-B
   AND every ADS-B feed source, de-duplicated, with loop guards.
5. **A landing-page AIR & SEA MAP button**: the combined map with no radio taken.

## RESUME HERE

- **Built, tested, committed on `b96-sbsfeed`:**
  - `vibe_sbs.h`: the BaseStation parser (MSG 1–8; junk, partial lines, over-long lines, binary bytes), a per-source
    table tagged by label, positions absolute, the fix's own time, impossible jumps refused.
  - `vibe_feeds.h`: server-level sources (Beast | SBS × connect | listen × ADS-B | HFDL | VDL2 | ACARS), the LAN-only
    listener (refused peers logged once per address, then once per 10 min), down/stall rules, the banner text, the status
    file other processes read, the shared-table provider ("feeds"), the loopback ingest for the radios' own ADS-B.
  - The hub (`vibe_beast.h Hub`) as a middle man: foreign frames byte-faithful (`frameRaw`), de-duplication across
    inputs, "only my own radios" per destination and for the Beast output, loop guards from every source host.
  - Config: `ServerConfig::aircraftFeeds` + `ServerConfig::adsbShare`; `migrateAircraftFeeds` on every read (a feed
    radio → a source, its entry dropped; per-radio `adsbFeed` → the server-level hub, duplicates once).
  - Server wiring (`main.cpp applyAircraftFeeds`): the front door / a Simple server owns sources and hub; radios behind
    the door forward their own ADS-B to the door's loopback ingest; `aircraft.json` (served by the door now too) carries
    `feeds` (labels, never addresses) and `feedWarn`; `admin/adsbfeed` at the door lists the sources; `addbeast` adds a
    source; a left-over `beast-N` radio process exits 0 saying why; `--beast-in host:port` keeps the old ADS-B-only process
    as a diagnostic (test-server-adsb-delta uses it).
  - Setup page SERVER tab: "Aircraft feeds from other receivers" and "Share ADS-B with flight trackers" (moved there).
    Admin page: one server-level row. The AIR & SEA map's banner (web + app) from `aircraft.json feedWarn`.
  - The standalone AIR & SEA map on the web landing page: **`openAirSeaViewer()`** (also `window.openAirSeaViewer`).
- **Not done (honest):**
  - **The app**: no AIR & SEA button on the app's server landing and no navigation target yet (the app's map lives inside
    SDRScreen, bound to a connected radio). The web entry point is `openAirSeaViewer()`; the app needs a screen that hosts
    `MapOverlay kind="airsea"` fed by `aircraft.json` polls with no `VibeServerClient`.
  - **The map's own limits** ("Map viewers", "Map time limit", the admin count, the visitor stats) — not built. The viewer
    is plain HTTP polling today, so there is no viewer session to count yet (see Open questions).
  - **Ships in the button's gate**: the landing button is shown when the server has aircraft or an aircraft feed; a
    ships-only server does not get it yet.
  - **The landing on the HFDL layer**: the standalone map opens on AIRCRAFT with every layer; no per-source landing.
  - **Android**: the phone server keeps its own per-radio flight-tracker card (`AdsbFeedSetup`); server-level SOURCES are
    not wired on the phone (`LocalSdrShim::setAircraftFeeds` exists; Kotlin/JNI/RN settings do not call it). The new
    headers compile with the NDK (aarch64, armv7); Gradle was not run.
  - **Feed aircraft use aircraft.json, not the delta stream**: a feed's aircraft reach a radio's AIR & SEA map through the
    3-second `aircraft.json` poll (the full list), not the ADS-B delta protocol, which carries only a radio's own decode.
  - **"Other" as a label** is not offered: the shared table, the clients and the community feed know five sources
    (ADS-B, ACARS, VDL2, HFDL, ADS-C); an "Other" aircraft would be drawn by no map. VDL2 and ACARS were offered instead
    (dumpvdl2 and acarsdec-family tools also write BaseStation). Open question below.
- **Never connected to a real receiver or aggregator** (by the rules). Everything ran on 127.0.0.1.

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

A radio's own replies reach the hub while ADS-B runs on it. To feed all day from a radio, make ADS-B its landing with
**Keep ADS-B running when nobody is connected**. Beast feed sources always run. Frames arriving keep the push links up
(with the 2-minute grace); a server with no ADS-B running anywhere and no feed frames sends nothing.

## Settings

| Where | |
|---|---|
| `ServerConfig::aircraftFeeds` | the server-level sources (JSON list, `vibe::feeds::parseSources`; checked at the save, refused in words; applied live by `applyAircraftFeeds` → `LocalSdrShim::setAircraftFeeds`). |
| `ServerConfig::adsbShare` | the server-level hub: `{"serve","port","open","serveOwn","push","feeds":[{"host","port","on","uuid","own"}]}` (`vibe::beast::parseConfig`), applied by the owning process with `LocalSdrShim::setAdsbFeed`. |
| `RadioConfig::adsbFeed` / `beastIn` + `driver:"beast"` | **migrated away on read** (`migrateAircraftFeeds`); still read so an older file loads. |
| Setup page | SERVER tab, after the community map: **Aircraft feeds from other receivers** (`#feedsCard`: list + status + REMOVE, `+ Add aircraft feed` — format, direction, address or port, label for SBS only, name) and **Share ADS-B with flight trackers** (`#adsbFeedCard`, drawn when the server has ADS-B: a radio that could run it or an ADS-B feed). Saved with Save (`aircraftFeeds`, `adsbShare` at the top of the config). |
| Admin page | `GET /vibeserver/admin/adsbfeed` at the door (admin-gated) → the hub status + `sources` → `admin.ts renderAdsbFeed` (one row). |
| `aircraft.json` | `feeds` [{title, label, state, aircraft, perSec, msgs, down}] (no addresses) and `feedWarn`. |
| Android | unchanged: the phone's one radio keeps its own flight-tracker card (`vibe_android_setup.h` afServe… → adsbFeed → `setAdsbFeed`, `AdsbFeedSetup`). Server-level sources are not wired on the phone yet. |

## Feed sources (server-level)

`android/app/src/main/cpp/vibe_feeds.h`. Configured as `ServerConfig::aircraftFeeds`, a JSON list (at most 8):

```json
[{"fmt":"beast","dir":"connect","host":"192.168.86.12","port":30005,"label":"adsb","name":"PiAware"},
 {"fmt":"sbs","dir":"listen","port":32010,"label":"hfdl","name":"UberSDR HFDL"}]
```

- **Where it runs:** ONE process per machine — the front door (Full layout), the only process of a Simple server, or the
  Android server. Radio processes behind the door run none of it: they read the shared table and the status file
  (`<runtime dir>/aircraft-feeds.json`, written every 2 s, stale after 15 s).
- **Beast** is Mode S, so always ADS-B. Its replies go through our own `vibeadsb::Tracker` (CRC, CPR, plausibility), its
  aircraft into the shared table with the ADS-B bit, and — when believed — to the hub byte-faithfully.
- **BaseStation (SBS)** is text with absolute positions (`vibe_sbs.h`). The label decides the shared table's bit: an HFDL
  source's aircraft carry the HFDL bit and an HFDL position, never ADS-B; the community feed then keeps their positions
  30 minutes (its datalink window) and fades them by age. SBS is never forwarded to flight trackers.
- **connect:** `BeastInput` (the same client as before: keepalive, 8 s connect deadline, 2 s → 60 s backoff with jitter,
  forever). For SBS sources labelled HFDL / VDL2 / ACARS a quiet link is never dropped (they report minutes apart).
- **listen:** a TCP listener on the port, **LAN only** (`beast::peerAllowed`, the RAW IQ OUT rule), at most 4 clients,
  keepalive on accepted sockets; a refused address is logged once, then at most once per 10 minutes, and counted.
- **Down:** a connect source whose link has been lost (at once), a listen source with nobody connected for 90 s, or an
  ADS-B source with no data for 90 s. The banner on the AIR & SEA map lists every down source and the live ones:
  `PiAware lost — reconnecting… (2 min) · UberSDR HFDL still live`.
- **Status** per source: state (opening / listening / connected / stalled / reconnecting / down / error), messages a
  second, aircraft, connected-from address, refused count, last error. The owner's view (Setup, admin) has addresses;
  `aircraft.json` has labels and states only.
- **Live**: a save re-applies at once; a source whose settings did not change keeps its connection and aircraft.

## The former ADS-B Beast feed radio

A radio entry with `driver: "beast"` (from `b96-adsbfeed`) is migrated on load into a source (Beast, connect, its
`beastIn`, ADS-B, named after the radio) and the entry is dropped. A process started for it (`vibeserver@beast-1`) exits 0
with a line saying where the feed went. The ADS-B-only process itself survives as `--beast-in host:port` (diagnostic and
tests). The outage rules below are the Beast client's, unchanged, and apply to every connect source.

### When a receiver goes away

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

## BaseStation / HFDL from UberSDR (dumphfdl)

What dumphfdl sends (read 2026-11-26 from its README and its basestation formatter on GitHub):

- `--output decoded:basestation:tcp:address=<host>,port=<port>` — the **tcp output CONNECTS OUT** to that address and
  retries every 10 s if it drops. So VibeServer **listens**.
- Only `MSG,3` lines: the ICAO address (6 upper-case hex digits), the callsign when known (HFDL's are 6 characters),
  latitude and longitude (6 decimals), altitude / speed / track **empty**, squawk `0` (or the HF channel in kHz with
  `--freq-as-squawk`, which VibeServer shows as "HFDL 8977"), on-ground `0`, and the position's own time in the
  "generated" fields (VibeServer keeps it as the fix time when it is believable).
- A line only for an aircraft whose ICAO address it knows (from a logon) and only from positions under 5 minutes old,
  taken from performance data, frequency data and ADS-C basic reports.
- Nothing in a line says HFDL — so the source's label does. dumphfdl asks that this feed is kept local and not sent to
  public ADS-B networks: VibeServer never forwards SBS to flight trackers.

**Stuart's set-up** (the Lenovo VibeServer is 192.168.86.13):

1. On the Lenovo's Setup page, SERVER tab, **Aircraft feeds from other receivers → + Add aircraft feed**: Format
   *BaseStation / SBS text*, Direction *It connects to this server (listen on a port)*, Port **32010**, What it is
   **HFDL**, Name *UberSDR HFDL*. ADD, then **Save**.
2. On the UberSDR, the dumphfdl service in its docker-compose file:

   ```yaml
       environment:
         - EXTRA_ARGS=--output decoded:basestation:tcp:address=192.168.86.13,port=32010
   ```

   (in the list form, no quotes — they would become part of the value; add `--freq-as-squawk` to the same line to see the
   HF channel on each aircraft). If the service already has `EXTRA_ARGS`, add this to the end of it. Then
   `docker compose up -d` for that service.
3. The feed's line on the Setup page becomes "connected from 192.168.86.x …"; HFDL aircraft with positions appear on the
   AIR & SEA map's HFDL layer and, with the community map on, on the community map labelled HFDL.

The Lenovo's firewall (if any) must allow TCP 32010 from the LAN.

## VibeServer as a hub

Stuart's Lenovo, all at once:

```
 PiAware Pi 3 (192.168.86.12) ── Beast :30005 ──►┐
 UberSDR dumphfdl ── BaseStation → :32010 ───────►├─ Lenovo VibeServer ── shared aircraft table ──► AIR & SEA map, community map
 the Lenovo's own ADS-B radio (if any) ──────────►┘          │
                                                             └─ the hub (ADS-B only) ──► adsb.lol, map.flights (+ Beast out :30105)
```

- **Inputs to the hub:** every radio's own believed replies (behind the door: over the loopback ingest the door opens and
  writes in `adsb-ingest.port`; the radio's hub pushes there, unreduced) and every **Beast** source's believed replies,
  passed on **byte-faithful** — the upstream timestamp and signal byte exactly as received.
- **Never into the hub:** SBS sources (HFDL, VDL2, ACARS, or SBS labelled ADS-B — their positions are not Mode S frames).
- **De-duplication:** the same 7 or 14 bytes from a DIFFERENT input within 2 s is sent once (`Hub::put`, one steady
  clock). Repeats from the same input always go (DF11 replies and identity squitters repeat byte for byte).
- **Loop guards:** a destination on any source's host is never connected (shown "skipped": "ADS-B comes into this
  server from there"); a local Beast reader connecting from a source's address gets only our own radios.
- **Own only:** each destination, and the local Beast output, can be set to "Only this server's own radios" (default:
  everything).
- **Migration:** every radio's old flight-tracker settings merge into the server-level list (the local server on if any
  radio served, its port; push on if any pushed; each host:port once, any case, the first UUID wins).
- **Do not** serve Beast on 30005 on the Lenovo if a dump1090 runs there; for Stuart's case the Pi keeps its own FlightAware
  feed, and the Lenovo should not push to FlightAware for it.

## The standalone AIR & SEA map

**One entry point: `openAirSeaViewer()`** in `web/client/src/main.ts` (also on `window`), for the landing page's AIR & SEA
MAP button (`#splashAirSea`) and the menu entry beside GALLERY (b96-mapchat). It opens the same AIR & SEA map (`asOpen`)
with no radio: no WebSocket, no tuner — only this server's HTTP reads, which the map already makes (`aircraft.json`
every 3 s, the ship store, the gallery's remembered sondes and VOR), so it is **live** and keeps working when every radio
is busy or there are none. It controls nothing (CLOSE MAP goes back to the landing). The door serves `aircraft.json`
itself. The button is drawn only when the server has aircraft or an aircraft feed (one read when the landing draws its
radios, never polled). Not yet: the app, ships in the gate, and the viewer limits (Open questions).

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
3. On the VibeServer's Setup page (the Lenovo in Stuart's case), SERVER tab, **Aircraft feeds from other receivers → + Add
   aircraft feed**: Format *Beast*, *This server connects to it*, Address `192.168.86.12:30005`, Name *PiAware*. ADD, then
   **Save**. No restart; no radio entry.
4. Its line shows "Now: Beast feed from 192.168.86.12:30005: connected, N messages a second · N aircraft". If it shows
   "nothing is listening on …", check that dump1090-fa is running on the Pi and that nothing blocks 30005.
5. **Send to flight-tracking sites** (server-level) now includes the Pi's aircraft unless a site is set to "Only this
   server's own radios". The loop guard stops a feed back to the Pi itself, not a feed to another site.

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
| `vibeserver/test-aircraft-feeds.cpp` | **80/80** (×6 runs, ASan clean) | SBS parsing (dumphfdl's own MSG,3 line, dump1090 MSG 1/3/4/6, flags, CRLF, junk: binary, 5 000-byte line, split across reads, `~` and 000000 addresses, (0,0), nan/inf/hex numbers, other record kinds, 20 000 random lines); the table (HFDL never ADS-B, freq-as-squawk, the fix's own time, an impossible jump refused, bounded, expiry); the list (defaults, every refusal, Beast forced to ADS-B); a LISTEN source (stream, split line, status, shared-table row HFDL-only, never "ADS-B now", reconnect clears the banner, a refused address counted and logged once); two sources (Beast connect + SBS listen) merged into ONE row with both bits, banner "PiAware lost … · HFDL source still live", the status file and staleness; the hub (two Beast sources + our radio → one destination gets each reply once, byte-identical with the upstream timestamp/signal, HFDL never, the loop guard, own-only). |
| `vibeserver/test-adsb-feed.cpp` | **107/107** | The Beast encoder/reader/server/push as before (the de-dup only across inputs keeps its repeated-frame checks). |
| `vibeserver/test-config-adsbfeed.cpp` | **15/15** | Migration: a radio's blob → `adsbShare` and cleared; idempotent; two radios merged (duplicates once, any case); a Beast feed radio → a source, entry dropped, not twice; sources round-trip. |
| `scripts/test-server-aircraft-feeds.mjs` | **18/18** | The REAL binary, Full layout, a fake RTL radio: the feed radio migrated; a left-over `beast-1` process exits 0; the door's admin lists both sources; dumphfdl lines (connecting in) + a synthetic dump1090 → one row with ADS-B+HFDL, an HFDL-only row with "HFDL 8977"; `aircraft.json` feeds without addresses, at the door and through the radio; the hub forwards the Beast replies byte-faithfully and nothing else, skips the source's host; the banner when the Pi goes; `addbeast` adds a source with no restart; the saved config. |
| `scripts/test-web-airsea-standalone.mjs` | **11/11** | Headless Edge (`--mute-audio`): the landing's AIR & SEA MAP with the only radio not running; the map with the feed's aircraft and **no WebSocket**; a new aircraft appears live; CLOSE MAP back to the landing; no button with nothing to show. |
| `scripts/test-server-adsb-delta.ts` | **11/11** | Now on `--beast-in`: older 32.12 KB/s vs delta 2.41 KB/s at 155 aircraft. |
| `scripts/test-setup-adsb-feed.mjs` | **68/68** | The server-tab cards: placement, the old boxes gone, own-only switches and blob, the gate (a radio that could run ADS-B or an ADS-B feed; HFDL-only not), afRender, the feeds form (defaults, Stuart's two sources, refusals, round trip, the label only for SBS, dumphfdl's option). |
| `scripts/test_adsb_feed.ts` | **19/19** | The app's copy on the shared fixture; the admin rows (a source line). |
| Checks | | `check-setup-page` green. Not run here: `scripts/run-tests.sh` (the coordinator's). |

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

5. **The Lenovo with both feeds**: PiAware (Beast connect 192.168.86.12:30005) and the UberSDR's dumphfdl (the
   docker-compose line above, SBS listen 32010, HFDL). Check: both lines connected on the Setup page; HFDL aircraft on the
   AIR & SEA map's HFDL layer and on the community map; one aircraft heard both ways is one row; stop dumphfdl's
   container → after 90 s the banner names it and says PiAware is still live; start it → the banner clears.
6. **The landing page's AIR & SEA MAP** on the Lenovo from a phone browser while every radio is busy.

## Open questions for Stuart

4. "Other" as a feed label: offer it (needs a sixth source bit in the shared table, a file version bump, and a badge in
   every map) or keep ADS-B / HFDL / VDL2 / ACARS?
5. The standalone map's limits ("Map viewers", "Map time limit"): count viewers by a lightweight session (a ticket from a
   `/vibeserver/airsea/join` the polls carry), or by distinct addresses polling `aircraft.json` in the last 10 s? The
   second needs no client change but miscounts shared NAT.
6. Feed aircraft on a radio's live ADS-B layer (the delta stream) instead of the 3-second aircraft.json: worth it for a
   server that is mainly feeds?

1. ~~Offer ADS-B Exchange as a preset?~~ Done 11-26 (Stuart): a preset, with MLAT and stats left to their client.
2. Put the push feeds on the phone's server screen too? They are there now. Is that wanted for phones on mobile data?
3. MLAT: worth a future job (our own mlat-client-compatible timing)? Not promised anywhere.
