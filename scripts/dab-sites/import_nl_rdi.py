#!/usr/bin/env python3
"""import_nl_rdi.py — the Netherlands' DAB transmitters from RDI, into the header the server compiles in.

    python3 scripts/dab-sites/import_nl_rdi.py                  # find the current file, download, write
    python3 scripts/dab-sites/import_nl_rdi.py --file x.txt     # from a copy already on disk

Writes android/app/src/main/cpp/vibe_dab_txdb_nl.h.

★ THE SOURCE (2026-10-06). RDI (Rijksinspectie Digitale Infrastructuur) publishes "Overzicht DAB en
  DVB-T2 zenders" as a `;`-separated text file whose name carries its date, so the importer reads the
  document page for the current link. rdi.nl's copyright statement: "Voor deze website geldt de
  Creative Commons zero-verklaring (CC0 1.0)" — the exceptions are texts marked as copyrighted and
  images; this file is neither. CC0 asks for nothing; we credit RDI anyway.
  Columns: Opstelpunt (site); Coord OL; Coord NB ("005E25 47.36" = 5°25'47.36" E); Kanaal (the
  block for DAB, a number for DVB-T2); Bandgrenzen; ERP(kW); Eff.Hoogte; Polarisatie; Provincie.
★★ NO ENSEMBLE, NO EId, NO TII — matched on country (EId nibble 8) + block (kDabTxByBlock), the
   province shown as the area.
"""
import argparse, os, re, sys, time, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dabsites import SOURCES, BY_BLOCK, Site, is_block, tidy_name, write_header  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
OUT = os.path.join(ROOT, 'android', 'app', 'src', 'main', 'cpp', 'vibe_dab_txdb_nl.h')
UA = 'VibeSDR-dab-sites/1.0 (+https://github.com/Stuey3D/VibeSDR; open-source importer)'

_COORD = re.compile(r'^\s*(\d{1,3})([NSEW])(\d{1,2})\s+(\d{1,2}(?:\.\d+)?)\s*$')


def coord(s: str) -> float:
    """'005E25 47.36' → 5.42982; '51N25 15.68' → 51.42102. Raises ValueError on anything else."""
    m = _COORD.match(s or '')
    if not m:
        raise ValueError('not an RDI coordinate: %r' % s)
    v = int(m.group(1)) + int(m.group(3)) / 60.0 + float(m.group(4)) / 3600.0
    return -v if m.group(2) in 'SW' else v


def fetch(url: str) -> str:
    req = urllib.request.Request(url, headers={'User-Agent': UA})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read().decode('utf-8', errors='replace')


def current_file_url() -> str:
    page = fetch(SOURCES['rdi']['url'])
    links = re.findall(r'href="(https://www\.rdi\.nl/site/binaries/[^"]+\.txt)"', page)
    if not links:
        raise SystemExit('no .txt link on %s — the page has changed shape' % SOURCES['rdi']['url'])
    return links[0]


def normalise(text: str) -> list:
    src = SOURCES['rdi']
    lines = [l for l in text.splitlines() if l.strip()]
    hdr = [h.strip() for h in lines[0].split(';')]
    ix = {h: i for i, h in enumerate(hdr)}
    for n in ('Opstelpunt', 'Coord OL', 'Coord NB', 'Kanaal', 'Provincie'):
        if n not in ix:
            raise SystemExit('RDI file has changed shape — missing column %s' % n)
    rows, seen = [], set()
    for l in lines[1:]:
        c = [x.strip() for x in l.split(';')]
        if len(c) < len(hdr):
            continue
        block = c[ix['Kanaal']].upper()
        if not is_block(block):
            continue                                 # a DVB-T2 channel, not a DAB block
        try:
            lat, lon = coord(c[ix['Coord NB']]), coord(c[ix['Coord OL']])
        except ValueError:
            continue
        if not (50.6 < lat < 53.8 and 3.2 < lon < 7.4):
            continue
        site = tidy_name(c[ix['Opstelpunt']])
        if not site or (site, block) in seen:
            continue
        seen.add((site, block))
        rows.append(Site(eid=src['eid_nibble'] << 12, main=0, sub=0, site=site,
                         area=tidy_name(c[ix['Provincie']]), lat=round(lat, 5), lon=round(lon, 5),
                         block=block, src=src['id'], flags=BY_BLOCK))
    rows.sort(key=lambda x: (x.block, x.site))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--file', help='a downloaded zenderlijst-*.txt instead of fetching')
    ap.add_argument('--out', default=OUT)
    a = ap.parse_args()
    if a.file:
        text = open(a.file, encoding='utf-8', errors='replace').read()
        origin = os.path.basename(a.file)
    else:
        url = current_file_url()
        time.sleep(2)                                # polite: one request, a pause, the next
        text = fetch(url)
        origin = url
    rows = normalise(text)
    sites = len({r.site for r in rows})
    note = ('Netherlands (ECC E3, EId country 8), %d DAB sites; by-block (no EId/TII/ensemble in the source). '
            'Fetched %s from %s.' % (sites, time.strftime('%Y-%m-%d'), origin))
    n = write_header(a.out, rows, 'NL', 'rdi', note)
    print('wrote %d rows (%d sites) to %s' % (n, sites, a.out))


if __name__ == '__main__':
    main()
