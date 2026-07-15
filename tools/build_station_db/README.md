# build_station_db — FCC LMS → `assets/db/stations.sqlite`

Build-time ETL for the offline "stations near me" database (addendum §3). This is
a **human-run build step**, not on-device code. It downloads the FCC LMS public
files, flattens the FM facilities into one denormalized row each, and emits the
SQLite that ships in the APK.

> The FCC explicitly warns the LMS structure **is not simple** and that staff
> can't help with the tables. Budget real time for the join. This README is where
> we record what we found so the next refresh isn't a re-discovery.

## Run

The script is self-contained — you can run it from anywhere (copy it out of the
repo if you like). Outputs default to `stations.sqlite` **in the current
directory**; pass `--out` to place it elsewhere.

```bash
python3 build_station_db.py fetch      # download + unzip LMS files -> ./lms_files
python3 build_station_db.py inspect    # print each .dat's columns + a sample row
python3 build_station_db.py build      # emit ./stations.sqlite
python3 build_station_db.py self-test  # prove the pipeline with synthetic data
python3 build_station_db.py sample     # emit a small SYNTHETIC db for emulator/UI dev

# to drop it straight where the app bundles it, run from that folder or use --out:
python3 build_station_db.py build --out ../../assets/db/stations.sqlite
```

The app loads the bundled DB from `assets/db/stations.sqlite`, so copy the built
file there (the build prints this reminder).

`sample` writes ~12 obviously-fake stations (callsigns `KAMP`, `KBMP`, …, city
"Sample City", snapshot `SAMPLE`) around a center (`--lat/--lon`, default San
Francisco) so you can build the "Nearby" UI and exercise the query/ranking in the
emulator **before** wrangling the real LMS download. Replace it with a real
`build` (and bump `DB_ASSET_VERSION`) before shipping.

Then **bump `DB_ASSET_VERSION` in `src/services/stationDb.ts`** so installed apps
re-copy the new DB, and rebuild the app.

Only `python3` (stdlib) is required — no pip installs.

## Data source

- **FCC LMS Public Database Files** — authoritative, US, public domain, updated
  each business day. Frequency, callsign, service, class, ERP, TX lat/long.
- **Do NOT use CDBS** — frozen since 2023-10-01. Lots of stale tooling still
  points at it.
- Format/genre and logos are NOT in the FCC data — those come at runtime from
  Radio-Browser (`src/services/radioBrowser.ts`), never bundled.

If the download URL 404s, get the current one from the LMS Public Database page:
<https://enterpriseefiling.fcc.gov/dataentry/public/tv/lmsDatabase.html>

## The join (VERIFY against a real download)

The FM facility identity, engineering, and location live in separate LMS tables.
The mapping is isolated in the **CONFIG block** at the top of
`build_station_db.py`; each field lists candidate column names and the first one
present wins. **Confirm these against `inspect` output** and update this table
when you do:

| Output column   | LMS file (assumed)         | LMS column (assumed)              | Verified? |
|-----------------|----------------------------|-----------------------------------|-----------|
| `facility_id`   | `facility.dat`             | `facility_id`                     | ⬜ TODO   |
| `callsign`      | `facility.dat`             | `fac_callsign`                    | ⬜ TODO   |
| `service`       | `facility.dat`             | `fac_service` (FM/FX/FL)          | ⬜ TODO   |
| `city`/`state`  | `facility.dat`             | `comm_city` / `comm_state`        | ⬜ TODO   |
| `frequency_mhz` | `app_antenna_frequency.dat`| `station_freq` (or channel 200-300)| ⬜ TODO  |
| `erp_kw`        | `app_antenna_frequency.dat`| `station_erp`                     | ⬜ TODO   |
| `station_class` | `app_antenna_frequency.dat`| `station_class`                   | ⬜ TODO   |
| `lat`/`lon`     | `app_location.dat`         | decimal, or DMS + hemisphere      | ⬜ TODO   |

Notes / gotchas found so far:
- **Linkage:** facilities are joined to engineering/location **by `facility_id`**.
  If the flattened files only expose `application_id`, wire that hop in the CONFIG
  block and pick the record from the *latest granted license* (the current code
  takes the first row per facility — refine if duplicates appear).
- **Coordinates** may be decimal (`lat_dec`/`lon_dec`) or DMS
  (`lat_deg/min/sec` + `lat_dir`). Both are handled in `coords()`.
- **Frequency** may be MHz directly or an FM channel number (200–300 →
  87.9–107.9 MHz); both handled.
- Rows failing validation (non-FM, missing eng/loc, freq out of 87.5–108.1, bad
  coords) are dropped and **counted** in the build report — watch those counts.

## Output

`stations.sqlite` — table `stations` (schema in the addendum §4) + a `meta` table
holding `lms_snapshot_date`, `schema_version`, `row_count`, `built_at`. Expect
~20–22k rows, single-digit MB. The build warns if it produces far fewer FM rows
than expected (usually a broken join).

## Station Explorer (sibling toy)

`station_explorer.py` in this folder is a tiny stdlib **Tkinter** GUI to browse a
`stations.sqlite` with the *same* logic as the app (bounding box → haversine →
receivability score, plus RDS PI ⇄ callsign decode, self-checked against
`stationGeo.ts` / `piCallsign.ts`). Handy for sanity-checking a build without an
emulator.

```bash
# GUI needs Tk: `sudo apt install python3-tk` (Debian/Mint/Ubuntu)
python3 station_explorer.py --db stations.sqlite
# headless, no Tk:
python3 station_explorer.py --cli --db stations.sqlite --lat 37.77 --lon -122.42
python3 station_explorer.py --db stations.sqlite --pi 0x54C4     # decode a PI
```

With no `--db`, it looks for `stations.sqlite` in the current directory, then
next to the script itself, then the repo's `assets/db/stations.sqlite` — first
one found wins. So `build`/`sample` then `station_explorer.py` in the same folder
just works, and running it from inside the repo finds the bundled DB.

## Three-letter callsign table (separate task)

`src/services/piCallsign.ts` needs the NRSC-4-B Annex D three-letter → PI table
(e.g. KOB, WHO, WGN). It's intentionally empty there. Populate `THREE_LETTER_PI`
from Annex D during a DB refresh; it's a small, static list.
