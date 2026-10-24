# ADS-B aircraft data — sources, licences, credits

VibeServer shows what an aircraft is (registration, type, operator, country, route) from these sources. Each
server downloads them itself, weekly; the package carries only the CC0 starter. Retrieved for this build: 2026-10-24.

| source | licence | URL | used for |
|---|---|---|---|
| Virtual Radar Server standing data — Andrew Whewell and the VRS users who submit it (commit 77285be18219b753f1552bc1d00c159a9dc8a218) | CC0 1.0 Universal ([text](LICENSE-VRS-standing-data-CC0-1.0.txt)) | https://github.com/vradarserver/standing-data | routes, airports, airlines, aircraft types, address blocks (military, territories), country names; the CC0 starter compiled into the server |
| Mictronics aircraft database (dbversion 527) | Open Data Commons Attribution License 1.0 ([text](LICENSE-Mictronics-ODC-By-1.0.txt), https://opendatacommons.org/licenses/by/1-0/) | https://github.com/Mictronics/aircraft-database | registrations, type codes and descriptions, operators, military flag |
| ICAO Annex 10 Vol. III, Table 9-1 — 24-bit address allocations | facts; our own table (vibe_adsb_blocks.h) | — | country of registry |

**Notice (ODC-By 1.0 §4.3):** Contains information from Mictronics aircraft database, which is made available under
the ODC Attribution License.

Not used, by decision (docs/v12/ADSB-DATA-RESEARCH.md): OpenSky, adsbdb, ADS-B Exchange / tar1090-db, UK CAA G-INFO,
OpenFlights (ODbL), Wikipedia tables (CC BY-SA). Operator logos and silhouettes are never shipped or downloaded —
they are trademarks; an owner may install their own on the admin page. Aircraft photos come from Planespotters.net,
fetched by the listener's own browser or app, credited to the photographer and linked as their terms require.
