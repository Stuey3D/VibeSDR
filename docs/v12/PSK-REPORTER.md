# PSK Reporter, WSPRnet and the ranking card (V12 beta90)

Stuart, 2026-11-21: an owner switch, OFF by default, that sends this server's digital spots to PSK Reporter under the
owner's callsign; a second switch for WSPR spots to WSPRnet; and the server's place in the receiver rankings shown in
the gallery. Branch `b90-report`.

The ranking idea is **M9PSY's**: he built PSK Reporter / WSPR rank fetching into UberSDR. Ours is our own code, written
from the two sources' public pages. Nothing of his was copied.

## RESUME HERE

- **Built and tested:** the reporter (server), all settings surfaces, the directory Worker's ranking round, the gallery
  card (web + app). Everything below "Tests" passes.
- **Not deployed, and must not be without Stuart:** the directory Worker (`directory/src/ranks.js`, the new `41 * * * *`
  cron, `/api/ranks`). Apply `directory/migrations/0011-ranks.sql` BEFORE deploying the Worker.
- **Never sent from a test:** nothing in this branch has sent a datagram to report.pskreporter.info or an upload to
  wsprnet.org. The first real send is the first time an owner switches it on, and needs checking by a person:
  1. PSK Reporter: switch on with a real callsign on a server hearing FT8, wait ~6 minutes, look the callsign up as a
     receiver on https://pskreporter.info/pskmap.html. If nothing appears, PSK Reporter's packet analyser on port 14739
     (see its developer page, "Testing") shows how a datagram was read — point `Config::pskPort` at 14739 for that one
     test, by hand.
  2. WSPRnet: with the second switch on, check https://www.wsprnet.org/olddb ("Show spots heard by" the callsign). The
     line format is the documented ALL_MEPT one, but WSPRnet's parser has only been read about, not run against.
- **Open questions for Stuart:** see the bottom of this file.

## What is sent (PSK Reporter)

Written from the developer page **https://pskreporter.info/pskdev.html** (read 2026-11-21). `test-spot-report` checks
our encoder byte for byte against that page's own complete example datagram and its "later in the session" datagram.

- **Transport:** IPFIX (RFC 5101 / 7011) over UDP to `report.pskreporter.info:4739`. One socket for the reporter's life,
  so every datagram has the same source port (the page asks for this).
- **Receiver record** (every datagram): receiverCallsign (the owner's), receiverLocator (the server's grid: the Setup
  page's locator, else worked out from its latitude and longitude; on Android, RECEIVER LOCATION), decoderSoftware
  `VibeServer <version>`, antennaInformation (the owner's text, else the radio's port name from `antennaNames`, else its
  antenna description).
- **Sender records:** senderCallsign, frequency (Hz, 4 bytes), sNR (1 byte), mode (ADIF name), informationSource = 1
  ("automatically extracted"), senderLocator (the station's own grid if it sent one, else empty), flowStartSeconds (the
  start of the slot it was heard in).
- **Our descriptors:** the page's fourth sender descriptor without iMD (we never send an IMD we did not measure), with
  our own template ids 0x5652 / 0x5653 (the page: "advisable to have distinct values"). IPFIX is self-describing; the
  page invites a descriptor that fits the software. → open question 2.
- **Descriptors** in the first three datagrams, then once an hour.
- **Sequence number** counts the data records sent (RFC 7011; the page: "the count of reports").
- **Modes:** only those with an ADIF name: FT8, FT4, JS8, WSPR, Q65, MSK144, PSK31, PSK63, FST4, FST4W, JT65, JT9.
  FT2 and JTTY have none yet and are not sent; CW (the skimmer) is not included.
- **Never anything about listeners.** The spots come from the decoders (`DecoderHost::emitSpot`, both a listener's run
  and background decoding); no record names who was listening, or whether anyone was.

### Batching (the page's rules)
- A batch every **5 minutes from when the reporter started, + 0–60 s** at first and + 0–30 s after each batch; never
  on the clock's minute ("must not be synchronized to the system clock").
- **Each call + band + mode at most once per batch** (the first record kept, a later locator taken). beta94: was each
  call — a station on two bands or modes in one batch lost the second; PSK Reporter's limit is per call + band + mode.
- **The same callsign + band + mode not again for an hour** (the page: "Ideally … once per hour if it has not
  'changed'. A change might be a move to a different band"). A new band is reported at once.
- A full batch splits into datagrams of at most **1400 bytes**, each carrying the receiver record.
- Bounded: 8000 waiting call|band|mode reports (beta94, was 2000; ~270 B each, ~4.3 MB at a full batch's peak), 16384
  entries in the repeat memory, 600 WSPR lines. What a full batch turns away is counted once per call|band|mode
  (`droppedHour` / `droppedTotal` in the status; the setup page's line says "dropped: N (queue full)").

## WSPRnet

Written from WSPRnet's upload form (https://www.wsprnet.org/olddb, "Upload your log!") and its published note
**https://www.wsprnet.org/automate.txt** ("How can I automate this?"): an HTTP POST (multipart) of a file element
`allmept` with `call` and `grid`, to `http://wsprnet.org/meptspots.php`. No account.

- **WSPR spots only**, each transmission once (callsign + slot), sent with the PSK Reporter batch.
- **Line format** (ALL_MEPT / ALL_WSPR, as QRP Labs AN002 also shows):
  `YYMMDD HHMM sync snr dt freqMHz call grid dBm drift`, e.g. `110101 0102   5 -21  0.3   10.140211  K1JT FN20 30 0`.
  `sync` is written as 5. We have no WSPR-program quality figure, and the parser's use of it is not documented.
  → open question 3.
- `digi::Spot` gained `dbm` and `drift` (set only by the WSPR engine).
- A failed upload (not 200) is said in words on the status line and kept **once** for the next batch.
- Our own small HTTP/1.1 client over a socket (`postHttp`), with deadlines of 10 s to connect and 20 s to answer. This
  is the same on the daemon and on Android, which has no curl.

## The status line

"412 spots sent to PSK Reporter in the last hour, last 16:40 · 37 WSPR spots sent to WSPRnet in the last hour, last 16:40".
- Each radio process writes `spotreport-<pid>.json` into the runtime directory (`handoffDir()`), and the setup page's
  config carries `spotReportStatus`, which is all of them added up (`aggregateStatus`, files under 2 h old).
- On Android the page's config carries the one process's line (JNI config handler). The phone's screen reads
  `GET /vibeserver/gallery/spotreport.json`, which gives counts and times only, every minute while the server runs.

## The ranking card

- **Directory Worker** (`directory/src/ranks.js`, hourly cron `41 * * * *`, D1 table `rank_mirror`):
  - **PSK Reporter:** one GET of https://www.pskreporter.info/cgi-bin/pskstats.pl. It reads the two tables by their
    variable names: `reportResult` ("Top Monitors by reports over last 24 hours") and `countryResult` ("Top Monitors by
    number of different countries reported"). Each has the top 50 a band, `{"callsign","day","week"}`; we read `day`.
  - **wspr.live:** one query of its public ClickHouse (`https://db1.wspr.live/?query=`, the only parameter it keeps). The
    query takes the last 24 hours and ranks receivers by unique transmitters heard, the top 100 a band. wspr.live's
    terms: free use as long as the results are free to everyone. A receiver only appears there if it uploads to WSPRnet,
    which is the second switch.
  - **Places use competition ranking** (ties share a place: 1, 2, 2, 4).
  - **Manners:** a clear User-Agent `VibeSDR-directory-ranks/1 (+https://vibesdr.net)` with no person's address, and
    compression asked for. There is at most one round in 50 minutes. A failure holds that source for 1 h, doubling to a
    day, and its last good copy keeps serving. A 200 that is not the page never replaces a good copy. Answers are
    capped at 4 MB.
  - **Storage:** the lookup is kept in buckets by the callsign's first character (about 40 rows and 40 writes an hour).
    `GET /api/ranks?call=G4ABC` returns that callsign's places only, with `psk`/`wspr` `{fetched, window, source,
    bands}`, cached for 15 minutes. `GET /api/ranks/status` gives the round's state in words.
- **Server:** with reporting on and a callsign, the reporter asks the directory hourly (+ 0–5 min) for its own callsign.
  The daemon asks through curl (`spotHttpGet`, no shell, 20 s, 64 KB cap); Android asks through VibeHttp. It keeps the
  last good answer and serves it at `GET /vibeserver/gallery/ranks.json`. That gives `{"on":false}` unless reporting is
  on, and the gallery's privacy switch covers it. The front door asks too, because it serves the gallery.
- **Card** (`src/utils/spotRanks.ts`, web + app): on page 1 of ALL, "G4ABC · PSK Reporter: No. 5 on 40 m for reports ·
  No. 2 on 60 m for countries · WSPR: No. 3 on 40 m (431 stations heard)". The second line names each source, its
  window and its age. There is nothing to press.
- The page's tables are **24-hour** figures, not the last hour, so the card says "last 24 hours". There is no "callsigns"
  ranking on the page; the two measures it has are reports and countries.

## Settings

| Where | Keys |
|---|---|
| `vibeserver.json` (ServerConfig, machine-level) | `pskrOn`, `pskrCall`, `pskrAntenna`, `wsprnetOn` (all off / empty by default) |
| Setup page | Server tab, **PSK Reporter** card (after the gallery); a pointer under Background Decoding on the radio tab. The save is refused in words when the switch is on with no callsign. |
| Android | `vibe_android_setup.h` table / pageConfig / settingsPatch; `serverSettingsMirror.ts`; ServerModeScreen **PSK REPORTER**; `nativeServerConfig`; VibeServerBoot → `VibeLocalSDR.setSpotReporting` (JNI) |
| Shim | `LocalSdrShim::setSpotReporting`, `setSpotReportStatusDir`, `setSpotReportFetcher` |

## Tests (what was run, how)

- `vibeserver/test-spot-report.cpp`: **59/59.** The IPFIX golden bytes against the page; our descriptors decoded by an
  independent reader in the test; batching and dedupe against a **local UDP socket on 127.0.0.1**; the timing, the
  descriptors, the sequence and the split; off and no-callsign; WSPRnet against a **local HTTP sink**; the status line,
  the aggregate, and the ranking (a fake fetcher).
- `vibeserver/test-config-pskr.cpp`: 5/5 (the config round trip, off by default).
- `directory/scripts/test-ranks.mjs`: **32/32.** Real SQLite with every migration, a fake network, and fixtures trimmed
  from the real answers (`directory/scripts/testdata/`, fetched once each on 2026-11-21).
- `scripts/test_spot_ranks.ts`: 8/8 (the card's words).
- `check-remote-keys` 470/470, `check-android-settings-parity` 193/193, `test-android-setup` + e2e green,
  `check-setup-page` green, `tsc` clean for our files. The only errors left are the existing `node:` types in
  handheld-ui/test and scripts/wefax-world.
- **Not run:** a real send to PSK Reporter or WSPRnet (by the rules, and see RESUME HERE). No live end-to-end through
  the full server binary: the binary builds, and the units are tested.

## Open questions for Stuart
1. **Deploy** the directory Worker (and apply migration 0011 first)?
2. **Tell PSK Reporter's maintainer (Philip Gladstone)** about a new reporting program, and about our sender descriptor
   (the page's fourth without iMD)? His page invites contact for software that does not fit the standard descriptors.
3. **WSPRnet's `sync` column:** written as 5. Check the first real upload on wsprnet.org/olddb.
4. Should CW skimmer spots go to PSK Reporter too? They are left out for now.
