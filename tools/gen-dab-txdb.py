#!/usr/bin/env python3
"""gen-dab-txdb.py — turn a DAB transmitter list into the header the server compiles in.

    python3 tools/gen-dab-txdb.py E1 ~/Downloads/txparamsdab.csv > android/app/src/main/cpp/vibe_dab_txdb_e1.h

The UK source is Ofcom's "Technical parameters for broadcast radio transmitters" (Open
Government Licence): https://www.ofcom.org.uk/spectrum/information/radio-tech-parameters —
the DAB CSV has EID, Site, Transmitter Area, Block, TII Main Id (Hex), TII Sub Id (Hex), Lat, Long.
The file itself: https://www.ofcom.org.uk/siteassets/resources/documents/spectrum/tv-transmitter-guidance/tech-parameters/txparamsdab.csv

★★ POWER AND HEIGHT (2026-11-25, Stuart: show why a farther site is stronger). The same file carries
   "In-Use ERP Total" and "In-Use Ae Ht"; both become fields of the row (DabTx erpKw / heightM):
   - "In-Use ERP Total" is kW, written with a comma after the third decimal: "9.549,926" = 9.549926 kW
     (Daventry, 12B). Read continentally — 9 549,926 W — it is the same 9.55 kW, so either reading
     agrees. Blank = not published, and the row gets 0 ("unknown"), never a guess.
   - "In-Use Ae Ht" is the aerial's height above the ground at the site, in metres (the MF and VHF sheets
     of Ofcom's txparams.xlsx call the same column "In-Use Aerial Ht"). "Site Height" — the ground above
     sea level — is not carried: one height, clearly labelled, is what the panel shows.
   Both columns are optional: an older file without them still generates, with no figures.

★ ONE COUNTRY PER FILE, KEYED BY ECC. Stuart, 2026-09-07: "have a DAB transmitter list for each
  country so each country gets the same treatment; the transmitter code we currently have as a
  fallback would be applicable if no transmitter directory available." Another country's list
  goes through the same generator (map its columns below) and lands in vibe_dab_txdb_<ecc>.h —
  or, without a rebuild, in <data dir>/dab-tii-<ecc>.csv as `eid,main,sub,site,area,lat,lon`,
  which the server loads at start (see vibe_dab_txdb.h).
★ 2026-10-06: the other countries' importers (Switzerland, Czechia, the Netherlands) live in
  scripts/dab-sites/, with each source's licence and attribution; docs/DAB-SITES-SOURCES.md has the
  survey. Rows from this generator take the DabTx defaults: src = Ofcom, matched by EId.
"""
import csv, sys

def erp_kw(v):
    """Ofcom's "In-Use ERP Total" → kW. '9.549,926' → 9.549926; blank or nonsense → 0 (not published)."""
    v = (v or '').strip().strip('"').replace(',', '')
    try:
        kw = float(v)
    except ValueError:
        return 0.0
    return kw if 0 < kw < 10000 else 0.0


def aerial_m(v):
    """Ofcom's "In-Use Ae Ht" → whole metres above ground; blank or nonsense → 0 (not published)."""
    try:
        h = int(round(float((v or '').strip().strip('"'))))
    except ValueError:
        return 0
    return h if 0 < h < 2000 else 0


def fmt_kw(kw):
    s = ('%.4f' % kw).rstrip('0') if kw > 0 else '0.'
    return s + '0' if s.endswith('.') else s     # always a float literal: 5.0f, never 5f


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr); sys.exit(2)
    ecc, path = sys.argv[1].upper(), sys.argv[2]
    rows = list(csv.reader(open(path, encoding='utf-8', errors='replace')))
    hdr = [h.strip() for h in rows[0]]
    ix = {h: i for i, h in enumerate(hdr)}
    need = ['EID', 'Site', 'Transmitter Area', 'Block', 'TII Main Id (Hex)', 'TII Sub Id (Hex)', 'Lat', 'Long']
    for n in need:
        if n not in ix: print('missing column', n, file=sys.stderr); sys.exit(1)
    has_erp, has_ht = 'In-Use ERP Total' in ix, 'In-Use Ae Ht' in ix
    out, seen = [], set()
    for r in rows[1:]:
        try:
            eid = int(r[ix['EID']].strip(), 16)
            # ★★ A BLANK TII IS A SITE WITH NO TII ASSIGNED, NOT A BAD ROW (2026-11-05). Ofcom leaves both columns empty
            #    for many small-scale multiplexes (Rugby and Daventry, C231, Cold Ashby, 9A) — and those are exactly the
            #    ones whose null symbol carries no TII, so the licence record is the ONLY way to name the site. They were
            #    being dropped here. 0/0 is DabTx's "no TII" (vibe_dab_txdb.h); a TII match never uses it.
            mt = r[ix['TII Main Id (Hex)']].strip().strip('"'); st = r[ix['TII Sub Id (Hex)']].strip().strip('"')
            main_ = int(mt, 16) if mt else 0
            sub = int(st, 16) if st else 0
            if (mt == '') != (st == ''): continue   # ★ half a code is a typo, not a site without one
            lat = float(r[ix['Lat']]); lon = float(r[ix['Long']])
        except (ValueError, IndexError):
            continue
        site = r[ix['Site']].strip().replace('\\', '').replace('"', "'")
        area = r[ix['Transmitter Area']].strip().replace('\\', '').replace('"', "'")
        key = (eid, main_, sub, round(lat, 3), round(lon, 3))
        if key in seen: continue
        seen.add(key)
        out.append((eid, main_, sub, site, area, lat, lon, r[ix['Block']].strip(),
                    erp_kw(r[ix['In-Use ERP Total']]) if has_erp else 0.0,
                    aerial_m(r[ix['In-Use Ae Ht']]) if has_ht else 0))
    out.sort()
    print('// vibe_dab_txdb_%s.h — GENERATED by tools/gen-dab-txdb.py from %s. Do not edit.' % (ecc.lower(), path.split('/')[-1]))
    print('// %d transmitters, ECC %s. Source: Ofcom technical parameters (Open Government Licence v3).' % (len(out), ecc))
    print('// Columns: eid, TII main, sub, site, area, lat, lon, block, src, flags, ERP kW ("In-Use ERP Total"), aerial height m')
    print('// above ground ("In-Use Ae Ht"), height reference (1 = aerial above ground, 0 = not published). 0 = not published.')
    print('#pragma once\n#include "vibe_dab_txdb.h"\nnamespace vibedab {')
    print('inline const DabTx kDabTx_%s[] = {' % ecc)
    # ★ src 0 (Ofcom), flags 0, then ERP kW, height m and the height's reference (1 = aerial above ground; 0 = none).
    for eid, m, s, site, area, lat, lon, blk, kw, ht in out:
        print('    { 0x%04X, %2d, %2d, "%s", "%s", %.5ff, %.5ff, "%s", 0, 0, %sf, %d, %d },'
              % (eid, m, s, site, area, lat, lon, blk, fmt_kw(kw), ht, 1 if ht > 0 else 0))
    print('};')
    print('inline const size_t kDabTx_%s_n = sizeof(kDabTx_%s) / sizeof(kDabTx_%s[0]);' % (ecc, ecc, ecc))
    print('}  // namespace vibedab')

if __name__ == '__main__':
    main()
