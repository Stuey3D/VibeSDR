/**
 * The "Licensed sites" line's tail (src/services/dabTypes.ts dabLicensedTail) — whose record a site is.
 *
 * ★★ 2026-10-06: the server lists Switzerland's (BAKOM), Czechia's (ČTÚ) and the Netherlands' (RDI)
 *    sites as well as Ofcom's, and an owner's own TII list; it names the source in `src`, and only
 *    Ofcom's rows carry a TII code. The panel must never credit Ofcom with another regulator's data,
 *    and an older server (no `src`) was only ever Ofcom's. The app and the web client share this one copy.
 *
 * Run: node --no-warnings scripts/test_dab_licensed.ts   (run-tests.sh does)
 */
import { dabLicensedTail, dabTxSpecsTail, dabTxPowerText, dabTxHeightText, parseDabMessage } from '../src/services/dabTypes.ts';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};

eq('older server, Ofcom row', dabLicensedTail({ code: '01/07' }), ' · 01/07 · Ofcom record');
eq('Ofcom row named', dabLicensedTail({ code: '01/07', src: 'Ofcom record' }), ' · 01/07 · Ofcom record');
eq('BAKOM row: no code', dabLicensedTail({ code: '', src: 'BAKOM record' }), ' · BAKOM record');
eq('ČTÚ row', dabLicensedTail({ code: '', src: 'ČTÚ record' }), ' · ČTÚ record');
eq('owner list with a code', dabLicensedTail({ code: '0C/03', src: 'your list' }), ' · 0C/03 · your list');

// ── parsing keeps `src`, cleans it, and leaves it absent when the server sent none ──
const base = { type: 'dab', on: 1, eid: 0x4FFE, services: [] };
const p1 = parseDabMessage({ ...base, licensed: [{ site: 'Zuerich Uetliberg', area: 'SRG D01', code: '', km: 3.2, src: 'BAKOM record' }] });
eq('src carried through', p1.licensed?.[0]?.src, 'BAKOM record');
eq('empty code carried through', p1.licensed?.[0]?.code, '');
const p2 = parseDabMessage({ ...base, licensed: [{ site: 'Wrotham', area: 'Kent', code: '01/07', km: 10 }] });
eq('no src from an older server', p2.licensed?.[0]?.src, undefined);
eq('…which reads as Ofcom', dabLicensedTail(p2.licensed![0]), ' · 01/07 · Ofcom record');
const p3 = parseDabMessage({ ...base, licensed: [{ site: 'X', area: 'Y', code: '', km: 1, src: 'A'.repeat(200) }] });
eq('src is bounded', (p3.licensed?.[0]?.src ?? '').length <= 24, true);

// ── ★★ 2026-11-25: the regulator's ERP and height, in words that say what the height is measured from ──
eq('Daventry 12B (Ofcom)', dabTxSpecsTail({ erpKw: 9.5499, htM: 106, htRef: 'agl' }), ' · 9.5 kW · aerial 106 m above ground');
eq('Northampton 12B', dabTxSpecsTail({ erpKw: 2, htM: 30, htRef: 'agl' }), ' · 2 kW · aerial 30 m above ground');
eq('Geddington 12B', dabTxSpecsTail({ erpKw: 5, htM: 49, htRef: 'agl' }), ' · 5 kW · aerial 49 m above ground');
eq('ČTÚ: watts and the site above sea level', dabTxSpecsTail({ erpKw: 0.316, htM: 402, htRef: 'asl' }), ' · 316 W · site 402 m above sea level');
eq('RDI: effective height', dabTxSpecsTail({ erpKw: 3.16, htM: 332, htRef: 'eff' }), ' · 3.2 kW · effective height 332 m');
eq('BAKOM: power alone', dabTxSpecsTail({ erpKw: 0.27 }), ' · 270 W');
eq('big site rounds to whole kW', dabTxPowerText(12.6), '13 kW');
eq('an older server: nothing', dabTxSpecsTail({}), '');
eq('a height with no known reference is not shown', dabTxHeightText(106, 'xyz'), '');
eq('nonsense power is not shown', dabTxPowerText(Number.NaN), '');
eq('negative height is not shown', dabTxHeightText(-5, 'agl'), '');
const p4 = parseDabMessage({ ...base,
  tii: [{ main: 0x17, sub: 2, db: 24, site: 'Daventry', area: 'Northants', km: 19, erpKw: 9.5499, htM: 106, htRef: 'agl' }],
  licensed: [{ site: 'Daventry', area: 'Northants', code: '17/02', km: 19, src: 'Ofcom record', erpKw: 9.5499, htM: 106, htRef: 'agl' }] });
eq('tii line keeps erpKw/htM/htRef', [p4.tii?.[0]?.erpKw, p4.tii?.[0]?.htM, p4.tii?.[0]?.htRef], [9.5499, 106, 'agl']);
eq('licensed site keeps them', dabTxSpecsTail(p4.licensed![0]) + dabLicensedTail(p4.licensed![0]),
   ' · 9.5 kW · aerial 106 m above ground · 17/02 · Ofcom record');
const p5 = parseDabMessage({ ...base, tii: [{ main: 1, sub: 1, db: 3, site: 'X', htM: 10, htRef: '<script>' }] });
eq('a hostile htRef shows no height', dabTxSpecsTail(p5.tii![0]), '');

console.log(`test_dab_licensed: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
