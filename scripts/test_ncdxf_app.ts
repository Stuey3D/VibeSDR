// test_ncdxf_app.ts — V12 2026-11-16 the NCDXF / IARU beacon tracker's client half (src/services/ncdxf.ts — shared by the
// app and the web client): the server's report sanitised, the grid rows, the status line, and the keys wired in both
// clients (only where the server offers "ncdxf"). Run: npx tsx scripts/test_ncdxf_app.ts
import { readFileSync } from 'node:fs';
import { ncdxfSanitise, ncdxfRows, ncdxfStatus, ncdxfStepColour, ncdxfCellView, NCDXF_BANDS, NCDXF_HINT, NCDXF_CALLS, ncdxfSlot, ncdxfBandOf,
         ncdxfBeaconOn, ncdxfFollowBand, ncdxfFollowAllowed, ncdxfFollowTunedAway, NCDXF_FOLLOW_LEAD_S } from '../src/services/ncdxf';
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

// ── ★★ FOLLOW (Stuart 2026-11-16): one beacon up the bands, on the listener's own dial only ──
{
  const H = 1794816000;   // a multiple of 180: hh:mm:00 with mm % 3 = 0 — 4U1UN on 14.100
  eq('slot 0 at the cycle start, 17 at +170 s, wraps at 180', [ncdxfSlot(H), ncdxfSlot(H + 175), ncdxfSlot(H + 180)], [0, 17, 0]);
  eq('the published schedule: 4U1UN on 20 m in slot 0, 17 m in slot 1 … 10 m in slot 4, silent in slot 5',
     [0, 1, 2, 3, 4, 5].map((sl) => ncdxfBandOf(0, sl)), [0, 1, 2, 3, 4, -1]);
  eq('OH2B (13) on 20 m in slot 13; who is on 15 m in slot 15 (OH2B)', [ncdxfBandOf(13, 13), ncdxfBeaconOn(2, 15)], [0, 13]);
  eq('YV5B (17) wraps: 17 m in slot 0', ncdxfBandOf(17, 0), 1);
  const all = [true, true, true, true, true];
  // following 4U1UN through a cycle, one second into each slot: the band it is on, a moment early; while silent, back on 20 m
  const seen = []; for (let t = 0; t < 180; t += 10) seen.push(ncdxfFollowBand(0, H + t + 1, all));
  eq('FOLLOW 4U1UN: 20 17 15 12 10 m, then waits on 20 m for 13 slots', seen, [0, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
  eq('…tuned ' + NCDXF_FOLLOW_LEAD_S + ' s before the slot (at +9.6 s it is already on 17 m)', ncdxfFollowBand(0, H + 9.6, all), 1);
  eq('…and not earlier (at +9.4 s still 20 m)', ncdxfFollowBand(0, H + 9.4, all), 0);
  const noTwelve = [true, true, true, false, true];
  eq("a band the radio cannot reach is skipped: in 12 m's slot it waits on 10 m", ncdxfFollowBand(0, H + 30, noTwelve), 4);
  const tenOnly = [false, false, false, false, true];
  eq('only 10 m reachable: it waits there all cycle', [0, 50, 100, 170].map((t) => ncdxfFollowBand(0, H + t, tenOnly)), [4, 4, 4, 4]);
  eq('none reachable: -1 (FOLLOW is not offered)', ncdxfFollowBand(0, H, [false, false, false, false, false]), -1);
  eq('★ never a shared dial: exclusive yes, open / room / shared flag no, an old server (no dial word) yes',
     [ncdxfFollowAllowed('exclusive'), ncdxfFollowAllowed('open'), ncdxfFollowAllowed('room'), ncdxfFollowAllowed('exclusive', true), ncdxfFollowAllowed(undefined)],
     [true, false, false, false, true]);
  eq('a hand tune 2 kHz away stops it; the CW pitch nudge of 700 Hz does not', [ncdxfFollowTunedAway(14_102_000, 14_100_000), ncdxfFollowTunedAway(14_100_700, 14_100_000), ncdxfFollowTunedAway(14_100_000, 0)], [true, false, false]);
  eq("the eighteen calls, the tracker's order", [NCDXF_CALLS.length, NCDXF_CALLS[0], NCDXF_CALLS[13], NCDXF_CALLS[17]], [18, '4U1UN', 'OH2B', 'YV5B']);
  const tracker = src('android/app/src/main/cpp/decoders/ncdxf_tracker.h');
  ok('the calls match ncdxf_tracker.h', NCDXF_CALLS.every((c) => tracker.includes(`"${c}"`)));
  const web = src('web/client/src/main.ts'), html = src('web/client/index.html'), scr = src('src/screens/SDRScreen.tsx'), ms = src('src/components/ModeSelector.tsx');
  ok('web: the FOLLOW row, hidden unless allowed', html.includes('id="ncdxfFollowRow" hidden') && /ncdxfFollowAllowed\(dialModeNow, srvSharedDial\)/.test(web));
  ok('web: a dial turning shared re-checks (onDial) and leaving the tracker stops it', /dialModeNow = d\.mode;\n\s+ncdxfFollowSync\(\)/.test(web) && /else ncdxfFollowStop\(\)/.test(web));
  ok('app: offered only where allowed (ncdxfFollowAllowed on the dial\'s mode)', /ncdxfFollowAllowed\(dialStateRef\.current\?\.mode\)/.test(scr) && /ncdxfFollow: ncdxfCapable && ncdxfFollowMay\(\)/.test(scr));
  ok('app: the FOLLOW row under BAND', /<SubLabel label="FOLLOW" \/>/.test(ms));
  ok('both: tuned in CW, a band key stops FOLLOW', /decTune\(hz, 'cwu'/.test(web) && /ncdxfFollowStop\(\); decTune\(b\.hz/.test(web) && /setNcdxfFollow\(\(f\) => \(\{ \.\.\.f, on: false \}\)\); onTuneHz\(hz, 'cwu'/.test(scr));
}

console.log(`${fail ? 'FAIL' : 'ok '}  test_ncdxf_app — ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
