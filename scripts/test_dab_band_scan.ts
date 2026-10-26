/**
 * The DAB band scan's result list, as both clients show it (src/services/dabBandScan.ts, V12 2026-10-26).
 * M7EIW: "a scan function for DAB which also shows the results".
 * Run: node --no-warnings scripts/test_dab_band_scan.ts   (run-tests.sh does)
 */
import { parseDabScan, dabScanRows, dabScanLine, dabScanOthersLine, dabScanQuality, dabScanCount, dabScanMHz,
         dabScanToggle } from '../src/services/dabBandScan.ts';

let fails = 0;
const ok = (c: boolean, w: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${w}`); if (!c) fails++; };

const hit = (block: string, channel: number, n: number, fib = 1) => ({ block, channel, hz: 222064000, ensemble: ` ${block} MUX `, fib, mer: 20,
  complete: true, services: Array.from({ length: n }, (_, i) => ({ sid: 0xC000 + i, label: `S${i}` })) });

console.log('parse');
ok(parseDabScan({ type: 'dab' }) === null, 'not a dab_scan message → null');
const p = parseDabScan({ type: 'dab_scan', running: true, done: 12, total: 41, current: '9D', found: [hit('11D', 29, 3), hit('7D', 11, 2),
  { block: '', channel: -1 }, { block: '12B', channel: 32, services: [{ sid: 0, label: 'x' }, { sid: 5, label: '  ' }, { sid: 6, label: 'OK' }] }] })!;
ok(p.found.length === 3, 'a row with no block is dropped');
ok(p.found[0].ensemble === '11D MUX', 'the ensemble name is trimmed (DAB pads to 16)');
ok(p.found[2].services.length === 1 && p.found[2].services[0].label === 'OK', 'services with no SId or no name are dropped');
ok(parseDabScan({ type: 'dab_scan', found: 'nope', done: 'x' })!.found.length === 0, 'garbage fields do not throw');

console.log('rows and words');
ok(dabScanRows(p).map((h) => h.block).join(',') === '7D,11D,12B', 'rows in block order whatever the arrival order');
ok(dabScanLine(p) === 'Scanning 12 / 41 blocks · 9D — 3 ensembles so far', `progress: "${dabScanLine(p)}"`);
const done = parseDabScan({ type: 'dab_scan', running: false, finished: true, done: 41, total: 41, added: 5, found: [hit('11D', 29, 3), hit('7D', 11, 2)] })!;
ok(dabScanLine(done) === '2 ensembles, 5 stations — 5 stations added to bookmarks', `finished: "${dabScanLine(done)}"`);
const one = parseDabScan({ type: 'dab_scan', finished: true, done: 41, total: 41, added: 1, found: [hit('11D', 29, 1)] })!;
ok(dabScanLine(one) === '1 ensemble, 1 station — 1 station added to bookmarks', `singulars: "${dabScanLine(one)}"`);
const none = parseDabScan({ type: 'dab_scan', finished: true, done: 41, total: 41, added: 0, found: [] })!;
ok(/^No DAB found on any of the 41 blocks/.test(dabScanLine(none)), `nothing: "${dabScanLine(none)}"`);
const stop = parseDabScan({ type: 'dab_scan', finished: true, cancelled: true, done: 9, total: 41, added: 0, why: 'stopped — the dial was tuned', found: [] })!;
ok(dabScanLine(stop) === 'Scan stopped (the dial was tuned) at 9 / 41 — 0 ensembles, 0 stations added to bookmarks', `stopped: "${dabScanLine(stop)}"`);
const err = parseDabScan({ type: 'dab_scan', running: false, error: 'Scanning is only offered on a single-listener radio.' })!;
ok(dabScanLine(err).startsWith('Scanning is only offered'), 'a refusal is shown in its own words');
ok(dabScanCount(hit('11D', 29, 1)) === '1 station' && dabScanCount(hit('11D', 29, 12)) === '12 stations', 'the count reads naturally');
ok(dabScanMHz(222064000) === '222.064 MHz', 'the frequency');
ok(dabScanQuality(hit('a', 1, 1, 0.97)).word === 'strong' && dabScanQuality(hit('a', 1, 1, 0.7)).word === 'good'
   && dabScanQuality(hit('a', 1, 1, 0.3)).word === 'weak' && dabScanQuality(hit('a', 1, 1, 0.97)).pct === 97, 'quality from the FIB rate');

console.log('others on the radio');
ok(dabScanOthersLine(parseDabScan({ type: 'dab_scan', running: true, by: 'admin', done: 4, total: 41, found: [] })).startsWith('The owner is scanning'), 'an admin scan is announced to everybody else');
ok(dabScanOthersLine(parseDabScan({ type: 'dab_scan', running: true, by: 'listener', done: 4, total: 41, found: [] })) === '', '…a single-user listener’s own scan is not');

console.log('expanding');
let open = new Set<string>();
open = dabScanToggle(open, '11D'); open = dabScanToggle(open, '7D');
ok(open.has('11D') && open.has('7D'), 'two rows open at once');
open = dabScanToggle(open, '11D');
ok(!open.has('11D') && open.has('7D'), 'closing one keeps the other');

console.log(fails ? `test_dab_band_scan: ${fails} FAILED` : 'test_dab_band_scan: all passed');
process.exit(fails ? 1 : 0);
