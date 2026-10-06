#!/usr/bin/env python3
"""import_ch_bakom.py — Switzerland's DAB+ transmitter sites from BAKOM, into the header the server
compiles in.

    python3 scripts/dab-sites/import_ch_bakom.py                 # download, then write the header
    python3 scripts/dab-sites/import_ch_bakom.py --file x.json   # from a copy already on disk

Writes android/app/src/main/cpp/vibe_dab_txdb_ch.h.

★ THE SOURCE (2026-10-06). BAKOM's "Swiss radio and TV broadcasters" (ch.bakom.radio-fernsehsender),
  published on opendata.swiss under the "Open use" terms (terms_open: commercial and non-commercial
  use allowed, citing the source recommended — we cite it anyway, in the header, the About credits
  and docs/DAB-SITES-SOURCES.md). The federal geodata terms (geo.admin.ch) add that data "may be
  processed, analysed and published in accordance with their terms of use". It is the GeoJSON the
  map at map.geo.admin.ch draws, refreshed by BAKOM (the dataset says ANNUAL; the file is
  regenerated daily) — LV95 coordinates, one feature per site, with three parallel comma lists:
  service ("DAB+", "RADIO", "DVB-T"), program (the ensemble, "SRG  D01") and freqchan ("12C").
★★ IT HAS NO EIds AND NO TII CODES. So these rows cannot name a TII hit — only the panel's
   "Licensed sites" row, matched by the EId's country nibble (4 = Switzerland) and the block we
   are tuned to (kDabTxByBlock in vibe_dab_txdb.h). The ensemble's name goes in `area`, so the
   line reads "Uetliberg (SRG D01) · 3 mi · BAKOM record" and a listener can check it is theirs.
"""
import argparse, json, os, sys, time, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dabsites import SOURCES, BY_BLOCK, Site, is_block, lv95_to_wgs84, tidy_name, write_header  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
OUT = os.path.join(ROOT, 'android', 'app', 'src', 'main', 'cpp', 'vibe_dab_txdb_ch.h')
UA = 'VibeSDR-dab-sites/1.0 (+https://github.com/Stuey3D/VibeSDR; open-source importer)'


def fetch(url: str) -> dict:
    req = urllib.request.Request(url, headers={'User-Agent': UA})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode('utf-8'))


def ensemble_name(s: str) -> str:
    s = ' '.join((s or '').split())
    return '' if not s or set(s) <= {'?'} else s


def normalise(geojson: dict) -> list:
    """GeoJSON features → one Site per (site, block). Rows whose block is not a real DAB block,
    or whose coordinates fall outside Switzerland and its border sites, are dropped, not guessed."""
    src = SOURCES['bakom']
    nib = src['eid_nibble'] << 12
    rows, seen = [], set()
    for f in geojson.get('features', []):
        p = f.get('properties') or {}
        g = f.get('geometry') or {}
        if g.get('type') != 'Point':
            continue
        try:
            e, n = float(g['coordinates'][0]), float(g['coordinates'][1])
        except (KeyError, IndexError, TypeError, ValueError):
            continue
        svc = (p.get('service') or '').split(',')
        prog = (p.get('program') or '').split(',')
        chan = (p.get('freqchan') or '').split(',')
        if not (len(svc) == len(prog) == len(chan)):
            continue                        # the three lists must line up or the pairing is a guess
        lat, lon = lv95_to_wgs84(e, n)
        if not (45.5 < lat < 48.1 and 5.7 < lon < 10.8):
            continue
        site = tidy_name(p.get('name') or '')
        if not site:
            continue
        for s, pr, ch in zip(svc, prog, chan):
            if 'DAB' not in s.upper():
                continue
            block = ch.strip().upper()
            if not is_block(block):
                continue
            key = (site, block)
            if key in seen:
                continue
            seen.add(key)
            rows.append(Site(eid=nib, main=0, sub=0, site=site, area=ensemble_name(pr),
                             lat=round(lat, 5), lon=round(lon, 5), block=block,
                             src=src['id'], flags=BY_BLOCK))
    rows.sort(key=lambda r: (r.block, r.site))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--file', help='a downloaded radio-fernsehsender_2056_*.json instead of fetching')
    ap.add_argument('--out', default=OUT)
    a = ap.parse_args()
    if a.file:
        data = json.load(open(a.file, encoding='utf-8'))
        origin = os.path.basename(a.file)
    else:
        data = fetch(SOURCES['bakom']['download'])
        origin = SOURCES['bakom']['download']
    rows = normalise(data)
    sites = len({r.site for r in rows})
    note = ('Switzerland (ECC E1, EId country 4), %d DAB+ sites × blocks, by-block (no EId/TII in the source). '
            'Fetched %s from %s.' % (sites, time.strftime('%Y-%m-%d'), origin))
    n = write_header(a.out, rows, 'CH', 'bakom', note)
    print('wrote %d rows (%d sites) to %s' % (n, sites, a.out))


if __name__ == '__main__':
    main()
