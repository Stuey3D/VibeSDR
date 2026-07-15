# Station Explorer

A tiny stdlib **Tkinter** GUI to browse the bundled station DB using the *same*
logic as the app — bounding box → haversine → receivability score, plus RDS
PI ⇄ callsign decode. The geo/ranking and PI functions are ported from and kept
in step with `src/services/stationGeo.ts` and `src/services/piCallsign.ts`, so
what you see here matches the phone.

Handy for sanity-checking a freshly built `stations.sqlite`, eyeballing how the
ranking behaves, and confirming PI decodes — without an emulator.

## Run

```bash
# GUI (needs Tk: `sudo apt install python3-tk` on Debian/Mint/Ubuntu)
python3 station_explorer.py                     # opens assets/db/stations.sqlite

# Headless / scripting — no Tk needed
python3 station_explorer.py --cli --lat 37.77 --lon -122.42 --radius 100
python3 station_explorer.py --pi 0x54C4         # decode one PI and exit
```

Point it at a `sample` DB to try it before the real FCC build:

```bash
python3 ../build_station_db/build_station_db.py sample --out /tmp/s.sqlite
python3 station_explorer.py --cli --db /tmp/s.sqlite --lat 37.77 --lon -122.42
```

The GUI has: a DB picker, lat/lon/radius inputs with a ranked results table
(freq, callsign, service, class, ERP, distance, score, city), and a PI-decode box.
It's a dev toy, not shipped in the app.
