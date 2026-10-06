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
import { dabLicensedTail, parseDabMessage } from '../src/services/dabTypes.ts';

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

console.log(`test_dab_licensed: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
