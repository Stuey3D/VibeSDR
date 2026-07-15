#!/usr/bin/env python3
"""
Build-time ETL: FCC LMS public database files -> assets/db/stations.sqlite
(addendum §3). Run occasionally by a human; NEVER on-device.

    python3 build_station_db.py fetch                 # download + unzip LMS files
    python3 build_station_db.py inspect               # dump table/columns/samples
    python3 build_station_db.py build                 # emit stations.sqlite
    python3 build_station_db.py self-test             # prove the pipeline (synthetic)

The FCC warns the LMS structure is not simple and staff can't help with the
tables. So the facility -> engineering join below is ISOLATED and marked VERIFY:
use `inspect` against a real download to confirm the file/column names, fix the
CONFIG block, and record what you found in README.md so the next refresh isn't a
re-discovery.

Scale check: ~11k full-power FM + ~9k translators + ~2k LPFM ~= 20-22k small
rows -> single-digit MB SQLite. Do not turn this into a server.
"""
from __future__ import annotations

import argparse
import csv
import io
import math
import os
import sqlite3
import sys
import zipfile
from datetime import date, datetime
from urllib.request import urlopen, Request

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_LMS_DIR = os.path.join(HERE, "lms_files")
DEFAULT_OUT = os.path.normpath(os.path.join(HERE, "..", "..", "assets", "db", "stations.sqlite"))

# LMS public database files (zip of pipe-delimited tables), updated each business
# day. Confirm the current URL on the LMS Public Database page if this 404s:
#   https://enterpriseefiling.fcc.gov/dataentry/public/tv/lmsDatabase.html
LMS_ZIP_URL = "https://enterpriseefiling.fcc.gov/dataentry/public/tv/lms_db_download/lms_public_database.zip"

# FCC service codes we keep (addendum §3.3). AM is out of scope.
FM_SERVICES = {"FM", "FX", "FL"}  # full-power FM, FM translator, LPFM

# ─────────────────────────────────────────────────────────────────────────────
# CONFIG — the join. VERIFY every name here against `inspect` output on a real
# download; the LMS column names are the thing most likely to drift. Each field
# lists candidate column names (case-insensitive); the first present one wins.
# ─────────────────────────────────────────────────────────────────────────────
FACILITY_FILE = "facility.dat"
FACILITY_COLS = {
    "facility_id": ["facility_id", "fac_facility_id"],
    "callsign":    ["fac_callsign", "callsign"],
    "service":     ["fac_service", "service", "service_code"],
    "status":      ["fac_status", "status"],
    "city":        ["comm_city", "community_served_city", "city"],
    "state":       ["comm_state", "community_served_state", "state"],
}
# Engineering (frequency / ERP / class) and location (lat/lon). In LMS these live
# in app_* tables linked facility -> application -> antenna/location. Community
# tooling and the FCC's own guidance differ on the exact hops, so this is the
# part to VERIFY first. We link by facility_id where the flattened files expose
# it; otherwise wire application_id here after inspecting.
ENG_FILE = "app_antenna_frequency.dat"
ENG_COLS = {
    "facility_id":   ["facility_id"],
    "frequency_mhz": ["station_freq", "freq_mhz", "frequency", "frequency_mhz"],
    "channel":       ["station_channel", "channel"],   # fallback: FM ch 200..300
    "erp_kw":        ["station_erp", "erp_kw", "hrz_erp_kw", "erp"],
    "station_class": ["station_class", "fac_class", "class"],
}
LOC_FILE = "app_location.dat"
LOC_COLS = {
    "facility_id": ["facility_id"],
    # Either decimal degrees, or DMS + hemisphere. Both handled (see coords()).
    "lat_dec":  ["lat_dec", "latitude", "lat"],
    "lon_dec":  ["lon_dec", "longitude", "lon", "long"],
    "lat_deg":  ["lat_deg", "lat_degrees"],
    "lat_min":  ["lat_min", "lat_minutes"],
    "lat_sec":  ["lat_sec", "lat_seconds"],
    "lat_dir":  ["lat_dir", "lat_direction", "nsflag"],
    "lon_deg":  ["lon_deg", "lon_degrees"],
    "lon_min":  ["lon_min", "lon_minutes"],
    "lon_sec":  ["lon_sec", "lon_seconds"],
    "lon_dir":  ["lon_dir", "lon_direction", "ewflag"],
}

SCHEMA = """
CREATE TABLE stations (
  callsign      TEXT NOT NULL,
  callsign_base TEXT NOT NULL,
  frequency_mhz REAL NOT NULL,
  service       TEXT NOT NULL,
  station_class TEXT,
  erp_kw        REAL,
  lat           REAL NOT NULL,
  lon           REAL NOT NULL,
  city          TEXT,
  state         TEXT,
  facility_id   INTEGER,
  PRIMARY KEY (facility_id)
);
CREATE INDEX idx_callsign_base ON stations(callsign_base);
CREATE INDEX idx_lat ON stations(lat);
CREATE INDEX idx_lon ON stations(lon);
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);
"""


# ── helpers ──────────────────────────────────────────────────────────────────
def resolve(colmap: dict, header_lower: dict) -> dict:
    """Map our field -> actual header name using the candidate lists."""
    out = {}
    for field, candidates in colmap.items():
        for c in candidates:
            if c.lower() in header_lower:
                out[field] = header_lower[c.lower()]
                break
    return out


def load_dat(path: str) -> tuple[list[str], list[dict]]:
    """Load a pipe-delimited LMS .dat (assumes a header row — VERIFY)."""
    with open(path, "r", encoding="latin-1", newline="") as f:
        reader = csv.reader(f, delimiter="|")
        rows = list(reader)
    if not rows:
        return [], []
    header = [h.strip() for h in rows[0]]
    out = [dict(zip(header, [c.strip() for c in r])) for r in rows[1:] if len(r) >= 1]
    return header, out


def callsign_base(cs: str) -> str:
    return cs.upper().split("-")[0].strip()


def channel_to_mhz(ch: float) -> float:
    # FM channels 200..300 -> 87.9..107.9 MHz, 200 kHz spacing.
    return round(87.9 + (ch - 200) * 0.2, 1)


def dms_to_dec(deg, mn, sec, direction) -> float | None:
    try:
        v = abs(float(deg)) + float(mn) / 60 + float(sec) / 3600
    except (TypeError, ValueError):
        return None
    if str(direction).strip().upper() in ("S", "W"):
        v = -v
    return round(v, 6)


def coords(row: dict, lc: dict) -> tuple[float | None, float | None]:
    """Decimal columns if present, else DMS + hemisphere."""
    if "lat_dec" in lc and "lon_dec" in lc:
        try:
            return round(float(row[lc["lat_dec"]]), 6), round(float(row[lc["lon_dec"]]), 6)
        except (KeyError, ValueError):
            pass
    lat = dms_to_dec(row.get(lc.get("lat_deg", "")), row.get(lc.get("lat_min", "")),
                     row.get(lc.get("lat_sec", "")), row.get(lc.get("lat_dir", "")))
    lon = dms_to_dec(row.get(lc.get("lon_deg", "")), row.get(lc.get("lon_min", "")),
                     row.get(lc.get("lon_sec", "")), row.get(lc.get("lon_dir", "")))
    return lat, lon


def index_by_facility(rows: list[dict], cols: dict) -> dict:
    key = cols.get("facility_id")
    out: dict[str, dict] = {}
    if not key:
        return out
    for r in rows:
        fid = r.get(key, "").strip()
        if fid and fid not in out:  # first wins; refine to "latest license" if needed
            out[fid] = r
    return out


# ── commands ─────────────────────────────────────────────────────────────────
def cmd_fetch(args):
    os.makedirs(args.lms_dir, exist_ok=True)
    print(f"Downloading {LMS_ZIP_URL}")
    req = Request(LMS_ZIP_URL, headers={"User-Agent": "VibeSDR-CarFM station DB build"})
    data = urlopen(req, timeout=300).read()
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        z.extractall(args.lms_dir)
    print(f"Extracted {len(os.listdir(args.lms_dir))} files to {args.lms_dir}")


def cmd_inspect(args):
    """Print each .dat's columns + a sample row — the tool for wiring the join."""
    files = sorted(f for f in os.listdir(args.lms_dir) if f.endswith(".dat"))
    if not files:
        sys.exit(f"No .dat files in {args.lms_dir} — run `fetch` first.")
    for fn in files:
        header, rows = load_dat(os.path.join(args.lms_dir, fn))
        print(f"\n=== {fn}  ({len(rows)} rows) ===")
        print("  columns:", ", ".join(header))
        if rows:
            sample = {k: v for k, v in list(rows[0].items())[:12]}
            print("  sample :", sample)


def build(lms_dir: str, out_path: str, snapshot: str) -> dict:
    fac_header, fac_rows = load_dat(os.path.join(lms_dir, FACILITY_FILE))
    eng_header, eng_rows = load_dat(os.path.join(lms_dir, ENG_FILE))
    loc_header, loc_rows = load_dat(os.path.join(lms_dir, LOC_FILE))

    fc = resolve(FACILITY_COLS, {h.lower(): h for h in fac_header})
    ec = resolve(ENG_COLS, {h.lower(): h for h in eng_header})
    lc = resolve(LOC_COLS, {h.lower(): h for h in loc_header})
    for name, need, got in [("facility", ("facility_id", "callsign", "service"), fc),
                            ("engineering", ("facility_id",), ec),
                            ("location", ("facility_id",), lc)]:
        missing = [k for k in need if k not in got]
        if missing:
            sys.exit(f"[{name}] missing required columns {missing}. Run `inspect` "
                     f"and fix the CONFIG block. Resolved: {got}")

    eng = index_by_facility(eng_rows, ec)
    loc = index_by_facility(loc_rows, lc)

    stations, skipped = [], {"non_fm": 0, "no_eng": 0, "no_loc": 0, "bad_freq": 0, "bad_coord": 0}
    for r in fac_rows:
        service = r.get(fc["service"], "").strip().upper()
        if service not in FM_SERVICES:
            skipped["non_fm"] += 1
            continue
        fid = r.get(fc["facility_id"], "").strip()
        e, l = eng.get(fid), loc.get(fid)
        if not e:
            skipped["no_eng"] += 1
            continue
        if not l:
            skipped["no_loc"] += 1
            continue

        # frequency: prefer MHz column, else derive from channel
        freq = None
        if "frequency_mhz" in ec and e.get(ec["frequency_mhz"], "").strip():
            try:
                freq = round(float(e[ec["frequency_mhz"]]), 1)
            except ValueError:
                freq = None
        if freq is None and "channel" in ec and e.get(ec["channel"], "").strip():
            try:
                freq = channel_to_mhz(float(e[ec["channel"]]))
            except ValueError:
                freq = None
        if freq is None or not (87.5 <= freq <= 108.1):
            skipped["bad_freq"] += 1
            continue

        lat, lon = coords(l, lc)
        if lat is None or lon is None or not (-90 <= lat <= 90 and -180 <= lon <= 180):
            skipped["bad_coord"] += 1
            continue

        cs = r.get(fc["callsign"], "").strip().upper()
        def num(v):
            try:
                return float(v)
            except (TypeError, ValueError):
                return None
        stations.append((
            cs, callsign_base(cs), freq, service,
            (e.get(ec.get("station_class", ""), "") or None),
            num(e.get(ec.get("erp_kw", ""), "")),
            lat, lon,
            (r.get(fc.get("city", ""), "") or None),
            (r.get(fc.get("state", ""), "") or None),
            int(fid),
        ))

    if os.path.exists(out_path):
        os.remove(out_path)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    db = sqlite3.connect(out_path)
    db.executescript(SCHEMA)
    db.executemany(
        "INSERT OR REPLACE INTO stations VALUES (?,?,?,?,?,?,?,?,?,?,?)", stations)
    db.executemany("INSERT INTO meta(key,value) VALUES (?,?)", [
        ("lms_snapshot_date", snapshot),
        ("schema_version", "1"),
        ("row_count", str(len(stations))),
        ("built_at", datetime.utcnow().isoformat(timespec="seconds") + "Z"),
    ])
    db.commit()
    integrity = db.execute("PRAGMA integrity_check").fetchone()[0]
    by_service = dict(db.execute(
        "SELECT service, COUNT(*) FROM stations GROUP BY service").fetchall())
    db.close()
    return {"rows": len(stations), "skipped": skipped, "by_service": by_service,
            "integrity": integrity, "out": out_path,
            "size_mb": round(os.path.getsize(out_path) / 1e6, 2)}


def cmd_build(args):
    if not os.path.isdir(args.lms_dir):
        sys.exit(f"No LMS dir {args.lms_dir} — run `fetch` first.")
    snapshot = args.snapshot or date.today().isoformat()
    rep = build(args.lms_dir, args.out, snapshot)
    print(f"\nBuilt {rep['out']}  ({rep['size_mb']} MB, integrity={rep['integrity']})")
    print(f"  rows: {rep['rows']}  by service: {rep['by_service']}")
    print(f"  skipped: {rep['skipped']}")
    if rep["rows"] < 10000:
        print("  WARNING: far fewer FM rows than the ~20k expected — verify the "
              "join (run `inspect`).")
    print("\nRemember: bump DB_ASSET_VERSION in src/services/stationDb.ts so the "
          "app re-copies the new DB.")


def cmd_sample(args):
    """Emit a small SYNTHETIC stations.sqlite for emulator/UI dev before the real
    LMS build. Clearly-fake callsigns + city, snapshot 'SAMPLE', a spread of
    ERP/class/service/distance so ranking and the list UI are exercisable."""
    lat0, lon0 = args.lat, args.lon
    # (bearing°, distance km, freq MHz, service, class, ERP kW)
    specs = [
        (0, 8, 88.5, "FM", "C", 100.0), (45, 15, 90.7, "FM", "C1", 50.0),
        (90, 25, 92.3, "FM", "B", 25.0), (135, 40, 94.1, "FM", "C", 100.0),
        (180, 60, 95.9, "FM", "C", 100.0), (225, 12, 97.3, "FL", None, 0.1),
        (270, 20, 98.7, "FX", None, 0.25), (315, 5, 100.1, "FM", "A", 6.0),
        (30, 70, 101.5, "FM", "C", 98.0), (200, 33, 103.3, "FM", "C2", 15.0),
        (110, 50, 104.9, "FX", None, 0.05), (300, 18, 106.7, "FM", "A", 3.5),
    ][: args.count]
    rows = []
    for i, (brg, dist, freq, svc, cls, erp) in enumerate(specs):
        b = math.radians(brg)
        dlat = (dist * math.cos(b)) / 111.32
        dlon = (dist * math.sin(b)) / (111.32 * max(0.01, math.cos(math.radians(lat0))))
        cs = f"K{chr(65 + (i % 26))}MP"  # KAMP, KBMP, ... — obviously sample data
        rows.append((cs, cs, freq, svc, cls, erp,
                     round(lat0 + dlat, 6), round(lon0 + dlon, 6),
                     "Sample City", "SA", 900000 + i))
    if os.path.exists(args.out):
        os.remove(args.out)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    db = sqlite3.connect(args.out)
    db.executescript(SCHEMA)
    db.executemany("INSERT INTO stations VALUES (?,?,?,?,?,?,?,?,?,?,?)", rows)
    db.executemany("INSERT INTO meta(key,value) VALUES (?,?)", [
        ("lms_snapshot_date", "SAMPLE"), ("schema_version", "1"),
        ("row_count", str(len(rows))),
    ])
    db.commit()
    print(f"Wrote {len(rows)} SAMPLE stations to {args.out} around "
          f"({lat0}, {lon0}). integrity={db.execute('PRAGMA integrity_check').fetchone()[0]}")
    db.close()
    print("This is synthetic dev data — run `build` for the real FCC DB, and bump "
          "DB_ASSET_VERSION in src/services/stationDb.ts.")


def cmd_self_test(args):
    """Prove the transform/filter/emit with synthetic staging files (no FCC)."""
    import tempfile
    d = tempfile.mkdtemp()
    def w(name, header, rows):
        with open(os.path.join(d, name), "w", encoding="latin-1", newline="") as f:
            wr = csv.writer(f, delimiter="|")
            wr.writerow(header)
            wr.writerows(rows)
    w(FACILITY_FILE, ["facility_id", "fac_callsign", "fac_service", "fac_status", "comm_city", "comm_state"], [
        ["1", "KBBB", "FM", "LICEN", "Reno", "NV"],       # keep
        ["2", "K250XY", "FX", "LICEN", "Reno", "NV"],     # keep (translator)
        ["3", "WOLD", "AM", "LICEN", "Dallas", "TX"],     # drop (AM)
        ["4", "KNOLOC", "FM", "LICEN", "Nowhere", "NV"],  # drop (no location)
    ])
    w(ENG_FILE, ["facility_id", "station_freq", "station_erp", "station_class"], [
        ["1", "101.1", "100", "C"],
        ["2", "95.7", "0.25", ""],
        ["3", "1080", "50", "B"],
        ["4", "88.5", "6", "A"],
    ])
    w(LOC_FILE, ["facility_id", "lat_dec", "lon_dec"], [
        ["1", "39.5296", "-119.8138"],
        ["2", "39.50", "-119.80"],
        ["3", "32.7767", "-96.7970"],
        # facility 4 intentionally absent -> exercises no_loc skip
    ])
    out = os.path.join(d, "out.sqlite")
    rep = build(d, out, "2099-01-01")
    ok = True
    def check(name, cond):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + name)
        ok = ok and cond
    check("2 FM/FX rows kept", rep["rows"] == 2)
    check("AM dropped", rep["by_service"].get("AM") is None)
    check("no-location dropped", rep["skipped"]["no_loc"] == 1)
    check("integrity ok", rep["integrity"] == "ok")
    con = sqlite3.connect(out)
    kbbb = con.execute("SELECT callsign_base, frequency_mhz, erp_kw, lat FROM stations WHERE facility_id=1").fetchone()
    check("KBBB row correct", kbbb == ("KBBB", 101.1, 100.0, 39.5296))
    snap = con.execute("SELECT value FROM meta WHERE key='lms_snapshot_date'").fetchone()[0]
    check("meta snapshot set", snap == "2099-01-01")
    con.close()
    print("\nSELF-TEST PASS" if ok else "\nSELF-TEST FAILURES")
    sys.exit(0 if ok else 1)


def main():
    p = argparse.ArgumentParser(description="Build stations.sqlite from FCC LMS files.")
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("fetch", "inspect", "build", "self-test", "sample"):
        sp = sub.add_parser(name)
        sp.add_argument("--lms-dir", default=DEFAULT_LMS_DIR)
        sp.add_argument("--out", default=DEFAULT_OUT)
        sp.add_argument("--snapshot", help="LMS snapshot date YYYY-MM-DD (default: today)")
        if name == "sample":
            sp.add_argument("--lat", type=float, default=37.7749, help="center latitude (default: SF)")
            sp.add_argument("--lon", type=float, default=-122.4194, help="center longitude")
            sp.add_argument("--count", type=int, default=12, help="number of sample stations")
    args = p.parse_args()
    {"fetch": cmd_fetch, "inspect": cmd_inspect, "build": cmd_build,
     "self-test": cmd_self_test, "sample": cmd_sample}[args.cmd](args)


if __name__ == "__main__":
    main()
