# ADS-B enrichment data: the VRS-style list and card (research, 2026-10-24)

Research only: no code. It covers where Virtual Radar Server (VRS) gets each item in its list and card, what open
data we could use instead, what the licences require, and a design for VibeServer and the clients. Companion to
`ADSB.md` (the decoder, the report, `web/mapkit/adsblayer.js`).

Anything I could not read at first hand is marked **UNVERIFIED**. Licence wording is quoted from the licence
files themselves.

---

## Executive summary

**We can ship now, with no licensing doubt:**

1. **Country and military, from the ICAO address alone.** These come from our own table of ICAO 24-bit address
   blocks. We derive it from ICAO Annex 10 Vol III Table 9-1 and check it against VRS's `code-blocks.csv`, which is
   **CC0** and also marks 414 military sub-blocks. No database, no network, nothing to keep up to date. Flags are
   drawn from ISO alpha-2 codes, either as MIT-licensed SVGs (`lipis/flag-icons`) or as emoji.
2. **Route (origin → destination), from VRS standing data.** The route CSVs are **CC0** (about 621,000 callsigns,
   19 MB uncompressed, updated daily) and come with airports (name, lat/lon). The server looks routes up
   **locally**, so a route costs nobody an API call. A "plausible" check (is the aircraft near the great circle?)
   is our own code.
3. **Type, model, manufacturer, engines, wake category, species (land/heli/sea), operator name and callsign.**
   These come from Mictronics' type and operator tables (**ODC-By 1.0**) and the VRS `model-type` and `airlines`
   tables (**CC0**). All are small (under 0.5 MB each).
4. **Registration, type code and military flag per aircraft.** These come from the Mictronics aircraft database:
   **ODC-By 1.0**, 452,202 aircraft, a 3.35 MB zip, weekly. ODC-By has **no share-alike**. All it asks is a licence
   notice where the data is conveyed, plus the line "Contains information from Mictronics aircraft database, which
   is made available under the ODC Attribution License" wherever the output is shown publicly. We can do that in
   the card's ⓘ and on the About page.
5. **Photos as a client-side link-out to Planespotters**, with the photographer's credit and a plain link. The
   browser or app fetches each photo directly. The server must **not** proxy, store or re-serve them.

**Needs a decision (Stuart):**

- **How the databases arrive.** Bundled in the .deb/app, downloaded weekly by each owner's server, or mirrored on
  vibesdr.net. All three are allowed for CC0 and ODC-By; they differ in freshness, size and politeness (§4.2).
  Recommendation: a small bundled seed, then a weekly conditional download into the data directory, off by
  default on Lite/Android if RAM is tight.
- **Owner-supplied logo and silhouette folders.** These use VRS-compatible names, so existing packs can be reused.
  We would ship none and link to none from inside the product (§1.4).
- **Showing owner or operator names for private aircraft.** Recommendation: never show an individual's name. Show
  "Private", as VRS does with `IsPrivateOperator`. Honour the FAA LADD/PIA flags (§3.4).
- Whether to also accept an owner's own **`BaseStation.sqb`** (read-only), since VRS and BaseStation users already
  have one.

**Avoid:**

- **OpenSky aircraft database and API.** The licence is "solely for the purpose of non-profit research and
  non-profit education". "Operational use of the REST API in any live product … requires a written license".
- **adsbdb.com routes and aircraft.** Its README says the route data "may not be copied, published, or
  incorporated into other databases without the explicit permission of David J Taylor". Its aircraft data comes
  from PlaneBase, a commercial source. The MIT licence covers the code only.
- **ADS-B Exchange `basic-ac-db.json.gz`, and so `tar1090-db`'s `aircraft.csv.gz`, which merges it.** No data
  licence is stated. The site points to JetNet's terms of use and says commercial use needs written authorisation.
  tar1090-db states no licence at all.
- **UK CAA G-INFO.** The CAA asserts copyright and database right, and the download is licensed for a single PC.
- **ODbL sources (OpenFlights) and CC BY-SA sources (Wikipedia tables)** for anything we merge into a database.
  Share-alike would reach our merged database (§3). We do not need them: CC0 and ODC-By cover the same ground.
- **Airline logos and "operator flag" packs** in our tree, our packages or our auto-downloads. They are
  third-party trademarks. The best-known pack (`rikgale/VRSOperatorFlags`) is labelled GPL-3.0, which a logo
  author cannot grant over someone else's trademark anyway.
- **Any GPL code** (readsb, tar1090, dump1090-fa, `VRSOperatorFlags`). We take facts and formats, never code.
  VRS itself is **BSD-3-Clause**, but we still write our own.

---

## 1. How VRS gets each item

VRS is Andrew Whewell's program. The source is at github.com/vradarserver/vrs under **BSD-3-Clause** (`LICENSE.md`:
"Copyright (C) 2010 onwards, Andrew Whewell. All rights reserved. Redistribution and use in source and binary
forms, with or without modification, are permitted provided that …"). Reading it to understand behaviour is fine,
but we copy nothing.

### 1.1 Standing data (routes, airlines, airports, model types, code blocks)

- VRS's `StandingDataUpdater` downloads `http://www.virtualradarserver.co.uk/Files/StandingData.sqb.gz`, an SQLite
  database (15.8 MB gz, `Last-Modified` 23 Oct 2026). It first checks a small state file,
  `http://www.virtualradarserver.co.uk/Files/FlightNumberCoverage.csv`, which carries MD5s per table and a count
  (`2009-12-01,2026-10-23,620962,…`), so a client only downloads when something changed.
- The data is user-submitted to Andrew's **SDM** site (sdm.virtualradarserver.co.uk). The CSV form is published
  at **github.com/vradarserver/standing-data** under **CC0 1.0 Universal** (`LICENSE`: "Creative Commons Legal
  Code — CC0 1.0 Universal"). A bot commits "Standing data changes" daily (latest commits: 2026-10-23 and
  2026-10-24, 03:49 UTC).
- Folders, each `schema-01` (the README promises columns are only ever appended):

| folder | files | size | columns |
|---|---|---|---|
| `routes/` | 1,576 | 19.1 MB | `Callsign, Code, Number, AirlineCode, AirportCodes` (hyphen-separated, in flight order, e.g. `BAW1,BAW,1,BAW,EGLL-KJFK`). File per code: `B/BAW-all.csv`, or `BAW-0.csv`…`BAW-9.csv` above 10,000 routes |
| `airports/` | 659 | 2.6 MB | `Code, Name, ICAO, IATA, Location, CountryISO2, Latitude, Longitude, AltitudeFeet` (file per 2-letter prefix: `E/EG.csv`) |
| `airlines/` | 1 | 0.18 MB | `Code, Name, ICAO, IATA, PositioningFlightPattern, CharterFlightPattern` (5,965 rows) |
| `model-type/` | 28 | 0.42 MB | `ICAO, Manufacturer, Model, Engines, EngineTypeCode (E/J/P/R/T), EnginePlacementCode, SpeciesCode (A/G/H/L/S/T), WakeTurbulenceCode (L, L/M, M, M/H, H), IsActive` |
| `code-blocks/` | 1 | 0.03 MB | `Start, Finish, Count, Bitmask, SignificantBitmask, IsMilitary, CountryISO2` (811 rows; overlapping, "look for the smallest range that contains" the address; 414 military rows) |
| `countries/` | 1 | — | `ISO, Name` (250 rows) |
| `registration-prefixes/` | 1 | — | `Prefix, CountryISO2, HasHyphen, DecodeFullRegex, DecodeNoHyphenRegex, FormatTemplate` |
| `aircraft/` | 671 | 1.6 MB | `ICAO, Registration, ModelICAO, Manufacturer, Model, ManufacturerAndModel, IsPrivateOperator, Operator, AirlineCode, SerialNumber, YearBuilt` |

- The routes README documents **callsign normalisation**: split code and number with
  `^(?<code>[A-Z]{2,3}|[A-Z][0-9]|[0-9][A-Z])(?<number>\d[A-Z0-9]*)`, swap an IATA code for the ICAO code, and
  strip leading zeros (`EZY0001 → EZY1`, `U21234 → EZY1234`). That is a documented algorithm in a CC0 README, so
  we implement it ourselves.
- ★ **The `aircraft/` folder is "just the aircraft lookup data submitted by VRS users since June 2022 … quite a
  small subset"**. The bulk of VRS's per-aircraft data originally came from **PlaneBase**, and "None of the
  `PlaneBase` data is reproduced here" (last PlaneBase update April 2021). So the VRS GitHub data is excellent for
  routes, airlines, airports and types, but thin for registrations.
- Provenance caveat (low risk): `model-type` matches ICAO Doc 8643 designators and wake categories, and
  `airlines` matches Doc 8585 designators. VRS dedicates them CC0. Designators are facts, but ICAO's own
  documents are copyright. Mictronics carries the same caveat. **UNVERIFIED:** whether ICAO objects to bulk
  reuse. I know of no case where it has, and the whole hobby rests on it.

### 1.2 BaseStation.sqb and the online lookup

- `BaseStation.sqb` is the SQLite database of Kinetic's BaseStation program. VRS reads it, and with the
  BaseStation-database-writer plugin writes it: an `Aircraft` table keyed by `ModeS`, with registration, ICAO type,
  `OperatorFlagCode`, owner and more. Owners fill it from community sources or by hand. It is the owner's own
  file, and its contents are whatever they put in.
- **The online lookup** (`AircraftOnlineLookupProvider.cs`) works like this:
  1. VRS reads its settings from `http://sdm.virtualradarserver.co.uk/Aircraft/GetAircraftLookupSettings?language=en`.
     Today that returns
     `{"LookupByIcaoUrl":"http://sdm.virtualradarserver.co.uk/Aircraft/GetAircraftByIcaos","DataSupplier":"","SupplierCredits":"","SupplierUrl":"","MinSeconds":5,"MaxSeconds":60,"MaxBatchSize":1000}`.
  2. It POSTs `icaos=<hex>-<hex>-…` in batches of up to 1,000, at least 5 s apart, backing off to 60 s on failure.
  3. It caches the answers in a local SQLite file (`AircraftOnlineLookupCache`).
- The SDM site publishes **no terms or licence** for this endpoint. Its footer says only "Copyright © Andrew
  Whewell 2010 onwards". It is plainly provided **for VRS installations**. ★ **We should not call it.** Doing so
  would mean using another project's private service under its client's identity, which goes against
  `third_party_receiver_etiquette`. If we ever wanted it, we would ask Andrew first.

### 1.3 Photos

- VRS ships a plugin, `Plugin.AirportDataThumbnails`, which calls
  `http://www.airport-data.com/api/ac_thumb.json?m=<icao>&r=<reg>&n=<count>`. That returns an image URL, a link to
  the photo page and the photographer's name. I found **no published terms** for that API (**UNVERIFIED**). VRS
  can also show an owner's local picture folder.
- Airliners.net and JetPhotos: there is no public API. The convention is a **link-out** to their search page for
  the registration. We would do only that, if anything.

### 1.4 Operator flags and silhouettes

- VRS serves them from two **owner-configured folders** (Options → Data Sources: "Operator flags folder",
  "Silhouettes folder"). From `ImageServer.cs` (`CreateLogoImage`):
  - The client asks for `images/File-<candidates>/OpFlag.png` or `…/Type.png`, where `<candidates>` is a
    `|`-separated list.
  - The server tries each candidate as **`<candidate>.bmp`** in the folder. The first one that exists wins.
  - The image is widened or heightened to **85 × 20 px**. A missing image gives a blank 85×20.
- **The candidate order** (`format.ts`, `buildLogoCodeToUse`):
  1. The **ICAO 24-bit hex** (for one specific airframe).
  2. The **registration**, if "allow registration flag override" is on.
  3. The **logo code**: the aircraft's `OperatorFlagCode` for flags (normally the operator's ICAO 3-letter code,
     e.g. `BAW`), or the **ICAO type designator** for silhouettes (e.g. `B738`).
- Community packs extend this by putting richer codes into `OperatorFlagCode`.
  **github.com/rikgale/VRSOperatorFlags** (licence field **GPL-3.0**, updated 2026-05) has about 6,510 operator
  flags in 85×20 BMP, named `[airline ICAO]-[type ICAO][suffix]`, e.g. `RYR-B738`, `THY-A332SA`, plus GA flags by
  registration (`G-AZRZ`) and 1,319 silhouettes by type. Its companion tools set `OperatorFlagCode` in the owner's
  BaseStation.sqb so those names match.
- ★ These images are other companies' **trademarks** (airline logos and liveries). A GPL label from the pack's
  author does not licence the marks. VRS itself ships only a basic silhouette set (**UNVERIFIED:** the provenance
  of VRS's stock images). **We bundle none, auto-download none, and link to none from inside the product.** We can
  document "a VRS-format OperatorFlags/Silhouettes folder works" so owners can point at what they already have.

---

## 2. Open alternatives

### 2.1 Aircraft by ICAO 24-bit address

| source | licence (quoted) | size / count | cadence | fields | verdict |
|---|---|---|---|---|---|
| **Mictronics aircraft database** github.com/Mictronics/aircraft-database (`indexedDB.zip`) | "Aircraft database exports are made available under the Open Data Commons Attribution License" (ODC-By 1.0; the zip comment: "Generated by Mictronics. 18.10.2026 Made available under the Open Data Commons Attribution License.") | 3.35 MB zip / 14.7 MB JSON; **452,202 aircraft**, 6,310 operators, 2,788 types | "File update once per week" (commits 2026-10-11, -18) | `aircrafts.json`: hex → `[reg, type, flags, (desc)]`; flags `"10"` = military (17,068), `"01"` = interesting. `types.json`: `A124 → ["ANTONOV An-124 Ruslan","L4J","H"]` (species/engines/engine type, wake). `operators.json`: `AAC → ["Army Air Corps","United Kingdom","ARMYAIR"]` | ✓ **use** (ODC-By) |
| VRS standing-data `aircraft/` | CC0 | 1.6 MB, small subset | daily | reg, type, manufacturer/model, private flag, operator, airline, serial, year | ✓ use as an overlay |
| **FAA Releasable Aircraft Database** registry.faa.gov/database/ReleasableAircraft.zip | US federal work, public domain (17 U.S.C. §105) | ~60 MB zip (**UNVERIFIED**, faa.gov returned 403 to me) | daily, 23:30 Central (FAA page, via search) | MASTER file includes the Mode S hex (column name **UNVERIFIED**), N-number, make/model, owner name and address | ◐ optional; US only; contains **personal names and addresses**: never show individuals |
| tar1090-db `aircraft.csv.gz` (wiedehopf) | **none stated** | 8.2 MB gz, 622,506 rows | ad hoc | `hex;reg;type;flags;desc;year;ownOp;` | ✗ merges ADSBx `basic-ac-db` (below) and has no licence |
| ADS-B Exchange `basic-ac-db.json.gz` | none stated for the file; site terms are JetNet's; "Commercial … need written authorization"; "No bulk resale or redistribution" (via search) | — | daily | — | ✗ avoid |
| OpenSky aircraft database | "solely for the purpose of non-profit research and non-profit education" | — | — | — | ✗ avoid |
| UK CAA G-INFO | CAA: "copyright and Database right … vested in the CAA"; "authorised for use on a single PC only" (via search) | — | — | — | ✗ avoid |
| adsbdb.com `/v0/aircraft/<hex>` | code MIT; aircraft data "PlaneBase" | API | — | type, reg, owner, country, photo URL | ✗ avoid |

### 2.2 Operator ICAO designator → name

- **Mictronics `operators.json`** (ODC-By). The readsb README says it is based on FAA JO 7340.2.
- **VRS `airlines.csv`** (CC0). Includes IATA codes and charter/positioning callsign patterns.
- **FAA JO 7340.2 *Contractions*, Chapter 3** (faa.gov ATPUBS, US federal work). It reproduces ICAO Doc 8585
  designators, and monthly notices (e.g. N JO 7340.927, Aug 2026) add new foreign designators. A public-domain
  cross-check.
- ICAO **Doc 8585** itself is sold by ICAO. I did not open the store page, so this rests on the brief.
- OpenFlights `airlines.dat`: "made available under the Open Database License". The airline list is also a stale
  snapshot ("As of January 2012 …"; "Active" field "is *not* reliable"). ✗ Not needed.
- Wikipedia lists (CC BY-SA): ✗ not needed.

### 2.3 Route by callsign

| source | licence / terms | notes | verdict |
|---|---|---|---|
| **VRS standing-data `routes/` + `airports/`** | CC0 | 621k callsigns; daily; local lookup | ✓ **use** |
| adsb.lol `/api/0/routeset`, `/api/0/route/<cs>` | code BSD-3-Clause; the endpoint descriptions say "Data by https://github.com/vradarserver/standing-data/"; it redirects to `vrs-standing-data.adsb.lol`; adds a "plausible" flag (within max(50 NM, 20 % of route length) of the great circle); "Rate limits are dynamic"; API keys "in the future" for feeders | It is VRS data plus a check, so we don't need the API. Data licence of adsb.lol itself **UNVERIFIED** | ◐ not needed |
| adsbdb.com `/v0/callsign/<cs>` | route data "may not be copied, published, or incorporated into other databases without the explicit permission of David J Taylor"; rate limiter in source: 512 / 1,024 per window, 1 / 5 min blocks | — | ✗ avoid |
| OpenSky flights API | operational use needs a written licence | — | ✗ avoid |

### 2.4 Country from the ICAO address block

- **Primary:** ICAO Annex 10 Vol III, Part I, Chapter 9, Appendix, **Table 9-1**. It allocates blocks by fixed
  leading bits ("4, 6, 9, 12 or 14 bits" giving blocks of 1,048,576 … 1,024 addresses). For example, United
  Kingdom `010000------------------` (= 0x400000–0x43FFFF), United States `1010--------------------`
  (0xA00000–0xAFFFFF), Germany `001111…`. Allocations of states are facts. We write our own table from the
  standard (one copy opened: peter2000.co.uk). It is a few hundred rows of `prefix bits → ISO2`.
- **Cross-check 1:** VRS `code-blocks.csv` (CC0). It agrees on the GB example: `400000,43FFFF,…,GB`. It also adds
  **military sub-blocks**, e.g. `43C000–43FFFF GB` and `400000–40003F GB`, flagged `IsMilitary=1`. Military
  sub-blocks are **not** in Annex 10. They are community knowledge, and VRS's CC0 table is the cleanest source of
  them.
- **Cross-check 2:** SKYbrary's "24-bit Aircraft Address" article and ICAO regional guidance papers (APAC 2020
  IP04; WACAF 2023 guidelines).
- ★ Note: a 2024 CAAB (Bangladesh) document shows an amended table with 11-bit and 13-bit blocks. **Use the
  current Annex amendment**, and run a test that every VRS block maps to the same country in ours.
- dump1090/readsb carry similar tables in GPL code. We **do not read them for this**. They would only be a
  third cross-check, and we take nothing from them.

### 2.5 Photos

- **Planespotters.net public photo API.** The docs page sits behind Cloudflare, so I read it only through
  search-engine extracts and two GitHub projects that paraphrase it (**read the page and keep a PDF**). The
  conditions they report:
  - "free to use and does not require an access key"; one photo per query, by registration or hex. The endpoints
    are widely used as `https://api.planespotters.net/pub/photos/hex/<hex>` and `…/reg/<reg>` (**UNVERIFIED**).
  - "credit the photographer in text visible beside the image", and the thumbnail must link to the photo's
    Planespotters page using the returned URL, as a plain link "without rel="nofollow"".
  - Photos cannot be "a paid, premium, or member-only feature".
  - Browser clients send a valid Origin/Referer; access "can be throttled or revoked".
  - The image is fetched "from the address the API returns, unchanged, by the screen that displays it", is "never
    … proxied or passed on", and JSON answers are "cached for 24 hours at most".
  - The terms forbid "re-exposing the API or its data through your own API".
  ⇒ **The client calls Planespotters itself. The server never touches photos.**
- airport-data.com `ac_thumb.json`: there are no terms that I could find. ◐ Link-out only, or skip.
- Airliners.net / JetPhotos: ◐ search link-out only.

### 2.6 Country flags

- Emoji regional-indicator pairs from ISO2 need no assets at all. However, Windows browsers show letters, not
  flags.
- **`lipis/flag-icons`** (MIT): SVG per ISO2. Bundling it means shipping the MIT notice. This is the right answer
  for the web client and the apps.

---

## 3. ODbL, ODC-By and CC BY-SA, in our situations

### 3.1 The definitions that decide it (ODbL 1.0, quoted; ODC-By 1.0 uses the same definitions)

- "**Convey** … means Using the Database … in any way that enables a Person to make or receive copies of the
  Database or a Derivative Database. Conveying does not include interaction with a user through a computer
  network, or creating and Using a Produced Work, where no transfer of a copy … occurs."
- "**Produced Work** – a work (such as an image, audiovisual material, text, or sounds) resulting from using the
  whole or a Substantial part of the Contents (via a search or other query) …"
- "**Publicly** – means to Persons other than You or under Your control …"
- "**Derivative Database** … includes … Extracting or Re-utilising the whole or a Substantial part of the Contents
  in a new Database."

### 3.2 ODbL (OpenFlights): why we avoid it

1. **Bundling it in the server package** is Publicly Conveying it. §4.2 then requires a copy of the licence and the
   notices to go with it.
2. **Merging it** with other data in the server (an enriched in-memory or on-disk table) makes a Derivative
   Database.
3. Showing lookups to listeners makes **Produced Works**, which are Publicly Used. §4.4c: "A Derivative Database
   is Publicly Used and so must comply with Section 4.4. if a Produced Work created from the Derivative Database
   is Publicly Used".
4. §4.6 then requires us to "offer to recipients … a copy in a machine readable form of: a. The entire Derivative
   Database; or b. A file containing all of the alterations".
5. So every server that shows a merged airline name to a listener would owe that listener the merged database.
6. §4.5c exempts only "Use of a Derivative Database internally within an organisation". A shared public server is
   not that.
7. A **run-time download by the owner's server** moves the obligations from us to the **owner**, but it does not
   remove them. Each owner becomes the person who Publicly Uses the derivative.
8. That is a burden we would be pushing onto hobbyists for data we can get under CC0 anyway. **Verdict: no ODbL
   data in the merged tables.**

### 3.3 ODC-By (Mictronics): what we owe

- **If we bundle or mirror the files** (Publicly Convey), §4.2 applies: "Include a copy of this License or its
  [URI] with the Database … and in any relevant documentation" and "Keep intact any copyright or Database Right
  notices". In practice: `LICENSE` next to the data file, and a line in the package's copyright file.
- **Showing it to listeners** (Publicly Use a Produced Work), §4.3 applies: "include a notice associated with the
  Produced Work reasonably calculated to make any Person that … is otherwise exposed to the Produced Work aware
  that Content was obtained from the Database … and that it is available under this License". The suggested
  wording: "Contains information from Mictronics aircraft database which is made available under the ODC
  Attribution License" (both linked). ⇒ Put this in the map's ⓘ credits, beside the map tiles' attribution, and in
  About.
- **No share-alike** applies. Merging Mictronics with CC0 data is fine.
- A run-time download by the owner's server: we do not Convey anything. The owner's page still shows Produced
  Works, so our UI carries the §4.3 notice for them automatically.

### 3.4 CC0, public domain and privacy

- CC0 (VRS) and US federal works (FAA) carry no conditions. Credit them anyway: it is polite, and VRS's CREDITS
  files name the contributors.
- **Privacy is a separate question from licence.** Owner names from FAA data or ADSBx `ownOp` (e.g.
  "GONZALEZ OSCAR" in tar1090-db) identify private individuals. The FAA allows owners to withhold PII
  (49 U.S.C. §44114). readsb exposes `dbFlags` bits `PIA = 4` and `LADD = 8`. In the UK and EU, publishing a named
  private owner beside a live position is personal data.
- ⇒ **Rule: we never show an individual's name. We show "Private".** Operators are shown only when they are
  businesses (VRS `IsPrivateOperator = 0`) or an ICAO designator. Aircraft in LADD/PIA are hidden if we ever have
  that flag.

### 3.5 CC BY-SA (Wikipedia tables)

- CC BY-SA 4.0 licenses sui generis database rights too.
- Extracting a substantial part into our table is Adapted Material. That brings share-alike, and BY-SA terms
  inside an App Store binary.
- Not needed. ✗

### 3.6 The App Store question

- None of CC0, ODC-By or MIT imposes terms that conflict with App Store distribution. (GPL does, which is why we
  avoid GPL code; ODbL share-alike would be awkward.)
- The *data* lives on the **server** (VibeServer), not in the app. The app shows the server's fields, plus flag SVGs
  (MIT) and Planespotters photos fetched live.

---

## 4. Recommended design

### 4.1 What the server does

- **On first hearing a hex**, enrich it once and cache the result on the aircraft record (not on every report):
  1. `country` (ISO2) and `mil` from our block table. Always available, needs no data files.
  2. `r`, `t`, and the military flag from the aircraft database. Order: the owner's `BaseStation.sqb`, if
     configured, then the VRS `aircraft/` overlay, then Mictronics.
  3. `desc`, `wtc`, `eng`, `species` from `t` via the type tables.
  4. `op` (ICAO designator) and `opName` from the callsign's code (`BAW123 → BAW`). Use it only when it is a real
     designator (3 letters followed by a digit) **and** not a registration-as-callsign.
- **On a callsign**, look up the route **locally** in VRS `routes/`:
  1. Normalise the callsign with the documented rules.
  2. Load the file for that code on demand (`B/BAW-1.csv`) into a small LRU of codes, about 200 files. Never load
     all 19 MB, given the Pi 2 and 1 GB phones.
  3. Resolve the airports and run our own plausibility check against the aircraft's position.
  4. If the check fails, send `route` with `plausible:false`. The client then shows it struck through, or hides it.
- **Memory**, for the 1 GB / 32-bit floor:
  - Convert the Mictronics JSON once, at download time, into a compact **sorted binary file**: 3-byte hex,
    registration and type in a string table, and flags. That is about 9–10 MB on disk.
  - **mmap** it and binary-search it. Resident memory then stays at the pages touched.
  - Do not parse the 14.7 MB JSON into a heap map.
  - Type and operator tables are under 0.5 MB, so they can sit in memory.
- **Logos and silhouettes:** two optional owner folders (settings: "Operator flags folder", "Silhouettes folder").
  - Served at `/adsb/img/flag/<candidates>` and `/adsb/img/type/<candidates>`, using VRS's order: hex, then
    registration, then `op-t` (the community `RYR-B738` form), then `op`; and for silhouettes hex, then `t`.
  - Accept `.bmp` and `.png` (VRS packs are BMP; browsers, iOS and Android all decode BMP; **check on device**).
    Pass the image through unscaled and let the client fit it to 85×20.
  - Return 404 when absent, never a blank image, so the client can fall back to text.
  - Advertise `"adsbImages": {"flag": true, "type": false}` in `vibeserver.json`, so a client doesn't ask for
    nothing.
- **Fetching data (polite by design):**
  - One weekly job, at a random minute so servers don't arrive together.
  - It makes conditional GETs (`If-None-Match`/`If-Modified-Since`) for:
    - Mictronics `indexedDB.zip` (3.35 MB)
    - VRS `code-blocks.csv`, `airlines.csv` and `model-type/*`
    - VRS `routes/` and `airports/`, as a repository tarball, about 4–5 MB compressed (**UNVERIFIED** estimate).
      Check it first against the 2 kB VRS `FlightNumberCoverage.csv`, whose MD5 columns say whether anything
      changed.
  - User-Agent `VibeServer/<ver> (+https://vibesdr.net)`.
  - Failures back off. The job never runs on a server that is set to "no internet" (Pocket/hotspot).
  - **Alternative:** mirror the files on vibesdr.net weekly, so thousands of servers fetch from us, not GitHub.
    We then Convey ODC-By data ourselves (licence file alongside, which is easy).
- **Bundled seed** (zero network): the block table (ours), and the type and operator tables. Then a fresh server
  shows country, military, operator name and type description from the first minute.
- **Settings:** "Look up aircraft details" (on), "Look up routes" (on), "Download weekly updates" (on; off on
  Lite builds by default if RAM-tight), and the two image folders.

### 4.2 Bundle or download? (decision)

| option | freshness | package size | politeness | licence work |
|---|---|---|---|---|
| bundle everything in .deb/app | stale between releases (fleets change weekly) | +~8 MB | best (no fetching) | §4.2 notice in the package |
| owner's server downloads weekly from GitHub | ≤ 1 week | seed only | fine (small, conditional, jittered) | §4.3 notice in UI only |
| we mirror weekly on vibesdr.net | ≤ 1 week | seed only | best for upstreams | §4.2 notice on the mirror + §4.3 in UI |

Recommendation: **seed + weekly download, mirrored by us once we have many servers.**

### 4.3 Report fields (added to each aircraft; absent = unknown)

We use readsb's names where readsb already has the field (it puts its database fields into `aircraft.json` as
`r`, `t`, `desc`, `dbFlags`), so the "dump1090/readsb field names" rule in `ADSB.md` still holds.

| field | example | source | note |
|---|---|---|---|
| `r` | `"G-STBC"` | aircraft DB | readsb name |
| `t` | `"B77W"` | aircraft DB | readsb name; ICAO 8643 designator |
| `desc` | `"BOEING 777-300ER"` | type table | readsb name |
| `wtc` | `"H"` | type table | L / M / H / J (A380 "J" if present) |
| `eng` | `"2J"` | type table | count + E/J/P/R/T |
| `species` | `"L"` | type table | L/H/S/A/G/T (helicopter even without category A7) |
| `country` | `"GB"` | block table | ISO 3166 alpha-2, from the **address** (state of registry), not the operator |
| `mil` | `1` | block table OR DB flag | omit when 0 |
| `op` | `"BAW"` | callsign / DB | ICAO 3-letter operator |
| `opName` | `"British Airways"` | operator table | never a private individual's name; `"Private"` |
| `route` | `{"from":"EGLL","to":"KJFK","via":[],"plausible":true}` | VRS routes | client looks up names/cities from a server endpoint `/adsb/airport/<icao>` (cached) or the report includes `fromName`/`toName` |

- Static fields are sent **once per hex** (the first report that carries them, and again to a socket that joins),
  not every second. That keeps the 1 fps report small. The client keeps them per hex until the aircraft drops.

### 4.4 What the client shows

- **List row:**
  - flag (`country`), then `flight` or `r`
  - `t`, then `op` or the operator logo if the server has one, else the `op` text
  - FL/ft, kt, NM + bearing
  - a small MIL badge
- **Card:**
  - operator logo (85×20) or name
  - silhouette (85×20) or our category icon
  - registration · type · `desc`
  - operator name and country name (from ISO2)
  - Civil/Military
  - wake category and engines
  - route `EGLL → KJFK`, with names, struck through when not plausible
  - photo with "© photographer", linking to Planespotters
  - the credits line (Mictronics ODC-By, VRS standing data CC0)
- **Photos** are fetched by the client only when the card opens: one request per hex, the answer cached for at
  most 24 h, never prefetched for the whole list. Off on Apple TV (no WebView), and off when the listener turns
  photos off.
- **No VRS-only controls:** a field that a radio or server cannot supply is simply absent. Rule from AGENTS.md:
  never draw an inert control.

### 4.5 Phases

1. **Phase 1, zero licensing doubt:**
   - `country` + flag (our table, flag-icons MIT)
   - `mil` (block table)
   - `op`/`opName` from the callsign (VRS airlines CC0 + Mictronics operators ODC-By, bundled with notice)
   - `desc`/`wtc`/`eng`/`species` from `t` once `t` exists
   - route from VRS (CC0), local, with the plausibility check
   - the credits line
2. **Phase 2:** the per-aircraft database (Mictronics ODC-By) as an mmap'd binary, the weekly conditional
   download, the settings, and the optional owner `BaseStation.sqb`.
3. **Phase 3:** owner image folders (VRS-compatible names; we ship none) and Planespotters photos in the card
   (client-side, credited). Only after Stuart has read and saved the Planespotters terms page.
4. **Later / ask first:** the vibesdr.net mirror; asking Andrew Whewell whether using SDM's lookup or attributing
   VRS in a particular way is welcome; our own silhouettes drawn per type family (our artwork; no trademarks).

### 4.6 Open questions

- Planespotters' terms, read at first hand (Cloudflare blocked me), and whether a native app needs a particular
  Origin or User-Agent.
- The FAA MASTER column names, and the size of the zip (faa.gov gave 403).
- The size of the VRS repository tarball compressed (the estimate is 4–5 MB).
- The adsb.lol data licence (not needed if we use VRS directly).
- Whether ICAO's copyright in Docs 8643/8585 has ever been asserted against designator tables. I know of no case.

---

## Sources (opened)

- VRS standing data: https://github.com/vradarserver/standing-data, plus its `README.md`, `LICENSE` and the
  `schema-01/README.md` and `CREDITS.md` in `aircraft`, `airlines`, `airports`, `routes`, `code-blocks`,
  `model-type`, `countries` and `registration-prefixes` (raw.githubusercontent.com); `code-blocks.csv`,
  `airlines.csv`, `model-type/{A,B}.csv`, `airports/E/EG.csv`, `routes/B/BAW-all.csv`, `reg-prefixes.csv`,
  `countries.csv`; the GitHub API tree and commits for the repository.
- VRS source (BSD-3-Clause): https://github.com/vradarserver/vrs (`LICENSE.md`,
  `VirtualRadar.Library/AircraftOnlineLookup.cs`, `VirtualRadar.Library/AircraftOnlineLookupProvider.cs`,
  `VirtualRadar.Database/StandingData/StandingDataUpdater.cs`, `Plugin.AirportDataThumbnails/AirportDataDotCom.cs`,
  `VirtualRadar.WebSite/Middleware/ImageServer.cs`, `VirtualRadar.WebSite/Site/Web/script/vrs/format.ts`).
- http://sdm.virtualradarserver.co.uk/Aircraft/GetAircraftLookupSettings?language=en (one request);
  http://www.virtualradarserver.co.uk/Files/StandingData.sqb.gz (HEAD only);
  http://www.virtualradarserver.co.uk/Files/FlightNumberCoverage.csv; https://www.virtualradarserver.co.uk/FlightRoutes.aspx
- Mictronics: https://github.com/Mictronics/aircraft-database (`README.md`, `LICENSE` ODC-By, `indexedDB.zip`
  downloaded and inspected); https://github.com/Mictronics/readsb-protobuf/tree/dev/webapp/src/db
  (`operators.json`, `types.json`). https://www.mictronics.de/aircraft-database/ returned 404.
- tar1090-db: https://github.com/wiedehopf/tar1090-db (`README.md`, `update.sh`, `csv` branch `csv.sh`,
  `aircraft.csv.gz` downloaded and inspected).
- readsb JSON fields: https://github.com/wiedehopf/readsb/blob/dev/README-json.md
- adsbdb: https://github.com/mrjackwills/adsbdb (`README.md`, `src/db/redis/ratelimit.rs`); https://www.adsbdb.com/
- adsb.lol: https://github.com/adsblol/api (`README.md`, `src/adsb_api/utils/api_routes.py`, `plausible.py`)
- OpenSky terms: https://opensky-network.org/about/terms-of-use
- ADS-B Exchange: https://www.adsbexchange.com/data-products/sample-data/ (plus search summaries of
  https://adsbexchange.com/?p=19798)
- OpenFlights: https://openflights.org/data.php · OurAirports (public domain): https://ourairports.com/data/
- ODbL 1.0: https://opendatacommons.org/licenses/odbl/1-0/ · ODC-By 1.0: the Mictronics `LICENSE` above
- ICAO Annex 10 Vol III Ch. 9 (copy): https://www.peter2000.co.uk/aviation/misc/ICAO_Annex_10_Volume_III.pdf
- Operator flags pack: https://github.com/rikgale/VRSOperatorFlags (`README.md`, licence field)
- Flags: https://github.com/lipis/flag-icons (MIT, licence field)
- ZenithBoard's paraphrase of the Planespotters terms: https://github.com/regis57/ZenithBoard/pull/110

Seen only through search-engine summaries (**not opened by me; verify**): https://www.planespotters.net/photo/api,
https://www.planespotters.net/legal/termsofuse, the FAA releasable-aircraft page
https://www.faa.gov/licenses_certificates/aircraft_certification/aircraft_registry/releasable_aircraft_download/,
https://www.caa.co.uk/aircraft-register/g-info/g-info-download/, https://airport-data.com/api/doc,
https://skybrary.aero/articles/24-bit-aircraft-address,
https://www.faa.gov/air_traffic/publications/atpubs/cnt_html/chap_3.html, and the ICAO APAC/WACAF address-allocation
papers.
