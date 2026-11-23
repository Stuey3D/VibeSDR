# THE LOGS' RADIO FILTER (V12 beta92)

Stuart, 2026-11-22: *"we dont separate the digital spots, navtex etc. The only one that should be separate logs per
radio on the server should be the DX log as that is used for measuring sdr and antenna performance"* — and *"in the logs
themselves we could filter by sdr if multiple picked up the same signal"*.

So: every decoder log stays **one per server** (each radio is its own process, `vibeserver@<serial>`, feeding the same
files under flock). Each entry now only **remembers which radio(s) heard it**, and the log views offer a RADIO filter.
Filtering is a view — nothing is split, nothing is deleted. The DX Log is unchanged (still per radio).

## RESUME HERE

Branch `b92-logradio` (off v12-experimental fe5b674f6). Built and tested on the Mac; not merged, not released.

Done:
- **Server**: `android/app/src/main/cpp/vibe_log_radio.h` (new) — this process's radio id, the machine's radio names, the
  `<gallery>/radios.idx` registry, the `"ri"` id-list helpers. Station store rows carry a radios mask; gallery index lines
  carry `"ri"`. `stations.json` and `gallery.json` take `?radio=<id>` and answer `"radios"`. `vibeserver/main.cpp` sets the
  id and the names.
- **Web**: the gallery's radio chip row (`#galRadio`); the DIGI MODES MAP window's radio keys (`#rads`) and "Heard by" in
  its popup.
- **App**: GalleryOverlay's radio chips; the digi map's "All radios" select in the WebView filter row.
- Copy: AboutOverlay beta92 note; `sdrTour` VIEWS card mentions the radio picker.

Not done (open — each one line in the report):
- `vibe_web_page.h` was rebuilt for the web test but **not committed** (seven agents rebuild it; the coordinator runs
  `node scripts/build-web.mjs` once after merging).
- The NAVTEX MAP (`navtex-stations.json`) and the SHIPPING MAP's AIS ship store (`ais-ships.dat`) carry no radio yet —
  the NAVTEX *messages* in the gallery do (their items are tagged), so the gallery's filter covers NAVTEX; the two map
  stores would need their own fields (`vibe_navtex_map.h` folds gallery lines, so it could read `"ri"` cheaply; the ship
  store's fixed records have no spare bytes checked).
- **ISM stays per radio (decision for Stuart).** Its private devices (TPMS, key fobs, remotes) are named by a SipHash under a
  salt drawn fresh in each PROCESS and never stored (the privacy rule, `vibe_ism_service.h`). Two radio processes therefore
  give the same car different nicknames, and could give two different cars the same "TPMS #2" — merged into one card, that
  breaks the rule that "TPMS #2" never means two cars, and a private device heard by both would be counted twice. A
  server-wide ISM day needs ONE nickname space for the machine: e.g. the salt shared through the runtime directory (tmpfs,
  0600, gone at reboot) or a front-door nickname service. Each ISM item IS tagged with its radio, so the filter works on it.
- Hourly summary cards (FT8 / CW / PSK / RTTY / NDB) and NDB's own last-10 card stay the run's own (an hour is written
  once, when it closes). The day items and the FT8-family last-10 card are server-wide (below).
- The R36S native UI (one radio) and the watch: nothing to do (one radio = no filter).

## Shared day items — one per server (beta92, the coordinator's follow-up)

Stuart's rule (2026-11-22): every decoder log is one per server, only the DX Log per radio. `android/app/src/main/cpp/
vibe_gallery_shared.h` gives the writers what FUNcube's day log (`vibe_fctlm_gallery.h` FcLock) already had:

- **Lock** — flock on `<gallery>/.shared.lock`; every find-or-create, dedupe, append, merge and rewrite of a shared item is
  inside it (taken before the gallery's own `.lock`, which Store takes inside each call).
- **adopt()** — the item by its SLOT alone (`adoptSlot` anyWriter): never the radio, profile, origin or dial.
- **Dedupe** — a day log line is `HH:MM:SS  <text>`. A line is the same reception as ANOTHER radio's when the file's tail
  (last 256 KB) holds the same text within 20 s; each tail line answers for one of ours. `OwnLines` remembers what this
  process wrote in the last 3 minutes, so a radio's own genuine repeat (a coast station polling a ship twice) is never
  taken for the other radio's copy, and one batch never dedupes itself.
- **State** — a summary card's rows in `<gallery>/.shared/<slot>.state` (tab-separated text), merged under the Lock and the
  card drawn from it. Pruned two days after its last write; counted in the gallery's space (`sideBytes`).
- After an append the item is retagged, so its `"ri"` gains this radio.

| Decoder | What is one per server now | How |
|---|---|---|
| AIS / APRS (`vibe_station_log.h` DayLog) | the day's log per channel (and its parts) + the summary card | log lines deduped; the card's rows (count of DISTINCT messages, first, last) in the shared state |
| DSC (`vibe_dsc_gallery.h`) | the day's log per DSC frequency; a distress / urgency / pinned item | log deduped, title counts from the FILE; an alert another radio saved within 2 min (same title) is one item, retagged |
| NDB (`vibe_ndb_gallery.h`) | the day's heard log + map | each card write reads the card (every radio's beacons) and merges ours over it: first heard earliest, latest details, best SNR |
| FT8 / CW / PSK / RTTY (`vibe_bgdecode.h` Ft8SummaryTap) | the "last 10 minutes" card, per kind and dial | slot `last10-<kind>-<kHz>`; a shared 10-minute window of every radio's spots — a spot another process already has (same call + mode within 10 s) is the same transmission |
| NAVTEX map (`vibe_navtex_map.h`) | — (already server-wide) | per station `radios` (from its messages' `ri`) + the answer's `radios` list |
| AIS ship store (`vibe_ais_store.h`) | — (already server-wide) | `uint32_t radios` in the last 4 of the 512-byte record (zero before); per ship `radios` + the answer's list |

Old per-radio items stay as they are. The old FT8 `last10` slot is not adopted (the new slot has the kind and dial);
today's newest per-radio AIS/APRS/DSC/NDB item IS adopted by slot and becomes the shared one.

## How it works

### The radio id
`vsconfig::radioId(serial)` — eight hex characters, the id the radio's links and the landing list already use. Never the
serial (the gallery is public). The front door writes nothing, so it has none; the Android app's own radio has none
(its entries are "unknown"). Set in `main.cpp` beside `setGalleryRadioLabel`, always — a one-radio server's entries are
tagged too, so when a second radio arrives the old entries already say who heard them.

### The station store (`vibe_station_store.h`)
- `Rec`'s four pad bytes (always written 0 by beta90/91, `Rec r{}`) are now `uint32_t radios` — a mask; **0 = unknown**,
  so every older file reads as unknown and passes every filter. No format version change, no migration.
- Bit *n* = line *n* of `<gallery>/radios.idx` — append-only, one id a line, at most 32, under `flock` on the file. Every
  process resolves its bit at flush (`resolveRadio_`), so the same id gets the same bit on every process. A torn last
  line is closed before the next id is appended.
- Merged by OR in `mergeInto` — under the day file's flock, exactly where the counts merge as deltas — so a station two
  radios heard is one row listing both. A row spotted before the bit was resolved gets it at flush (the jobs are OR'd).
- `mapJson(…, radio)`: per-station `"radios":["id",…]` (absent = unknown), a top-level `"radios":[{"id","name","n"}]`
  (the radios the window holds, named, with station counts) and `"radioUnknown"`, and `radio=` filters the answer
  (unknown rows always pass). `byMode` follows the filter.

### The gallery (`vibe_gallery.h`)
- Every index line this build writes carries `"ri":"<id>[,…]"` (≤ 8 ids). `add`/`create` write the writer's id;
  `replace`/`retag` union the line's existing ids with the writer's (FUNcube's shared day log ends up listing every
  radio that rewrote it); `rewrite` keeps the line, `ri` and all. A line with no `ri` (older builds) = unknown.
- The compact table's `Row` gains a per-process radios mask (bits of `Cache::rids`, never written anywhere): 16 → 20
  bytes an item (200,000 items: 3.2 → 4 MB). Filtering and the per-radio counts are a walk of the table — no file read.
- `listJson(kind, limit, offset, radio)`: `radio=` is a view (that radio's items + the untagged), `"total"` is the view's
  so the pager is right; `"radios"` lists the radios the list (this kind) holds; `"radio"` echoes the filter.

### The names (`main.cpp`)
`setMachineRadios` — the config's radios in the owner's display order, `publicLabel(label or driver)` as the landing
cards name them, re-read when the config file changes (mtime + size). A radio no longer in the config keeps its id as
its name, so its entries can still be picked out.

### The API
- `GET /vibeserver/stations.json?window=…[&since=…][&radio=<id>]`
- `GET /vibeserver/gallery.json?offset=…&limit=…[&kind=…][&radio=<id>]`
- `radio` must be a whole `[0-9a-z]{1,16}` id; anything else is ignored (the full list).

### The clients (`src/utils/logRadio.ts`, one rule for both)
- `logRadioChoices`: a filter only when **2+ radios hold entries** in what is shown; otherwise nothing is drawn (a
  one-radio server, or a kind only one radio wrote — e.g. FT8 cards from the HF radio only).
- A pick that is no longer offered falls back to ALL (`keepRadioPick`).
- Gallery (web + app): `radio=` on the server (it pages); the item line says "Heard by A and B" when 2+ radios heard it.
- DIGI MODES MAP: client-side over the stored stations (the whole window is fetched anyway); this listener's own live
  spots carry no radio and always show. Web: keys beside the time window. App: a select in the WebView filter row, the
  choices pushed with `__radiosPush`, the stations' radios carried through `SpotRow.radios`.

## How it was tested

All on the Mac (M5 Pro), synthetic data only — no radio, no server of Stuart's touched.

| Test | What | Result |
|---|---|---|
| `vibeserver/test-station-store.cpp` (+ `testRadios`) | two **forked processes** with two radio ids flush one day: the station both heard is one row, count 20, both radios listed; one-radio stations list one; the registry; `radio=`; an **old-style row** (mask 0) reads as unknown and passes every filter; a row spotted before the bit was known | 103 / 0 |
| `vibeserver/test-log-radio.cpp` (new, in run-tests.sh) | the helpers (ids, split/union, ≤ 8), the registry (torn line), two **forked processes** on one gallery (NAVTEX each + a shared FUNcube item via `adoptSlot` anyWriter + `replace`) → the shared item lists both; `listJson` names + counts per kind, `radio=` + paging, junk / unknown ids; an **old line** with no `ri` = unknown, shown under both; retag/rewrite keep radios; one radio → one listed | 28 / 0 |
| `scripts/test_log_radio.ts` (in run-tests.sh) | the client rule: when offered, what passes, the pick kept/dropped, the query, "Heard by" | 17 / 0 |
| `scripts/test-web-log-radio.mjs` | a real `vibeserver` (this tree) + the real built page in **headless Edge, `--mute-audio`**: the API (names, counts, filters, the shared item, junk dropped, stations.json radios); the gallery's chip row appears with 2 radios and filters (VHF: 6 rows, none of HF's); **no row for FT8** (one radio) and the pick falls back to ALL; the DIGI MODES MAP window's keys and filter | 21 / 0 |
| `vibeserver/test-shared-day.cpp` (new, in run-tests.sh) | the dedupe helpers (another radio's line within 20 s; own repeats kept; one tail line per ours); two forked radios on one AIS day: ONE log + ONE card, the shared message once, each radio's own, both in `ri`, the card's counts the server's | 12 / 0 |
| `test-dsc` (+ section) | two forked radios on 2187.5 kHz (different dial, origin, profile), the same distress alert: one log, the alert once in it, ONE distress item, both radios in `ri`, "3 calls · 1 distress" | all passed |
| `test-ndb` (+ section) | two forked radios: WOD heard by both (different SNR) + one beacon each: one card, WOD once at the better SNR, 3 beacons, both in `ri` | 104 / 0 |
| `test-bgdecode` (+ `ft8SharedLast10`) | two forked radios on 1840 kHz: ONE last-10 card, 5 spots / 4 countries (two decoded by both, one each, a repeat), both in `ri`, "every radio on this server" | all passed |
| `test-navtex-map` (+ section) | a station heard by both radios lists both, one by one, an untagged one none; the named list | 62 / 0 |
| `test-ais-store` (4-process test + radio ids) | the shared coast lists all four radios, each radio's own ships only it; JSON names them; an old record is unknown | 58 / 0 |
| regressions | test-gallery-store (all pass), test-fctlm-gallery 49/0, test-ism-gallery 57/0, test-navtex-map 58/0, test-ndb-gallery-clean (pass), test-dsc (pass) | pass |
| `npx tsc --noEmit -p .` | the app + web | 45 errors before and after, all pre-existing in `handheld-ui/test/*` — none new |

CPU / memory: the store's per-spot path gains one atomic load; a flush resolves the bit once per gallery (one small
file read). The gallery's table is +4 bytes an item. `test-station-store`'s cost rows are unchanged (200,000 spots
33.6 ms; flush of 3,072 rows 2.2 ms; a 30-day answer 5.0 ms).
