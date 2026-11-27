# The ADS-B range outline from feeds, and the map's receiver position (b96-rangefeed, 2026-11-27)

Stuart, 11-27, on the Lenovo's AIR & SEA viewer:
- "range ring still too slow to update". Fresh ADS-B aircraft sat outside the ADS-B RANGE outline minutes after the map
  opened (VKG520 over the Humber, AFR75HB, FRA91, EJU41JX near the coast, KLM1523).
- "actually i cant activate the range rings either". RINGS, FIT and ⌖ were greyed and RANGE was missing.

The Lenovo's radio (RSP1A) has no ADS-B of its own. Its aircraft come from a server-level Beast feed (PiAware at
192.168.86.12:30005, `vibe_feeds.h`, owned by the front door). Its config has locator IO92nh, and its lat/lon are empty.

## RESUME HERE

Branch `b96-rangefeed` (from v12-experimental 567de0bd4). There are four commits, nothing is pushed, and no server was
touched. The only access to the Lenovo was read-only GETs.

- **Done:** the server counts feeds into the stored outline. The page's live outline puts every counted position on or
  inside the line. ADS-B is never sparse. The map's receiver falls back to lat/lon, then the locator centre, then the
  range centre. On a front-door page, the viewer reads the primary radio's `/location` and `/adsb/` instead.
  `vibe_web_page.h` is rebuilt.
- **Not done / open:**
  - The setup page (`admin.ts renderAdsbRange`) and the app's server screen draw their own outline preview. They still
    put a vertex on each bin's middle bearing. That is cosmetic in a small preview.
  - A feed at a **different site** would stretch this server's outline toward it. Every feed is assumed to be at this
    site. A per-feed "at this site" flag in the feed's settings would fix that (open decision for Stuart).
  - `test-web-avmap.mjs` was not run (it needs a VDL2 clip, and this Mac has none).
  - Nobody has looked at the Lenovo after a deploy yet. The coordinator should open the AIR & SEA viewer from the door
    and check: RANGE appears, RINGS/FIT are enabled with an "approximate — locator square IO92NH" title, and `GET
    /r/<id>/adsb/range` fills within a minute.

## What was wrong (confirmed, with evidence)

1. **The stored outline ignored feeds.** On the Lenovo, `GET /adsb/range` returned window 30 and centre 52.3125/-0.875,
   with every bin 0 while 127 feed aircraft were live. `vibe_adsb_range.h`'s store was fed only by
   `vibe_adsb_service.h`'s own Tracker, in the radio process. The feed's Tracker runs in the front door, a different
   process. So every page started from an empty outline and drew only what that page had seen since it opened.
2. **The sparse lead was only partly right.** I replayed 25 live polls of the Lenovo's `aircraft.json?d=` through
   `AircraftDeltaStore` → `sharedSanitise` → `avNowRule` → `avPlaneItems` and the layer's own maths. Feed rows arrive
   with `posSrc: "ADS-B"` and no `lastSeen`, so they are **not** sparse, and the live outline did grow (254 bearings in
   75 s). Only "last seen" ADS-B rows were sparse (position more than 60 s old, aircraft still answering). The trouble
   was that such a fix became the first of the two-fix pair at a time clamped to 60 s. The next fresh fix then read as
   a bad jump (AAL104, DLH953 and TARTN59 in the replay), so the edge-of-range aircraft waited one more fix.
3. **The furthest aircraft were drawn outside the line.** `rangeRing` put one vertex on each 1° bin's middle bearing. A
   bin's edges were then cut short by the lower neighbour. In the replay, 15 counted aircraft at any moment (EJU41JX among
   them) sat 0.5–4.4 km outside the outline, which is a whole icon at a regional zoom.
4. **No receiver, no range on the door.** The standalone viewer opens before any radio is started, so
   `loadServerLocation` never ran and the page had no `rx`. RINGS and FIT were disabled. On the front-door page (path
   `/`), `/location` and `/adsb/range` are answered by the door's own HTML page. The door serves `aircraft.json`, but not
   those two. So the layer had no outline and no centre, and RANGE never appeared. The radio's own `/location` already
   gives the locator's centre.

## What changed

### Server: `android/app/src/main/cpp/vibe_adsb_range_feed.h`, started in `LocalSdrShim::setAdsbRangeFiles`

**One owner each.** The door keeps the feeds. The radio keeps its outline and answers `/adsb/range`. The two already meet
in the shared aircraft table (`<runtime dir>/aircraft-live.dat`), which holds every writer's positions with their fix
times and the radios that heard each one.

**How it runs.** The process that answers `/adsb/range` reads that table every 2 s on a decoder-priority thread. That
process is a radio behind a door, a simple server, or the Android server. It reads only while the table header's
ADS-B-live stamp is fresh, and it never creates the file just to read it.

**What counts:**
- Positions whose source is **ADS-B** only. ACARS, VDL2, HFDL and ADS-C positions never count.
- Positions no more than **10 s** old.
- An aircraft's **second consistent fix**: the step must be ≤ 1 200 kt, assuming at least 1 s. This is the same posOk
  rule as the Tracker and the page.

**What is left out:** a row heard **only** by this radio. Its own service already offered those.

**What else counts:** a Beast or BaseStation ADS-B feed, and the other radios on this server. The door's loopback
ingest tags their rows "feed" too.

**Centre:** the server's location (`g_bgLat/Lon`). When only a locator is set, that is the locator's centre.

**Bounds:**
- At most 2 048 aircraft are tracked.
- An aircraft unheard for 120 s is forgotten.
- Each read is one 264 KB pread of a tmpfs file, every 2 s, and only while ADS-B is live. Each radio's writer already
  reads the whole table every 2 s.

**Decision taken:** feeds count **always**, not only when the radio has no ADS-B of its own. The outline is a maximum,
so a repeat is harmless, and the live outline on the page counts the same aircraft. ⚠ A feed is assumed to be at this
site (see the open item above).

### Page: `web/mapkit/adsblayer.js`, the one ADS-B layer for the web and the app

- **`rangeRing`:** vertices now sit on the boundaries between bins, at the further of the two. Both ends of a bin's edge
  are therefore at least its range. Every counted position is on or inside the line: 0 of 249 outside, against 24
  before. There is still one vertex per bearing, with no staircase.
- **`liveRangeFix`:** uses the position's own time (`now − seen_pos`, not clamped), and a sparse fix clears the pair.
- **Receiver fallback:** with no host position, the range outline's centre becomes the receiver once `GET range`
  answers. The RINGS and FIT titles then say "approximate — …".
- **`setReceiver(lat, lon, approx)`** passes the host's words for a position that is not exact.
- **Greyed controls say why:** "Not available: this server has not given its receiver position (no latitude/longitude
  or locator, and no ADS-B range outline yet)".
- **`receiverNow()`** is added for tests.
- **Re-sync:** the stored outline is still re-synced every **15 min** (`RANGE_POLL_MS`, not 5 min as an old comment
  said). It is merged with the live outline by max per bearing (`rangeGeo` → `rangeMerge`) and never replaces it.
- **Redraw:** the live outline redraws at most every 1.5 s after it grows (`rangeSoon`).

### Clients

- **`src/services/aviationMap.ts`:** `sparse = posSrc !== 'ADS-B'`. A "last seen" ADS-B fix is no longer sparse. It
  stays stale-toned through its `seen_pos`, and it has no track or speed, so it is never flown on.
- **`src/services/rxPosition.ts`** (new, pure):
  - `rxFromLocation`: lat/lon, else the locator square's centre, marked approximate. It is also marked approximate when
    the server put the square's centre into lat/lon.
  - `rxApproxWords`: the words a map adds for a position that is not exact.
  - `doorRadioId`: the door's primary radio, else its first.
- **`web/client/src/main.ts`:**
  - `loadServerLocation` uses `rxFromLocation`.
  - The standalone viewer asks for `/location` first. On a door it asks `/r/<primary>/location`.
  - The AIR & SEA map's `imageBase` on a door is `/r/<primary>/adsb/`.
  - `myPos()` carries the approx words.
- **`tracks.ts`:** passes the approx words to the layer.
- **The app:** `SDRScreen` already falls back from lat/lon to the grid (`serverPosFromLocation`). The app's maps get the
  layer's range-centre fallback through the generated source.
- **Scope:** these changes touch only where rx comes from and the enable rules. The other agent, b96-mapcards, is moving
  the controls into a MAP card.

## Tests (all silent)

| Suite | Result | Fails before |
|---|---|---|
| `vibeserver/test-adsb-range-feed.cpp` (new): the rule on rows; synthetic dump1090 → FeedSet → table → follower → bins | 27/27 | there was no path, so the bin stayed empty |
| `scripts/test-server-aircraft-feeds.mjs`: real door + RTL radio, the Lenovo's config shape; the radio's `/adsb/range` fills at 40621D's bearing (89°, 327 km) | 21/0 | the 567de0bd4 binary fails both new checks: no bins, no centre |
| `scripts/test_adsb_layer.mjs`: every counted position inside | 242/0 | old ring: 24 of 249 outside |
| `scripts/test-web-adsb-range.mjs` (headless Edge): sparse never pairs; stale → fresh counts at once; boundary vertices; no position → greyed with the reason; the range centre as receiver; the host position wins | 47/0 | 9 failed on the old layer |
| `scripts/test_aviation_map.ts`: feed / other-radio / "last seen" ADS-B not sparse; VDL2 and ADS-C sparse | 98/0 | the "last seen" case failed |
| `scripts/test_rx_position.ts` (new) | 17/0 | — |
| Related suites still passing: `test-adsb-range` 55/0, `test-aircraft-delta` 39/39, `test-server-aircraft-delta` 20/0, `test_live_maps` 65/0, `test_aircraft_feeds_app` 15/0, `test-web-adsb-list-width` 122/0 | | |

## What Stuart will see (after a deploy)

- **Outline from the start:** the outline is there as soon as the map opens, and it grows in the background from the
  PiAware feed. It is the server's 30-day outline, not just what this page has seen.
- **Edge aircraft:** the furthest aircraft sit on or inside the line, and an edge aircraft counts on its first fresh fix
  after a gap.
- **From the door:** the AIR & SEA viewer shows RANGE, and RINGS and FIT work. Their tooltip says the receiver position
  is approximate: the centre of locator square IO92NH.
