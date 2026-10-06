#!/usr/bin/env python3
"""test_dabsites.py — the DAB transmitter-site importers (2026-10-06): coordinates, the three
normalisers on small fixtures shaped like the real files, the refusal of a changed file, and the
header's provenance lines.

    python3 scripts/dab-sites/test_dabsites.py        (run-tests.sh does)

No network: every case is a fixture.
"""
import io, os, sys, unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dabsites as D                 # noqa: E402
import import_ch_bakom as CH         # noqa: E402
import import_cz_ctu as CZ           # noqa: E402
import import_nl_rdi as NL           # noqa: E402


class Coordinates(unittest.TestCase):
    def test_lv95_bern_origin(self):
        # swisstopo's projection origin, the old Bern observatory: 46°57'08.66" N, 7°26'22.50" E
        lat, lon = D.lv95_to_wgs84(2600000, 1200000)
        self.assertAlmostEqual(lat, 46.95108, places=4)
        self.assertAlmostEqual(lon, 7.43864, places=4)

    def test_lv95_swisstopo_worked_example(self):
        # the worked example in swisstopo's "approximate formulas": E 2 700 000, N 1 100 000
        # → 46°02'38.87" N, 8°43'49.79" E
        lat, lon = D.lv95_to_wgs84(2700000, 1100000)
        self.assertAlmostEqual(lat, 46 + 2 / 60 + 38.87 / 3600, places=4)
        self.assertAlmostEqual(lon, 8 + 43 / 60 + 49.79 / 3600, places=4)

    def test_rdi_coordinates(self):
        self.assertAlmostEqual(NL.coord('005E25 47.36'), 5 + 25 / 60 + 47.36 / 3600, places=6)
        self.assertAlmostEqual(NL.coord('51N25 15.68'), 51 + 25 / 60 + 15.68 / 3600, places=6)
        self.assertLess(NL.coord('001W00 00'), 0)
        with self.assertRaises(ValueError):
            NL.coord('51.4210')

    def test_dms(self):
        self.assertAlmostEqual(D.dms(49, 58, 15), 49.970833, places=5)


class Names(unittest.TestCase):
    def test_tidy(self):
        self.assertEqual(D.tidy_name('CHATEAU D OEX  SAROUCHE'), 'Chateau d Oex Sarouche')
        self.assertEqual(D.tidy_name('USTI NAD ORLICI'), 'Usti Nad Orlici')
        self.assertEqual(D.tidy_name('Den Haag'), 'Den Haag')                 # mixed case left alone
        self.assertEqual(D.tidy_name('SAINT-IMIER'), 'Saint-Imier')
        self.assertEqual(D.tidy_name('A "quoted" \\ name'), "A 'quoted'  name")

    def test_blocks(self):
        for b in ('5A', '9D', '12C', '13F', 'LA'):
            self.assertTrue(D.is_block(b), b)
        for b in ('4A', '12G', '93 MHz', '', '21', '5E'):
            self.assertFalse(D.is_block(b), b)


def _ch_feature(e, n, name, svc, prog, chan):
    return {'type': 'Feature', 'geometry': {'type': 'Point', 'coordinates': [e, n]},
            'properties': {'name': name, 'service': svc, 'program': prog, 'freqchan': chan}}


class Switzerland(unittest.TestCase):
    def test_normalise(self):
        gj = {'features': [
            _ch_feature(2679520, 1244780, 'ZUERICH UETLIBERG', 'DAB+,DAB+,RADIO', 'SRG  D01,SMC  D03,Radio X', '12C,7A,94.2 MHz'),
            _ch_feature(2679520, 1244780, 'ZUERICH UETLIBERG', 'DAB+', 'SRG  D01', '12C'),       # duplicate
            _ch_feature(2600000, 1200000, 'MISMATCHED', 'DAB+,DAB+', 'SRG  D01', '12C,7A'),        # lists disagree
            _ch_feature(2600000, 1200000, 'UNKNOWN MUX', 'DAB+', '???', '10B'),
            _ch_feature(9999999, 9999999, 'NOWHERE', 'DAB+', 'SRG  D01', '12C'),                   # outside CH
        ]}
        rows = CH.normalise(gj)
        self.assertEqual([(r.site, r.block, r.area) for r in rows],
                         [('Unknown Mux', '10B', ''), ('Zuerich Uetliberg', '12C', 'SRG D01'),
                          ('Zuerich Uetliberg', '7A', 'SMC D03')])
        for r in rows:
            self.assertEqual((r.eid, r.main, r.sub, r.src, r.flags), (0x4000, 0, 0, 1, D.BY_BLOCK))


CZ_HEADER = ('Vysílač,Blok,Polarizace,Program,"PI\xa0KÓD RDS","Výška nad mořem","ERP W","Kmitočet MHz",'
             '"Zem. délka stupně","Zem. délka minuty","Zem. délka sekundy","Zem. šířka stupně",'
             '"Zem. šířka minuty","Zem. šířka sekundy",Typ,"Zeměpisná délka","Zeměpisná šířka",ANT_ID\n')


class Czechia(unittest.TestCase):
    def test_normalise(self):
        text = CZ_HEADER + (
            '"USTI NAD ORLICI",,V,"ČRo Region Pardubice",280C,554,1000.00,98.6,16,22,28,49,57,36,"FM Final Lic",16.3744,49.96,1\n'
            'BEROUN,12C,V,"Vysílací síť A",0x2005,402,316.00,227.36,14,3,22,49,58,15,"DAB Final Lic",14.0561,49.9708,2\n'
            'BEROUN,12C,V,"Vysílací síť A",0x2005,402,316.00,227.36,14,3,22,49,58,15,"DAB Final Lic",14.0561,49.9708,2\n'
            '"PRAHA STRAHOV",10D,V,"Regionální síť 1D",,402,316.00,227.36,14,22,33,50,4,48,"DAB Final Lic",14.37,50.08,3\n'
            'ODDBALL,12C,V,"Vysílací síť A",0x9999,402,316.00,227.36,14,3,22,49,58,15,"DAB Final Lic",14.0,49.9,4\n')
        rows = CZ.normalise(text)
        got = [(hex(r.eid), r.flags, r.site, r.block, r.area) for r in rows]
        self.assertEqual(got, [
            ('0x2000', D.BY_BLOCK, 'Praha Strahov', '10D', 'Regionální síť 1D'),      # no EId → by block
            ('0x2000', D.BY_BLOCK, 'Oddball', '12C', 'Vysílací síť A'),              # foreign EId → by block
            ('0x2005', D.EID_AND_BLOCK, 'Beroun', '12C', 'Vysílací síť A'),
        ])
        self.assertAlmostEqual(rows[2].lat, 49 + 58 / 60 + 15 / 3600, places=4)
        self.assertTrue(all(r.src == 2 for r in rows))

    def test_changed_shape_is_refused(self):
        with self.assertRaises(SystemExit):
            CZ.normalise('Site,Block\nX,12C\n')


class Netherlands(unittest.TestCase):
    def test_normalise(self):
        text = ('Opstelpunt;Coord OL;Coord NB;Kanaal;Bandgrenzen;ERP(kW);Eff.Hoogte;Polarisatie;Provincie\r\n'
                'Eindhoven;005E25 47.36;51N25 15.68;5A;174.160 - 175.696;1.86;90;V;Noord-Brabant\r\n'
                'Eindhoven;005E25 47.36;51N25 15.68;5A;174.160 - 175.696;1.86;90;V;Noord-Brabant\r\n'
                'Lopik;004E55 00.00;51N58 00.00;27;522 - 530;50;300;H;Utrecht\r\n'          # DVB-T2
                'Faraway;020E00 00.00;60N00 00.00;5A;174.160 - 175.696;1;1;V;Elders\r\n')
        rows = NL.normalise(text)
        self.assertEqual([(r.site, r.block, r.area, hex(r.eid), r.flags, r.src) for r in rows],
                         [('Eindhoven', '5A', 'Noord-Brabant', '0x8000', D.BY_BLOCK, 3)])

    def test_changed_shape_is_refused(self):
        with self.assertRaises(SystemExit):
            NL.normalise('Site;Block\nX;5A\n')


class Header(unittest.TestCase):
    def test_provenance_and_rows(self):
        buf = io.StringIO()
        n = D.write_header(buf, [D.Site(0x4000, 0, 0, 'A "B"', 'SRG D01', 47.1, 8.2, '12C', 1, D.BY_BLOCK)],
                           'CH', 'bakom', 'note')
        t = buf.getvalue()
        self.assertEqual(n, 1)
        self.assertIn('// Licence:     opendata.swiss terms of use', t)
        self.assertIn('// Attribution: Source: Federal Office of Communications OFCOM (BAKOM)', t)
        self.assertIn('{ 0x4000,  0,  0, "A \'B\'", "SRG D01", 47.10000f, 8.20000f, "12C", 1, 1 },', t)
        self.assertIn('kDabTx_CH_n', t)

    def test_every_source_is_complete(self):
        ids = set()
        for k, s in D.SOURCES.items():
            for f in ('id', 'label', 'country', 'ecc', 'name', 'url', 'licence', 'licence_url', 'attribution'):
                self.assertTrue(s.get(f) is not None and s.get(f) != '', '%s lacks %s' % (k, f))
            self.assertNotIn(s['id'], ids)
            ids.add(s['id'])


if __name__ == '__main__':
    unittest.main(verbosity=1)
