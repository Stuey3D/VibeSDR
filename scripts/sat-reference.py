#!/usr/bin/env python3
"""V12 satellite engine — the independent reference for test-sat-orbit §3 (docs/v12/SATELLITE-ENGINE.md).

Skyfield (MIT, its own TEME→ITRS chain and event finder) computes looks (azimuth, elevation, range, range rate) and
passes (AOS / TCA / LOS / maximum elevation) for the CelesTrak fixture, and prints them in the form the C++ test reads:

  LOOK norad lat lon unix az el rangeKm rangeRateKmS
  PASS norad lat lon fromUnix aos tca los maxEl

Run once, by hand, in a scratch virtualenv (pip install skyfield==1.55) — the output is committed as
vibeserver/testdata-sat/reference-passes.txt so the test itself needs nothing but a compiler:

  python3 scripts/sat-reference.py vibeserver/testdata-sat/celestrak-amateur-2026-11-19.json > vibeserver/testdata-sat/reference-passes.txt
"""
import datetime
import json
import sys

from skyfield.api import EarthSatellite, load, wgs84

ts = load.timescale(builtin=True)
data = json.load(open(sys.argv[1]))
by_id = {o["NORAD_CAT_ID"]: o for o in data}

PLACES = [(52.270833, -0.875, "IO92ng"), (-23.5, -46.6, "Sao Paulo"), (64.1, -21.9, "Reykjavik")]
SATS = [25544, 27607, 7530, 44909, 61781]
START = 1795600000  # 2026-11-25T09:46:40Z — six days after the fixture's epochs


def unix(t):
    return t.utc_datetime().timestamp()  # UTC, as the engine's clock (SGP4's epochs are UTC)


def at(u):
    # Unix seconds (no leap seconds) → a Skyfield time. ★ Not ts.utc(1970, …, u): that counts the 27 leap seconds.
    return ts.from_datetime(datetime.datetime.fromtimestamp(u, datetime.timezone.utc))


for norad in SATS:
    sat = EarthSatellite.from_omm(ts, by_id[norad])
    for lat, lon, _ in PLACES:
        place = wgs84.latlon(lat, lon, 0)
        diff = sat - place
        t0 = at(START)
        t1 = at(START + 3 * 3600)
        times, events = sat.find_events(place, t0, t1, altitude_degrees=0.0)
        aos = None
        tca = None
        for t, e in zip(times, events):
            if e == 0:
                aos = t
            elif e == 1:
                tca = t
            elif e == 2 and aos is not None and tca is not None:
                alt, _, _ = diff.at(tca).altaz()
                print("PASS %d %.6f %.6f %.3f %.3f %.3f %.3f %.4f" % (norad, lat, lon, START, unix(aos), unix(tca), unix(t), alt.degrees))
                # looks through the pass, with the range rate as a central difference over ±50 ms
                for k in range(5):
                    tu = unix(aos) + (unix(t) - unix(aos)) * (k + 0.5) / 5
                    tt = at(tu)
                    alt, az, dist = diff.at(tt).altaz()
                    da = diff.at(at(tu + 0.05)).distance().km
                    db = diff.at(at(tu - 0.05)).distance().km
                    print("LOOK %d %.6f %.6f %.3f %.5f %.5f %.4f %.7f" % (norad, lat, lon, tu, az.degrees, alt.degrees, dist.km, (da - db) / 0.1))
                aos = tca = None
