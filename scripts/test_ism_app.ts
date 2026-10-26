/**
 * The app's ISM (src/services/ism.ts) against the web client's (web/client/src/ism.ts) — ONE RULE, TWO READERS
 * (2026-10-25): the same reports through both sanitisers, lines in every unit, ages, status lines, the filter (KEY FOB
 * includes suspected fobs), the capability and the band neighbours.
 *
 * Run: npx tsx scripts/test_ism_app.ts   (run-tests.sh does)
 */
import * as web from '../web/client/src/ism.ts';
import * as app from '../src/services/ism.ts';
import { needleGeometry } from '../src/utils/needleGeometry.ts';
import { readFileSync } from 'node:fs';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};

const report = {
  type: 'ism', band: '433', bandName: '433.92 MHz ISM', centre: 433_920_000, custom: 0, bursts: 41, unknown: 3, gain: 33.8, gainAuto: 1,
  capture: 100,
  devices: [
    { key: 'd1', kind: 'weather', make: 'Oregon', model: 'THGR122NX', name: 'Oregon THGR122NX', line: 'Oregon THGR122NX · 12.4 °C · 81 % RH · ch 2',
      temp: 12.43, hum: 81, ch: 2, wind: 18, gust: 29.4, dir: 250, rain: 3.25, rainRate: 1.2, batt: 1, freq: 433_910_000, snr: 18, seen: 4, count: 9 },
    { key: 'd2', kind: 'tpms', make: '', model: 'type A', name: 'TPMS #2', line: 'TPMS #2 · type A · 33 PSI · 18 °C', kpa: 228, temp: 18.4, nick: 2,
      freq: 433_920_000, snr: 11, seen: 75, count: 2 },
    { key: 'd3', kind: 'suspectfob', make: '', model: '', name: 'Suspected key fob #1', line: 'Suspected key fob #1 · button press',
      freq: 434_134_000, snr: 14, seen: 3700, count: 1 },
    { key: 'd4', kind: 'keyfob', make: 'KeeLoq', model: '', name: 'Key fob #3', line: 'Key fob #3 · KeeLoq · button 2', button: '2',
      freq: 433_920_000, snr: 20, seen: 1, count: 4 },
    { key: 'd5', kind: 'energy', name: 'Owl CM180', line: 'Owl CM180 · 1.23 kW', watts: 1234, freq: 433_900_000, seen: 30 },
    { kind: 'sensor', name: 'no line — dropped' },
    'not an object',
  ],
};

const w = web.ismSanitise(report), a = app.ismSanitise(report);
eq('the same report through both sanitisers', a, w);
eq('a non-device is dropped', a?.devices.length, 5);
eq('not a report', app.ismSanitise({ type: 'adsb' }), null);

for (const u of [{ mph: false, pressure: 'psi' }, { mph: true, pressure: 'kpa' }, { mph: true, pressure: 'bar' }, { mph: false, pressure: 'bar' }] as const) {
  for (const d of a!.devices) eq(`line ${d.key} in ${u.mph ? 'mph' : 'km/h'} / ${u.pressure}`, app.ismLine(d, u), web.ismLine(d as any, u));
}
for (const s of [-1, 0, 1.9, 2, 59, 61, 3599, 3600, 3900, 7200]) eq(`ago ${s}`, app.ismAgo(s), web.ismAgo(s));
eq('status', app.ismStatus(a!), web.ismStatus(w!));
const custom = app.ismSanitise({ ...report, custom: 1, centre: 433_420_000 })!;
eq('status on an exact frequency', app.ismStatus(custom), web.ismStatus(web.ismSanitise({ ...report, custom: 1, centre: 433_420_000 })!));
eq('band text on an exact frequency', app.ismBandText(custom), '433.420 MHz');
eq('kinds offered', app.ISM_KINDS, web.ISM_KINDS);

// ★ The filter — the web draws rows from ismListHtml; the same rows in the same order (data-key) here.
for (const k of app.ISM_KINDS) {
  const html = web.ismListHtml(w!, k.id, app.ISM_UNITS_DEFAULT);
  const webKeys = [...html.matchAll(/data-key="([^"]*)"/g)].map((m) => m[1]);
  eq(`filter ${k.id}: the web's rows`, app.ismRows(a, k.id).map((d) => d.key), webKeys);
  if (!webKeys.length) eq(`filter ${k.id}: the empty line`, html.includes(app.ismEmptyText(k.id)), true);
}
eq('KEY FOB shows the suspected ones', app.ismRows(a, 'keyfob').map((d) => d.kind), ['suspectfob', 'keyfob']);

// ── The capability ──
eq('no field: no control', app.parseIsmCapability({ dab: true }), undefined);
eq('true without bands: no control', app.parseIsmCapability({ ism: true }), undefined);
const cap = app.parseIsmCapability({ ism: true, ismBands: [{ id: '433', name: '433.92 MHz ISM', hz: 433_920_000 },
  { id: '868', name: '868 MHz SRD', hz: 868_600_000 }, { id: 9 }], ismLanding: '433' });
eq('live, the bands this radio reaches', cap, { ok: true, why: '', bands: [{ id: '433', name: '433.92 MHz ISM', hz: 433_920_000 },
  { id: '868', name: '868 MHz SRD', hz: 868_600_000 }], landing: '433' });
eq('refused: greyed with the reason', app.parseIsmCapability({ ism: false, ismWhy: 'Locked centre on a shared dial', ismBands: [{ id: '433', name: 'x', hz: 1 }] }),
   { ok: false, why: 'Locked centre on a shared dial', bands: [{ id: '433', name: 'x', hz: 1 }], landing: '433' });
eq('neighbours at the bottom', app.ismNeighbours(cap, a).prev, null);
eq('neighbours: up is 868', app.ismNeighbours(cap, a).next?.id, '868');
eq('neighbours: unknown band', app.ismNeighbours(cap, { ...a!, band: 'zz' }), { prev: null, next: null });

// ── The listener's choices ──
eq('prefs default', app.parseIsmPrefs(null), { filter: 'all', units: { mph: false, pressure: 'psi' } });
eq('prefs kept', app.parseIsmPrefs('{"filter":"tpms","mph":true,"pressure":"bar"}'), { filter: 'tpms', units: { mph: true, pressure: 'bar' } });
eq('prefs junk', app.parseIsmPrefs('{"filter":"evil","pressure":"atm"}'), { filter: 'all', units: { mph: false, pressure: 'psi' } });
eq('pressure cycles psi → kPa → bar → psi', [app.nextPressure('psi'), app.nextPressure('kpa'), app.nextPressure('bar')], ['kpa', 'bar', 'psi']);

// ── The box by pad and keyboard: every key and chip is a stop, in the order drawn, on its visual row ──
{
  const k = app.ismNavKeys({ on: true, big: true, chips: true, rows: 3 });
  eq('the stops, in order', k.map((x) => x.key), ['down', 'up', 'big', 'exit', ...app.ISM_KINDS.map((c) => `k:${c.id}`), 'u:speed', 'u:pressure', 'r:0', 'r:1', 'r:2']);
  eq('rows: header 0, chips 1, the list 2…', k.map((x) => x.row), [0, 0, 0, 0, ...app.ISM_KINDS.map(() => 1), 1, 1, 2, 3, 4]);
  eq('after a refusal: CLOSE only (no arrows, no chips)', app.ismNavKeys({ on: false, big: false, chips: false, rows: 0 }).map((x) => x.key), ['exit']);
  eq('every chip drawn is a stop', app.ismNavKeys({ on: true, big: false, chips: true, rows: 0 }).filter((x) => x.row === 1).length, app.ISM_KINDS.length + 2);
  const panel = readFileSync(new URL('../src/components/IsmPanel.tsx', import.meta.url), 'utf8');
  eq('the panel walks ismNavKeys with useListNav', /ismNavKeys\(/.test(panel) && /useListNav\(/.test(panel), true);
  eq('the screen highlight can land on the box', panel.includes("screenNavRef('ismBox'"), true);
  for (const c of ['k:${k.id}', 'u:speed', 'u:pressure', 'down', 'up', 'exit', 'big'])
    eq(`the ${c} key draws the highlight`, panel.includes(`hl(${c.startsWith('k:') ? '`' + c + '`' : "'" + c + "'"})`), true);
}

// ── No LISTEN needle in ISM (the web's wf.vfoHidden) ──
{
  const base = { bwHz: 2_400_000, tuneHz: 433_920_000, filterLow: -5000, filterHigh: 5000, width: 400, pxPerHz: 400 / 2.4e6,
                 hzToX: (hz: number) => ((hz - 432_720_000) / 2.4e6) * 400 };
  const n = needleGeometry(base);
  eq('the needle at the dial', n && Math.round(n.nX), 200);
  eq('hidden: no needle, no passband, no label', needleGeometry({ ...base, hidden: true }), null);
  const wv = readFileSync(new URL('../src/components/WaterfallView.tsx', import.meta.url), 'utf8');
  eq('WaterfallView takes its needle from needleGeometry with needleHidden', /needleGeometry\(\{ hidden: needleHidden/.test(wv), true);
  // ★ every renderer path reads `needle` — the needle canvas (iOS), the in-waterfall group (Android), the LISTEN label
  eq('no renderer places the needle from tuneHz itself', (wv.match(/hzToX\(tuneHz\)/g) ?? []).length, 0);
  eq('the LISTEN label is behind `needle &&`', /\{needle && showCenterMarker && \(/.test(wv), true);
  const scr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
  eq('the receiver screen hides it while ISM is on', /needleHidden=\{ismOn\}/.test(scr), true);
  eq('one waterfall (phone, iPad and Mac share it)', (scr.match(/<WaterfallView\b/g) ?? []).length, 1);
}

console.log(`ism app: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
