#!/usr/bin/env python3
"""
Station Explorer — a tiny desktop GUI to browse the bundled station DB with the
SAME logic the app uses. It's a dev/debug toy: point it at
assets/db/stations.sqlite (or a `sample`/`build` output), give it a location, and
it lists nearby stations ranked exactly as the on-device "Nearby" feature does —
bounding box → haversine → receivability score — plus RDS PI ⇄ callsign decode.

The geo/ranking and PI functions below are hand-ported from and kept in step with
  src/services/stationGeo.ts   and   src/services/piCallsign.ts
so what you see here matches the phone.

Run:
  python3 station_explorer.py                    # GUI (needs a display)
  python3 station_explorer.py --db PATH --lat 37.77 --lon -122.42 --radius 100
  python3 station_explorer.py --pi 0x54C4        # decode a PI
Stdlib only (tkinter + sqlite3). The GUI needs Tk: on Debian/Mint/Ubuntu that's
`sudo apt install python3-tk` (macOS/Windows python bundles it). The --cli/--pi
paths work without Tk.
"""
from __future__ import annotations

import argparse
import math
import os
import sqlite3

# ── logic ported from src/services/stationGeo.ts ─────────────────────────────
EARTH_R_KM = 6371.0088
KM_PER_DEG_LAT = 111.32


def haversine_km(lat1, lon1, lat2, lon2):
    d_lat = math.radians(lat2 - lat1)
    d_lon = math.radians(lon2 - lon1)
    a = (math.sin(d_lat / 2) ** 2 +
         math.cos(math.radians(lat1)) * math.cos(math.radians(lat2)) * math.sin(d_lon / 2) ** 2)
    return 2 * EARTH_R_KM * math.asin(min(1.0, math.sqrt(a)))


def bounding_box(lat, lon, radius_km):
    d_lat = radius_km / KM_PER_DEG_LAT
    cos = max(0.01, math.cos(math.radians(lat)))
    d_lon = radius_km / (KM_PER_DEG_LAT * cos)
    return lat - d_lat, lat + d_lat, lon - d_lon, lon + d_lon


def class_bonus(station_class):
    c = (station_class or "").upper()
    return {"C": 6, "C0": 6, "C1": 4.5, "C2": 3, "B": 3, "C3": 1.5, "B1": 1.5, "A": 0}.get(c, 0)


def receivability_score(erp_kw, station_class, distance_km):
    erp = max(0.0001, erp_kw if erp_kw else 0.05)
    dist = max(1.0, distance_km)
    return 10 * math.log10(erp) - 20 * math.log10(dist) + class_bonus(station_class)


# ── logic ported from src/services/piCallsign.ts (NRSC-4-B) ──────────────────
K_BASE, W_BASE, MAX_SUFFIX = 4096, 21672, 17575


def callsign_to_pi(callsign):
    cs = (callsign or "").upper().split("-")[0].strip()
    if len(cs) != 4 or cs[0] not in "KW" or not cs[1:].isalpha():
        return None
    base = W_BASE if cs[0] == "W" else K_BASE
    v = [ord(ch) - 65 for ch in cs[1:]]
    if any(x < 0 or x > 25 for x in v):
        return None
    return base + v[0] * 676 + v[1] * 26 + v[2]


def pi_to_callsign(pi):
    """Returns (callsign|None, confident, note) — mirrors the TS decoder."""
    if not isinstance(pi, (int, float)) or pi <= 0:
        return None, False, "invalid/zero PI"
    p = int(pi)
    if p == 0xFFFF:
        return None, False, "default/unset PI (0xFFFF)"
    if 0xA000 <= p <= 0xAFFF:
        return None, False, "A-block remapped PI (not inverted)"
    if p >= W_BASE:
        letter, rem = "W", p - W_BASE
    elif p >= K_BASE:
        letter, rem = "K", p - K_BASE
    else:
        return None, False, "below K block (not a US 4-letter PI)"
    if rem > MAX_SUFFIX:
        return None, False, "suffix out of range (likely translator/foreign)"
    cs = letter + chr(65 + rem // 676) + chr(65 + (rem % 676) // 26) + chr(65 + rem % 26)
    return cs, True, "formula"


# ── DB queries (same shape as src/services/stationDb.ts) ─────────────────────
def query_nearby(db_path, lat, lon, radius_km=100, limit=200):
    con = sqlite3.connect(db_path)
    con.row_factory = sqlite3.Row
    min_lat, max_lat, min_lon, max_lon = bounding_box(lat, lon, radius_km)
    rows = con.execute(
        "SELECT * FROM stations WHERE lat BETWEEN ? AND ? AND lon BETWEEN ? AND ?",
        (min_lat, max_lat, min_lon, max_lon),
    ).fetchall()
    con.close()
    out = []
    for r in rows:
        dist = haversine_km(lat, lon, r["lat"], r["lon"])
        if dist > radius_km:
            continue
        out.append({
            "freq": r["frequency_mhz"], "callsign": r["callsign"], "service": r["service"],
            "class": r["station_class"], "erp": r["erp_kw"], "city": r["city"],
            "state": r["state"], "dist": dist,
            "score": receivability_score(r["erp_kw"], r["station_class"], dist),
        })
    out.sort(key=lambda s: s["score"], reverse=True)
    return out[:limit]


def db_meta(db_path):
    try:
        con = sqlite3.connect(db_path)
        meta = dict(con.execute("SELECT key, value FROM meta").fetchall())
        n = con.execute("SELECT COUNT(*) FROM stations").fetchone()[0]
        con.close()
        return meta, n
    except Exception as e:
        return {"error": str(e)}, 0


def identify_pi(db_path, pi):
    cs, confident, note = pi_to_callsign(pi)
    station = None
    if cs:
        con = sqlite3.connect(db_path)
        con.row_factory = sqlite3.Row
        rows = con.execute("SELECT * FROM stations WHERE callsign_base = ?", (cs,)).fetchall()
        con.close()
        rows.sort(key=lambda r: {"FM": 0, "FL": 1, "FX": 2}.get(r["service"], 9))
        station = rows[0] if rows else None
        if station is None:
            confident, note = False, "no DB match for computed callsign"
        elif station["service"] != "FM":
            confident, note = False, f"matched a {station['service']} (formula unreliable for translators)"
    return cs, confident, note, station


# Find a default stations.sqlite by looking, in order: the current directory,
# this script's OWN directory (so a DB sitting next to the script is picked up),
# then the repo's bundled location if we're running from inside the checkout.
# First existing file wins; falls back to ./stations.sqlite (the GUI's Browse
# button and --db always work too).
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def _default_db() -> str:
    candidates = [
        os.path.join(os.getcwd(), "stations.sqlite"),
        os.path.join(_SCRIPT_DIR, "stations.sqlite"),
        os.path.normpath(os.path.join(_SCRIPT_DIR, "..", "..", "assets", "db", "stations.sqlite")),
    ]
    return next((c for c in candidates if os.path.isfile(c)), "stations.sqlite")


DEFAULT_DB = _default_db()


# ── self-check: the Python port must agree with the TS reference values ──────
def _selfcheck():
    assert callsign_to_pi("KBBB") == 4799, "PI port drift"
    assert pi_to_callsign(4799)[0] == "KBBB", "PI reverse drift"
    big = receivability_score(100, "C", 60 * 1.60934)
    tr = receivability_score(0.25, None, 15 * 1.60934)
    assert big > tr, "ranking port drift"


# ── CLI (headless-friendly, used to verify without a display) ────────────────
def run_cli(args):
    _selfcheck()
    meta, n = db_meta(args.db)
    print(f"DB: {args.db}")
    print(f"  meta={meta}  rows={n}")
    if args.pi is not None:
        pi = int(args.pi, 0)
        cs, conf, note, st = identify_pi(args.db, pi)
        where = f" — {st['callsign']} {st['frequency_mhz']} {st['city']},{st['state']}" if st else ""
        print(f"PI 0x{pi:04X} -> {cs or '(none)'}  confident={conf}  [{note}]{where}")
        return
    res = query_nearby(args.db, args.lat, args.lon, args.radius)
    print(f"\n{len(res)} stations within {args.radius} km of ({args.lat}, {args.lon}), best first:\n")
    print(f"  {'FREQ':>6} {'CALL':<8} {'SVC':<3} {'CLS':<3} {'ERPkW':>7} {'DISTkm':>7} {'SCORE':>6}  CITY")
    for s in res[:args.limit]:
        print(f"  {s['freq']:>6} {s['callsign']:<8} {s['service']:<3} {str(s['class'] or ''):<3} "
              f"{(s['erp'] or 0):>7.2f} {s['dist']:>7.1f} {s['score']:>6.1f}  {s['city'] or ''}")


# ── GUI ──────────────────────────────────────────────────────────────────────
def run_gui(args):
    import tkinter as tk
    from tkinter import ttk, filedialog

    root = tk.Tk()
    root.title("VibeSDR CarFM — Station Explorer")
    root.geometry("880x560")
    state = {"db": tk.StringVar(value=args.db)}

    top = ttk.Frame(root, padding=8)
    top.pack(fill="x")
    ttk.Label(top, text="Database:").grid(row=0, column=0, sticky="w")
    db_entry = ttk.Entry(top, textvariable=state["db"], width=70)
    db_entry.grid(row=0, column=1, sticky="we", padx=6)
    top.columnconfigure(1, weight=1)

    status = tk.StringVar()

    def refresh_status():
        meta, n = db_meta(state["db"].get())
        snap = meta.get("lms_snapshot_date", "?")
        status.set(f"{n} stations · data as of {snap}"
                   + ("  ⚠ database not built (0 rows) — run the ETL or `sample`" if n == 0 else ""))

    def browse():
        f = filedialog.askopenfilename(filetypes=[("SQLite", "*.sqlite *.db"), ("All", "*.*")])
        if f:
            state["db"].set(f)
            refresh_status()

    ttk.Button(top, text="Browse…", command=browse).grid(row=0, column=2)

    # query row
    q = ttk.Frame(root, padding=(8, 0))
    q.pack(fill="x")
    lat = tk.StringVar(value=str(args.lat))
    lon = tk.StringVar(value=str(args.lon))
    rad = tk.StringVar(value=str(args.radius))
    pi_var = tk.StringVar(value="0x54C4")
    for i, (lbl, var, w) in enumerate([("Lat", lat, 10), ("Lon", lon, 10), ("Radius km", rad, 7)]):
        ttk.Label(q, text=lbl).grid(row=0, column=i * 2, sticky="e", padx=(8, 2))
        ttk.Entry(q, textvariable=var, width=w).grid(row=0, column=i * 2 + 1, sticky="w")

    cols = ("freq", "call", "svc", "cls", "erp", "dist", "score", "city")
    tree = ttk.Treeview(root, columns=cols, show="headings", height=16)
    widths = {"freq": 70, "call": 90, "svc": 45, "cls": 45, "erp": 80, "dist": 80, "score": 70, "city": 220}
    heads = {"freq": "Freq", "call": "Callsign", "svc": "Svc", "cls": "Cls", "erp": "ERP kW",
             "dist": "Dist km", "score": "Score", "city": "City"}
    for c in cols:
        tree.heading(c, text=heads[c])
        tree.column(c, width=widths[c], anchor="center" if c != "city" else "w")

    pi_result = tk.StringVar()

    def find():
        try:
            res = query_nearby(state["db"].get(), float(lat.get()), float(lon.get()), float(rad.get()))
        except Exception as e:
            status.set(f"error: {e}")
            return
        tree.delete(*tree.get_children())
        for s in res:
            tree.insert("", "end", values=(
                s["freq"], s["callsign"], s["service"], s["class"] or "",
                f"{s['erp'] or 0:.2f}", f"{s['dist']:.1f}", f"{s['score']:.1f}",
                f"{s['city'] or ''}, {s['state'] or ''}"))
        refresh_status()

    def decode():
        try:
            pi = int(pi_var.get(), 0)
        except ValueError:
            pi_result.set("enter a PI like 0x54C4 or 21700")
            return
        cs, conf, note, st = identify_pi(state["db"].get(), pi)
        tag = "✓ confident" if conf else "· hint"
        where = f" → {st['callsign']} {st['frequency_mhz']} MHz, {st['city']}, {st['state']}" if st else ""
        pi_result.set(f"0x{pi:04X} = {cs or '(undecodable)'}  [{tag}: {note}]{where}")

    ttk.Button(q, text="Find nearby", command=find).grid(row=0, column=6, padx=10)

    pi_row = ttk.Frame(root, padding=(8, 4))
    pi_row.pack(fill="x")
    ttk.Label(pi_row, text="RDS PI:").pack(side="left")
    ttk.Entry(pi_row, textvariable=pi_var, width=10).pack(side="left", padx=4)
    ttk.Button(pi_row, text="Decode", command=decode).pack(side="left")
    ttk.Label(pi_row, textvariable=pi_result).pack(side="left", padx=10)

    tree.pack(fill="both", expand=True, padx=8, pady=4)
    ttk.Label(root, textvariable=status, relief="sunken", anchor="w").pack(fill="x", side="bottom")

    refresh_status()
    root.mainloop()


def main():
    p = argparse.ArgumentParser(description="Browse the CarFM station DB with the app's ranking logic.")
    p.add_argument("--db", default=DEFAULT_DB)
    p.add_argument("--lat", type=float, default=37.7749)
    p.add_argument("--lon", type=float, default=-122.4194)
    p.add_argument("--radius", type=float, default=100.0)
    p.add_argument("--limit", type=int, default=50)
    p.add_argument("--pi", help="decode a PI (e.g. 0x54C4 or 21700) and exit")
    p.add_argument("--cli", action="store_true", help="print to stdout instead of opening the GUI")
    args = p.parse_args()
    if args.cli or args.pi is not None:
        run_cli(args)
    else:
        run_gui(args)


if __name__ == "__main__":
    main()
