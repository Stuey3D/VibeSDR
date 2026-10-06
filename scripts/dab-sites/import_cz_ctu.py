#!/usr/bin/env python3
"""import_cz_ctu.py — Czechia's DAB transmitters from ČTÚ, into the header the server compiles in.

    python3 scripts/dab-sites/import_cz_ctu.py                  # download, then write the header
    python3 scripts/dab-sites/import_cz_ctu.py --file x.csv     # from a copy already on disk

Writes android/app/src/main/cpp/vibe_dab_txdb_cz.h.

★ THE SOURCE (2026-10-06). ČTÚ's "Rozhlasové vysílače" (radio transmitters), refreshed daily. Its
  catalogue record in the national catalogue (NKOD, data.gov.cz) declares, per distribution:
  no copyright works, not a copyright database, no sui generis database right, no personal data —
  which the catalogue maps to CC0. Nothing to meet, and we credit ČTÚ anyway.
  Columns: Vysílač (site), Blok, Polarizace, Program (the multiplex, "Vysílací síť A"), "PI KÓD RDS"
  (for DAB rows: the EId, "0x2005"), Výška nad mořem, ERP W, Kmitočet MHz, the position in
  degrees/minutes/seconds, Typ ("DAB Final Lic"), and decimal degrees to four places.
★★ EIds, NO TII. So rows match on the EId like the UK's — and on the block too (kDabTxEidAndBlock):
   ČTÚ files one placeholder EId (0x211B) against several regional networks on different blocks.
   A DAB row with no EId falls back to country + block (kDabTxByBlock).
"""
import argparse, csv, io, os, sys, time, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dabsites import SOURCES, BY_BLOCK, EID_AND_BLOCK, Site, is_block, dms, tidy_name, write_header  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
OUT = os.path.join(ROOT, 'android', 'app', 'src', 'main', 'cpp', 'vibe_dab_txdb_cz.h')
UA = 'VibeSDR-dab-sites/1.0 (+https://github.com/Stuey3D/VibeSDR; open-source importer)'


def fetch(url: str) -> str:
    req = urllib.request.Request(url, headers={'User-Agent': UA})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read().decode('utf-8-sig')


def normalise(text: str) -> list:
    src = SOURCES['ctu']
    rd = csv.reader(io.StringIO(text))
    hdr = [' '.join(h.replace('\xa0', ' ').split()) for h in next(rd)]
    ix = {h: i for i, h in enumerate(hdr)}
    need = ['Vysílač', 'Blok', 'Program', 'PI KÓD RDS', 'Typ', 'ERP W',
            'Zem. délka stupně', 'Zem. délka minuty', 'Zem. délka sekundy',
            'Zem. šířka stupně', 'Zem. šířka minuty', 'Zem. šířka sekundy']
    missing = [n for n in need if n not in ix]
    if missing:
        raise SystemExit('ČTÚ file has changed shape — missing columns: %s' % ', '.join(missing))
    rows, seen = [], set()
    for r in rd:
        if len(r) < len(hdr) or 'DAB' not in r[ix['Typ']].upper():
            continue
        block = r[ix['Blok']].strip().upper()
        if not is_block(block):
            continue
        try:
            lat = dms(r[ix['Zem. šířka stupně']], r[ix['Zem. šířka minuty']], r[ix['Zem. šířka sekundy']])
            lon = dms(r[ix['Zem. délka stupně']], r[ix['Zem. délka minuty']], r[ix['Zem. délka sekundy']])
        except ValueError:
            continue
        if not (48.4 < lat < 51.2 and 11.9 < lon < 19.0):
            continue                                   # outside Czechia: a typo, not a site
        site = tidy_name(r[ix['Vysílač']])
        if not site:
            continue
        eid_s = r[ix['PI KÓD RDS']].strip().lower()
        try:
            eid = int(eid_s, 16) if eid_s else 0
        except ValueError:
            eid = 0
        if eid and (eid >> 12) == src['eid_nibble']:
            e, flags = eid, EID_AND_BLOCK
        else:
            e, flags = src['eid_nibble'] << 12, BY_BLOCK
        key = (e, flags, site, block)
        if key in seen:
            continue
        seen.add(key)
        erp = r[ix['ERP W']].strip()
        rows.append(Site(eid=e, main=0, sub=0, site=site, area=' '.join(r[ix['Program']].split()),
                         lat=round(lat, 5), lon=round(lon, 5), block=block, src=src['id'], flags=flags,
                         extra={'erp_w': erp}))
    rows.sort(key=lambda x: (x.eid, x.block, x.site))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--file', help='a downloaded prehled_rozhlasovych_kmitoctu.csv instead of fetching')
    ap.add_argument('--out', default=OUT)
    a = ap.parse_args()
    if a.file:
        text = open(a.file, encoding='utf-8-sig').read()
        origin = os.path.basename(a.file)
    else:
        text = fetch(SOURCES['ctu']['download'])
        origin = SOURCES['ctu']['download']
    rows = normalise(text)
    sites = len({r.site for r in rows})
    note = ('Czechia (ECC E2, EId country 2), %d DAB sites; matched on EId + block (no TII in the source). '
            'Fetched %s from %s.' % (sites, time.strftime('%Y-%m-%d'), origin))
    n = write_header(a.out, rows, 'CZ', 'ctu', note)
    print('wrote %d rows (%d sites) to %s' % (n, sites, a.out))


if __name__ == '__main__':
    main()
