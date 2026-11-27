# Aircraft feeds: the front door fix, FIND, test before saving, and the UberSDR HFDL feed (beta96 follow-up)

Branch `b96-feedtest`, from `v12-experimental` at 567de0bd4 (beta96 shipped). Server hotfix scope. 2026-11-27.

Stuart on the Lenovo (192.168.86.13, a multi-radio Linux box with a front door) that morning:

1. **Bug:** two feeds saved in Setup (PiAware, Beast, connect 192.168.86.12:30005; UberSDR HFDL, SBS, connect
   192.168.86.13:32010) never started until the front door was restarted by hand.
2. "We need to test the connection before saving."
3. (Coordinator, same day) His UberSDR is headless and its docker-compose cannot be edited, so dumphfdl's BaseStation push
   is not an option: read the UberSDR's HFDL add-on instead.
4. "The easiest way possible please": the owner types an address, and the server works out the rest.

## RESUME HERE

- **Done and committed on `b96-feedtest`** (not pushed or merged; nothing touched on a live server):
  - Bug 1 fixed in `vibeserver/main.cpp`. The regression test fails on 567de0bd4 and passes with the fix.
  - `vibe_uberhfdl.h`: the `uberhfdl` source (HTTP poller). `vibe_feed_test.h`: TEST and FIND.
    `/vibeserver/admin/feedtest`, plus the JNI / Kotlin route for the app.
  - The setup page's feeds card and the app's `AircraftFeedsSetup`: FIND, ADD with the test run first, ADD ANYWAY, TEST
    on each listed feed, and an Advanced fold for manual entry.
  - Copy updated: About release note (new entry, and the beta96 entry's how-to), `ADSB-FEED.md`,
    `ANDROID-SETTINGS-PARITY.md`.
- **For the coordinator:**
  - Run `scripts/run-tests.sh`. New suites added to it: `test-feed-test` (C++), `test-feeds-door-watch`,
    `test-setup-feed-test`, and `test_aircraft_feeds_setup`.
  - Gradle was not run. The new C++ passed an NDK r27 `-fsyntax-only` check for armv7 API 21 and arm64 API 24
    (`test-feed-test.cpp`, which includes all the new headers). The whole shim compiled in the Mac build.
- **Live checks (a person):**
  - On the Lenovo after the update, add a feed through any of its pages; it should start within about 2 s with no
    restart.
  - Type `192.168.86.11` and press FIND. It should find the UberSDR HFDL add-on. Then ADD and Save.
  - Check the AIR & SEA map: HFDL aircraft from the UberSDR are labelled HFDL, and none of them go to flight trackers.
  - Stuart's old "UberSDR HFDL, SBS, connect 192.168.86.13:32010" entry should be removed. The new TEST button
    reports it as "Nothing is listening on 192.168.86.13:32010", with the hint "That address is this server…
    did you mean listen?".
- **Open for Stuart (one line each):**
  - Beast finds are named "PiAware" (his case; readsb looks the same on the wire). Should it be a neutral "ADS-B
    receiver"?
  - The `sig_level` and `gs_id` values from the UberSDR are not used, because the shared table has no field for them.

## Bug 1: feeds saved through a radio process never reached the front door

**Cause.** On a multi-radio box, the server-level aircraft feeds belong to the front door (`vibe_feeds.h`: "ONE OWNER
PER MACHINE"). Stuart's save went through the radio process on :48001. That process applied what it could and restarted.
In `main.cpp`, the config.json watch is the mechanism by which "a save by any process reaches the process that acts on
it", but it was guarded by `if (!frontDoorOnly && …)`, so the door never ran it. A save made through the door also did
not apply the feeds, because the save handler applies them only inside the per-radio block, which never matches in the
door. The door therefore kept its boot-time "0 sources".

**Fix (main.cpp).**
- The door runs its own config.json watch every 2 s and applies only what it owns. Each step is skipped when the value
  has not changed:
  - `applyAircraftFeeds` (the feeds and the flight-tracker hub; it was already keyed).
  - `applyCommunityDoorRadios`, the community map switches.
  - The directory listing, applied only when the fields it sends change. A new share length is sent only when that
    value itself changed.
- The door's `g_serverConfig` follows the file, as its own save already does.
- The door's first look applies too. The door's main loop starts well after its port opens, and a save in that gap would
  otherwise become the baseline and never be applied. Every applier is keyed, so an unchanged file does nothing.
- The door's own config save now applies the feeds immediately; the watch remains as the backstop.

**Test.** `scripts/test-feeds-door-watch.mjs` starts a door and one radio process (the Lenovo layout, with
`VIBESERVER_NO_RADIO_SPAWN`) on a fake rtl_tcp. It saves two feeds through the radio's own port and nonce, then checks:
- the door's `run/aircraft-feeds.json` lists both feeds;
- the door's own log applied them;
- the HFDL listener is open;
- an unrelated save re-applies nothing;
- a removal follows.

Results:

| Build | Result |
|---|---|
| 567de0bd4 | 5 passed, **5 failed**: the status file stays `[]` and the door never applies the feeds |
| `b96-feedtest` | **10/10**: both feeds listed 1.5 s after the save |

Other front-door suites with the fix: test-server-aircraft-feeds 18/0, test-bg-decode-frontdoor 12/0,
test-community-door 11/11.

## The UberSDR HFDL feed (fmt `uberhfdl`)

See `ADSB-FEED.md` §UberSDR HFDL add-on for the endpoint, the fields and the mapping. In short:
- The server sends `GET http://host:8080/addon/hfdl/aircraft` every 5 s (5 s is a minimum interval, never shortened).
- Each request is HTTP/1.0 with identity encoding and a plain User-Agent, and is bounded: 2 MB cap, 4 s deadline,
  2,000 rows.
- Each row is written into the feed's own HFDL table, exactly as SBS HFDL rows are. Rows are never sent to flight
  trackers.
- The fixture, `test/fixtures/uberhfdl/aircraft-2026-11-27.json`, is one unedited live response from Stuart's UberSDR.

## TEST and FIND (`vibe_feed_test.h`)

**One source (`{"src":{…}}`).** The source is read by the same parser the save uses (`parseSources`, given a list of
one), so the test refuses exactly what the save refuses, with the same wording.
- **Beast or BaseStation, connect.**
  - Resolve the host (3 s limit), then connect (3 s limit).
  - Read for 3 s while bytes arrive, or up to 5 s if nothing arrives.
  - Beast data is counted through `beast::Reader`, and aircraft through our own Tracker (CRC-checked). BaseStation data
    is counted as MSG lines.
  - Results, for example:
    - "Connected — 1,214 Beast messages in 3 s, 87 aircraft"
    - "Connected, but no data in 5 s"
    - "Connected, but this is not Beast data — it looks like BaseStation (SBS) text (try BaseStation)". Other formats
      recognised the same way: Beast, AVR raw text, HTTP and JSON.
    - "Nothing is listening on host:port"
    - "No route to …, or it timed out"
    - "Name not found: …"
  - A datalink source (HFDL, VDL2 or ACARS) that connects but stays silent passes, with "normal for HFDL…".
  - A refused connection to THIS machine gets the hint "dumphfdl connects OUT — did you mean listen?". This is exactly
    Stuart's mistake.
- **UberSDR HFDL.** One GET. Results:
  - "Connected — UberSDR HFDL, 23 aircraft (14 with positions)"
  - "Connected, but 404: this UberSDR has no HFDL add-on"
  - "Connected, but this is not UberSDR HFDL JSON"
  - "Nothing is listening on …"
- **Listen.**
  - If the port belongs to this feed already (`FeedSet::ownedListen`): "This feed already listens on port N — <its live
    line>".
  - If another feed or the radios' ingest holds it: "in use", naming which.
  - Otherwise the port is bound and closed at once, both dual-stack and plain IPv4, because macOS allows a dual-stack bind
    beside an IPv4 listener. The result is "free" or "in use by another program".
  - The hint always says the sender must connect to `<LAN IP>:port`.

**FIND (`{"find":"host[:port]"}`).**
- The server asks several ports at once, one thread each: the owner's port if given, 30005, 30003, 8080 and 80.
- Each port gets exactly **one** connection. The server reads whatever the port sends unprompted (Beast or BaseStation).
  If it sends nothing, the server sends the HFDL add-on's GET on that same connection.
- About 5 s in all: 1.5 s to connect, 1.2 s of silence, 1.8 s for the HTTP answer. Live against the UberSDR it took
  1.2 s.
- Each find comes back as a ready-to-add source with a suggested name.
- Duplicates are dropped:
  - a BaseStation copy when Beast was found too (same aircraft);
  - a second UberSDR port (8080 and 80 both answered live: same aircraft).
- If nothing is found, the result says which ports it tried, for example "Nothing found on … — tried 30005: nothing
  listening · …".
- Only the host the owner typed is asked. Nothing is written to it except that one GET.

**Bounded, and does not disturb running feeds.**
- `Tester` runs one test at a time per process ("busy" otherwise).
- Each test runs on its own thread at the network priority rung, joined by the request that asked for it. The request is
  a connection thread, which a stop joins.
- A test uses its own sockets only. A listen test binds and closes at once.
- `LocalSdrShim::stopLocked` calls `Tester::cancel()` before it joins the connection threads, so a running test ends
  within ~100 ms. This covers the Android in-process Impl lifetime; see `SAT-TRACKER-RESTART.md`.
- The one exception is a hostname lookup: getaddrinfo cannot be interrupted. It runs on a detached helper that holds only
  its own shared state, no server object, and is abandoned at its deadline.

**Where the test runs.**
- Web: `POST /vibeserver/admin/feedtest`, admin-gated like every setup endpoint. The page talks to the front door, which
  owns the feeds.
- App: `VibeLocalSDR.feedTest`. It asks the running server process (`VibeServerOps` "feedTest"), which owns the feeds,
  and otherwise tests inside the app's own process. It runs on a worker thread.

## UX

**Setup page, SERVER tab → "Aircraft feeds from other receivers" → "+ Add aircraft feed":**
- "The other receiver's address" field and FIND. Results are one line per find, each with a name box and ADD.
- "Advanced — a feed that connects to this server, or set it by hand" holds the manual form, with the new UberSDR HFDL
  format added. TEST tests the form's feed. ADD tests first: a pass adds the feed with the test's line; a failure shows
  why plus ADD ANYWAY.
- Each listed feed has TEST beside REMOVE, and its result appears under the row.
- The page-wide Save stores the list, as before.

**Android server screen → AIRCRAFT FEEDS FROM OTHER RECEIVERS:** the same flow. If the native module has no `feedTest`,
there is no FIND or TEST, and the manual form opens directly, so no control is offered that cannot work.

## Tests (all on the Mac)

| Suite | What | Result |
|---|---|---|
| `test-feed-test` (C++) | parser parity; fake Beast / SBS / closed / silent / wrong format; listen free / held / owned; the fixture parsed exactly (261 rows, 253 ICAO, 243 positions); fake UberSDR 200 / 404 / chunked HTML; the polite request; FIND across 6 ports (≤5.5 s, one connection per port, dedupe); busy + cancel; the poller as a Source (253 aircraft, `HFDL 8942`, reg, the 5 s minimum, one log line, down when gone) | 63/0 |
| `test-feeds-door-watch` | bug 1 (above) | 5 fail on 567de0bd4 → 10/0 |
| `test-setup-feed-test` | real page, headless **muted** Edge, door + radio: FIND a fake UberSDR → ADD; Advanced ADD passes on a fake Beast; a closed port → why + ADD ANYWAY; TEST on rows; Save → config.json; the UberSDR's aircraft on aircraft.json as HFDL | 18/0 |
| `test_aircraft_feeds_setup` (react-reconciler) | the app card: FIND → ADD, nothing found, ADD tests first, ADD ANYWAY, TEST on a row, no engine → no FIND/TEST | 19/0 |
| `test_aircraft_feeds_app` | + uberhfdl, test/find words | 24/0 |
| existing | test-aircraft-feeds 82/82, test-server-aircraft-feeds 18/0, test-bg-decode-frontdoor 12/0, test-community-door 11/11, test-setup-adsb-feed 68/0, check-setup-page, check-android-settings-parity 222/0, test-android-setup all, test-setup-android-e2e all, test-config-adsbfeed 15/0, test-config-keep 25/25, check-versions all good | pass |

**Live, read-only, against Stuart's UberSDR (192.168.86.11; standing OK to read from it):**
- FIND `192.168.86.11` in 1.2 s returned "Found: UberSDR HFDL (port 8080) — 233 aircraft (222 with positions)". Ports
  30005 and 30003 had nothing listening. Port 80 is the same UberSDR, and is now deduplicated.
- TEST on :8073 returned "Nothing is listening on 192.168.86.11:8073".
- A 7-second Source run got 224 aircraft into the table, 223 of them with positions, with registrations.

No other server was contacted.
