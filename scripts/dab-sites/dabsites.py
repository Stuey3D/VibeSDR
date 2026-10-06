"""dabsites.py — the shared half of the DAB transmitter-site importers.

★★★ ONLY SOURCES WHOSE LICENCE LETS A PUBLIC REPOSITORY CARRY THEM (Stuart, 2026-10-06: "incorporate
    them whilst being legally and ethically compliant"). Every source we compile in is listed in
    SOURCES below with its licence, the attribution it asks for and where it came from, and every
    row it produces carries that source's id — the panel then says "BAKOM record" or "Ofcom record",
    never a bare name with no owner. docs/DAB-SITES-SOURCES.md has the full survey, including the
    countries left OUT and why. ✗ FMLIST, wohnort.org and other community lists are not here and
    must not be added: their terms do not allow redistribution. A server owner may still use their
    OWN copy (vibe_dab_txdb.h loadTxListFile) — that is read on their machine, never shipped by us.

The normalised row is what the C++ table (vibe_dab_txdb.h, struct DabTx) holds:
    eid, main, sub, site, area, lat, lon, block, src, flags
A source with no EIds and no TII codes is written with flags=BY_BLOCK and eid = the EId's country
nibble << 12, and the server matches it on country + the block it is tuned to.
"""
from __future__ import annotations

import math
import re
from dataclasses import dataclass, field

# ── The sources. `id` is the C++ kDabSrc* value; `label` is what the panel prints. ──────────────
SOURCES = {
    'ofcom': dict(
        id=0, label='Ofcom record', country='GB', ecc=0xE1,
        name='Ofcom — Technical parameters for broadcast radio transmitters (DAB)',
        url='https://www.ofcom.org.uk/tv-radio-and-on-demand/coverage-and-transmitters/radio-tech-parameters',
        licence='Open Government Licence v3.0',
        licence_url='https://www.nationalarchives.gov.uk/doc/open-government-licence/version/3/',
        attribution='Contains public sector information licensed under the Open Government Licence v3.0.',
    ),
    'bakom': dict(
        id=1, label='BAKOM record', country='CH', ecc=0xE1, eid_nibble=0x4,
        name='BAKOM (Federal Office of Communications) — Swiss radio and TV broadcasters '
             '(ch.bakom.radio-fernsehsender)',
        url='https://opendata.swiss/en/dataset/schweizerische-radio-und-fernsehsender',
        download='https://data.geo.admin.ch/ch.bakom.radio-fernsehsender/radio-fernsehsender/'
                 'radio-fernsehsender_2056_en.json',
        licence='opendata.swiss terms of use — "Open use" (terms_open)',
        licence_url='https://opendata.swiss/en/terms-of-use#terms_open',
        attribution='Source: Federal Office of Communications OFCOM (BAKOM), Switzerland — '
                    '"Swiss radio and TV broadcasters", opendata.swiss.',
    ),
    'ctu': dict(
        id=2, label='ČTÚ record', country='CZ', ecc=0xE2, eid_nibble=0x2,
        name='ČTÚ (Czech Telecommunication Office) — Rozhlasové vysílače (radio transmitters), '
             'DAB rows ("DAB Final Lic")',
        url='https://data.ctu.gov.cz/dataset/rozhlasove-vysilace',
        download='https://data.ctu.gov.cz/sites/default/files/imports/import_rozhlas/'
                 'prehled_rozhlasovych_kmitoctu.csv',
        licence='Czech open-data terms of use (data.gov.cz / NKOD): contains no copyright works, is not '
                'a copyright database, is not protected by the database maker\'s sui generis right, '
                'contains no personal data — equivalent to CC0',
        licence_url='https://data.gov.cz/podmínky-užití/neobsahuje-autorská-díla/',
        attribution='Source: Český telekomunikační úřad (Czech Telecommunication Office), '
                    '"Rozhlasové vysílače", data.ctu.gov.cz.',
    ),
    'rdi': dict(
        id=3, label='RDI record', country='NL', ecc=0xE3, eid_nibble=0x8,
        name='RDI (Rijksinspectie Digitale Infrastructuur) — Overzicht DAB en DVB-T2 zenders',
        url='https://www.rdi.nl/documenten/2024/08/13/overzicht-dab-en-dvb-t2-zenders',
        download='https://www.rdi.nl/site/binaries/site-content/collections/documenten/2024/08/13/'
                 'overzicht-dab-en-dvb-t2-zenders/zenderlijst-totaal-16-sept.txt',
        licence='Creative Commons Zero (CC0 1.0) — rdi.nl copyright statement ("Voor deze website geldt '
                'de Creative Commons zero-verklaring (CC0 1.0)")',
        licence_url='https://www.rdi.nl/copyright',
        attribution='Source: Rijksinspectie Digitale Infrastructuur (RDI), "Overzicht DAB en DVB-T2 '
                    'zenders" (CC0 — credited as a courtesy).',
    ),
}

BY_BLOCK = 0x01
EID_AND_BLOCK = 0x02


def dms(deg, mins, secs) -> float:
    return float(deg) + float(mins) / 60.0 + float(secs) / 3600.0

# Band III and the L-band blocks a row may name (EN 300 401 / the server's kBandIII list).
_BLOCK_RE = re.compile(r'^(?:[5-9][A-D]|1[0-3][A-F]|L[A-W])$')


@dataclass(order=True)
class Site:
    eid: int
    main: int
    sub: int
    site: str
    area: str
    lat: float
    lon: float
    block: str
    src: int = 0
    flags: int = 0
    extra: dict = field(default_factory=dict, compare=False)


def is_block(name: str) -> bool:
    return bool(_BLOCK_RE.match(name or ''))


def lv95_to_wgs84(e: float, n: float) -> tuple[float, float]:
    """Swiss LV95 (EPSG:2056) easting/northing → WGS84 lat/lon, by swisstopo's published
    approximate formulas ("Approximate formulas for the transformation between Swiss projection
    coordinates and WGS84") — good to about a metre, far finer than a transmitter site needs."""
    y = (e - 2600000.0) / 1e6
    x = (n - 1200000.0) / 1e6
    lam = (2.6779094 + 4.728982 * y + 0.791484 * y * x + 0.1306 * y * x * x - 0.0436 * y ** 3)
    phi = (16.9023892 + 3.238272 * x - 0.270978 * y * y - 0.002528 * x * x
           - 0.0447 * y * y * x - 0.0140 * x ** 3)
    return phi * 100.0 / 36.0, lam * 100.0 / 36.0


def tidy_name(s: str) -> str:
    """A place name for the panel: whitespace collapsed, ALL-CAPS turned into Title Case (BAKOM
    sends "CHATEAU D OEX SAROUCHE"), and nothing that would break a C string literal."""
    s = re.sub(r'\s+', ' ', (s or '').strip())
    if s.isupper():
        # single letters are the stripped elisions and abbreviations ("D OEX", "LANGNAU I E")
        s = ' '.join(w.lower() if len(w) == 1 else '-'.join(p.capitalize() for p in w.split('-'))
                     for w in s.split(' '))
    return c_safe(s)


def c_safe(s: str) -> str:
    return (s or '').replace('\\', '').replace('"', "'").replace('\n', ' ').strip()


def haversine_km(lat1, lon1, lat2, lon2) -> float:
    r, d = 6371.0088, math.pi / 180
    a = (math.sin((lat2 - lat1) * d / 2) ** 2
         + math.cos(lat1 * d) * math.cos(lat2 * d) * math.sin((lon2 - lon1) * d / 2) ** 2)
    return 2 * r * math.asin(math.sqrt(a))


def write_header(path_or_file, rows: list[Site], table: str, source_key: str, note: str) -> int:
    """Write vibe_dab_txdb_<x>.h. Returns the row count. The licence and attribution go at the
    top of the file, so the provenance travels with the data wherever the header is copied."""
    src = SOURCES[source_key]
    out = []
    w = out.append
    w('// vibe_dab_txdb_%s.h — GENERATED by scripts/dab-sites/. Do not edit; re-run the importer.' % table.lower())
    w('// %d rows. %s' % (len(rows), note))
    w('// Source:      %s' % src['name'])
    w('//              %s' % src['url'])
    w('// Licence:     %s — %s' % (src['licence'], src['licence_url']))
    w('// Attribution: %s' % src['attribution'])
    w('// Provenance:  every row below carries src=%d (kDabSrc… in vibe_dab_txdb.h); see docs/DAB-SITES-SOURCES.md.' % src['id'])
    w('#pragma once')
    w('#include "vibe_dab_txdb.h"')
    w('namespace vibedab {')
    w('inline const DabTx kDabTx_%s[] = {' % table)
    for r in rows:
        w('    { 0x%04X, %2d, %2d, "%s", "%s", %.5ff, %.5ff, "%s", %d, %d },'
          % (r.eid, r.main, r.sub, c_safe(r.site), c_safe(r.area), r.lat, r.lon, r.block, r.src, r.flags))
    w('};')
    w('inline const size_t kDabTx_%s_n = sizeof(kDabTx_%s) / sizeof(kDabTx_%s[0]);' % (table, table, table))
    w('}  // namespace vibedab')
    text = '\n'.join(out) + '\n'
    if hasattr(path_or_file, 'write'):
        path_or_file.write(text)
    else:
        with open(path_or_file, 'w', encoding='utf-8') as f:
            f.write(text)
    return len(rows)
