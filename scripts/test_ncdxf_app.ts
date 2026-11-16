// test_ncdxf_app.ts — V12 2026-11-16 the NCDXF / IARU beacon tracker's client half (src/services/ncdxf.ts — shared by the
// app and the web client): the server's report sanitised, the grid rows, the status line, and the keys wired in both
// clients (only where the server offers "ncdxf"). Run: npx tsx scripts/test_ncdxf_app.ts
import { readFileSync } from 'node:fs';
import { ncdxfSanitise, ncdxfRows, ncdxfStatus, ncdxfStepColour, ncdxfCellView, NCDXF_BANDS, NCDXF_HINT } from '../src/services/ncdxf';
import { decoderNear } from '../src/utils/decoderBands';

let pass = 0, fail = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) pass++; else { fail++; console.log(`FAIL ${what}\n   got  ${g}\n   want ${w}`); }
};
const ok = (what: string, c: boolean) => eq(what, c, true);

// ★ the server's shape (vibe_ncdxf_run.h sendJson)
const beacons = [['4U1UN', 'United Nations, New York', 40.75, -73.97], ['VE8AT', 'Eureka, Nunavut', 79.99, -85.93], ['W6WX', 'Mt Umunhum, California', 37.16, -121.9],
  ['KH6RS', 'Maui, Hawaii', 20.75, -156.43], ['ZL6B', 'Masterton, New Zealand', -41.07, 175.65], ['VK6RBP', 'Rolystone, Australia', -32.1, 116.05],
  ['JA2IGY', 'Mt Asama, Japan', 34.45, 136.78], ['RR9O', 'Novosibirsk, Russia', 54.98, 82.9], ['VR2B', 'Hong Kong', 22.27, 114.15],
  ['4S7B', 'Colombo, Sri Lanka', 6.9, 79.87], ['ZS6DN', 'Pretoria, South Africa', -25.9, 28.27], ['5Z4B', 'Kikuyu, Kenya', -1.25, 36.7],
  ['4X6TU', 'Tel Aviv, Israel', 32.05, 34.78], ['OH2B', 'Lohja, Finland', 60.3, 24], ['CS3B', 'Madeira', 32.8, -17],
  ['LU4AA', 'Buenos Aires, Argentina', -34.62, -58.37], ['OA4B', 'Lima, Peru', -12.07, -76.95], ['YV5B', 'Caracas, Venezuela', 9.1, -67.8]];
const msg = {
  now: 1794816330, band: 0, tone: 1000, slot: { beacon: 14, at: 1794816320 },
  last: { beacon: 13, band: 0, at: 1794816310, steps: 4, snr: [18.8, 15.3, 5.1, 7.2], id: 1, other: -1, lag: 0, off: 0 },
  beacons, bands: [14100000, 18110000, 21150000, 24930000, 28200000],
  cells: [[13, 0, 20, 4, 18.8, 1, 20, 4, 20], [14, 0, 10, 3, 14.9, 1, 10, 3, 10], [1, 0, 4000, 1, 6.4, 1, 4000, 1, 4000], [3, 0, -1, 0, -99, 0, 50, 0, -1],
          [99, 0, 1, 1, 1, 1, 1, 1, 1], 'junk', [2, 7, 1, 1, 1, 1, 1, 1, 1], [5, 1, 30, 9, 1, 1, 30, 9, 30]],
};
const r = ncdxfSanitise(msg)!;
ok('sanitised', !!r && r.beacons.length === 18 && r.beacons[13].call === 'OH2B');
eq('cells: bad beacon / band / junk dropped, steps clamped', r.cells.map((c) => [c.beacon, c.band, c.steps]), [[13, 0, 4], [14, 0, 3], [1, 0, 1], [3, 0, 0], [5, 1, 4]]);
eq('a report without its 18 beacons is refused', ncdxfSanitise({ ...msg, beacons: beacons.slice(0, 5) }), null);
eq('not an object', ncdxfSanitise('x'), null);
const rows = ncdxfRows(r);
eq('18 rows in the published order, 5 cells each', [rows.length, rows[0].call, rows[17].call, rows[0].cells.length], [18, '4U1UN', 'YV5B', 5]);
eq('OH2B on 20 m: 4 steps', [rows[13].cells[0].text, rows[13].cells[0].steps, rows[13].cells[0].faded], ['4', 4, false]);
ok('…its title says how far down', rows[13].cells[0].title.includes('down to 0.1 W') && rows[13].cells[0].title.includes('callsign checked'));
eq('CS3B on the air now: outlined', rows[14].cells[0].now, true);
eq('VE8AT heard over an hour ago: faded', rows[1].cells[0].faded, true);
eq('KH6RS tried, not heard: —', rows[3].cells[0].text, '—');
eq('never tried: blank', ncdxfCellView(r, 0, 4).text, '');
ok('the step colours run green to amber', ncdxfStepColour(4) !== ncdxfStepColour(1) && ncdxfStepColour(0) === 'transparent');
eq('status: on now + the last slot', ncdxfStatus(r), '20 m: CS3B (Madeira) on now · OH2B heard down to 0.1 W ✓');
eq('status: off a beacon frequency', ncdxfStatus({ ...r, band: -1 }).startsWith('not on a beacon frequency'), true);
eq('status: another beacon', ncdxfStatus({ ...r, last: { ...r.last!, id: -1, other: 8 } }).includes('sounded like VR2B, not OH2B'), true);
eq('status: late by 1.0 s', ncdxfStatus({ ...r, last: { ...r.last!, lag: 1.0 } }).includes('late by 1.0 s'), true);
eq('status before a report', ncdxfStatus(null), 'waiting for the beacons…');
eq('the five bands', NCDXF_BANDS.map((b) => b.hz), [14100000, 18110000, 21150000, 24930000, 28200000]);
ok('the hint says where to tune and in what', NCDXF_HINT.includes('14.100') && NCDXF_HINT.includes('CW or USB') && NCDXF_HINT.includes('0.1 W'));
ok('the key sits on 14.100 (decoder tabs)', decoderNear('ncdxf', 14.1e6) && !decoderNear('ncdxf', 7.1e6));

// ── both clients wired, and only where the server offers it ──
const src = (p: string) => readFileSync(new URL('../' + p, import.meta.url), 'utf8');
const html = src('web/client/index.html'), main = src('web/client/src/main.ts'), dec = src('web/client/src/decoders.ts');
ok('web: the BEACONS key, hidden until the server says so', /data-dec="ncdxf" hidden/.test(html));
ok('web: shown only where "ncdxf"', main.includes("ncdxfCap = j.ncdxf === true || !!decWhy.ncdxf"));
ok('web: the grid and the band keys', html.includes('id="ncdxfGrid"') && html.includes('id="ncdxfBands"') && main.includes('function renderNcdxf'));
ok('web: the JSON routed', dec.includes("msg.type === 'ncdxf'") && dec.includes("ncdxf: 'ncdxf'"));
const ms = src('src/components/ModeSelector.tsx'), sd = src('src/screens/SDRScreen.tsx'), dc = src('src/services/DecoderClient.ts');
ok('app: the key only where offered', ms.includes("const ncdxfShown = !!decoderControls?.ncdxfAvail && !isBlocked('ncdxf')") && sd.includes('ncdxfAvail: ncdxfCapable'));
ok('app: the bands this radio reaches', sd.includes('NCDXF_BANDS.filter((b) => canReach(b.hz))'));
ok('app: attach + JSON', dc.includes("case 'ncdxf':   return { extension_name: 'ncdxf', params: {} };") && dc.includes("m.type === 'ncdxf'"));
const about = src('src/components/AboutOverlay.tsx');
ok('About: the V12 note', about.includes('NCDXF'));

console.log(`${fail ? 'FAIL' : 'ok '}  test_ncdxf_app — ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
