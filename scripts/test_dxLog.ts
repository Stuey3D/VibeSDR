/**
 * The executable spec for src/utils/dxLog.ts's frequency column, tags and footer — the DX Log's one rule for the app and
 * the web page (V12, 2026-11-16: the OIRT / extended FM ranges, and the locals in the discovered bookmarks).
 *
 * Run: node --no-warnings scripts/test_dxLog.ts
 */
import { dxWhere, dxTags, dxFooter, type DxEntry, type DxPage } from '../src/utils/dxLog.ts';

let fails = 0;
const ok = (cond: boolean, what: string) => {
  if (cond) console.log('  ok   ' + what);
  else { console.log('  FAIL ' + what); fails++; }
};
const fm = (freqKhz: number, extra: Partial<DxEntry> = {}): DxEntry => ({
  id: 1, band: 'FM', key: 'k', class: 'dx', freqKhz, firstHeard: 0, lastHeard: 0, timesHeard: 1, presence: 0, openings: 1,
  live: false, peakSnrDb: 20, ...extra,
});

ok(dxWhere(fm(98800, { range: 'fm' })) === '98.8', 'the main band: one decimal (98.8)');
ok(dxWhere(fm(69800, { range: 'oirt' })) === '69.80', '★ OIRT: always two decimals on its 30 kHz raster (69.80)');
ok(dxWhere(fm(66440, { range: 'oirt' })) === '66.44', 'OIRT 66.44');
ok(dxWhere(fm(69800)) === '69.80', 'an OIRT frequency from a server that sends no range still reads 69.80');
ok(dxWhere(fm(86650, { range: 'ext' })) === '86.65', '★ a pirate off the 100 kHz raster: 86.65');
ok(dxWhere(fm(76100, { range: 'ext' })) === '76.1', 'Japan 76.1');
ok(dxTags(fm(69800, { range: 'oirt' })).includes('OIRT'), 'an OIRT row is tagged OIRT');
ok(dxTags(fm(86650, { range: 'ext' })).includes('EXT FM'), 'an extended-range row is tagged EXT FM');
ok(dxTags(fm(98800, { class: 'local', inBookmarks: true })).includes('BOOKMARKED'), '★ a local in the discovered bookmarks reads BOOKMARKED');
ok(!dxTags(fm(98800, { class: 'dx' })).includes('BOOKMARKED'), 'a DX entry is not');
const page = (scanner: DxPage['scanner'], local = 0): DxPage => ({
  ok: true, total: 3, offset: 0, limit: 25, counts: { dx: 3, local, suspect: 0, provisional: 0 }, bootstrap: false, bootstrapEnds: 0,
  sweeps: { fm: 4, dab: 2 }, alertSeq: 0, now: 0, entries: [], scanner,
});
ok(/OIRT 65\.8–74, 76–87\.5, 87\.5–108 MHz/.test(dxFooter(page({ on: true, bands: 'both', waitMin: 5, raster: 100, viewAdmin: false, oirt: true, ext: true }), 3)),
   'the footer names the FM ranges swept');
ok(!/OIRT/.test(dxFooter(page({ on: true, bands: 'both', waitMin: 5, raster: 100, viewAdmin: false }), 3)), 'an older server (no range switches): no range line');
ok(/locals go into the bookmarks/.test(dxFooter(page(undefined, 2), 3)), 'with locals: the footer says they go into the bookmarks');

console.log(fails ? `\n${fails} FAILED` : '\nall passed');
process.exit(fails ? 1 : 0);
