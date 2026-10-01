/**
 * gridToLatLon (src/services/grid.ts): real Maidenhead locators decode; FT8's non-grid third fields never do.
 * ★★ B10: "RR73" (FT8's sign-off) looks like a square near the North Pole — every station signing off was
 *    plotted in the Arctic. Reports ("-12", "R-12", "73", "RRR") reached the map as "unparseable grids".
 *
 * Run: node --no-warnings scripts/test_grid.ts   (run-tests.sh does)
 */
import { gridToLatLon } from '../src/services/grid.ts';

let fails = 0, passes = 0;
const ok = (what: string, c: boolean) => { if (c) passes++; else { fails++; console.log('  FAIL ' + what); } };

for (const g of ['IO92', 'IO92nh', 'io92NH', 'JO01', 'AA00', 'RR99', 'FN31pr']) ok(`${g} decodes`, gridToLatLon(g) !== null);
for (const g of ['RR73', 'rr73', '-12', 'R-12', '73', 'RRR', 'R+05', '', 'IO9', 'IO92n', 'SS00', 'IO92ZZ'])
  ok(`"${g}" is not a grid`, gridToLatLon(g) === null);
const p = gridToLatLon('IO92nh')!;
ok('IO92nh lands near Northampton (52.3 N, 0.9 W)', Math.abs(p.lat - 52.31) < 0.05 && Math.abs(p.lon + 0.88) < 0.05);

console.log(`${fails ? 'FAIL' : 'ok'}  grid: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
