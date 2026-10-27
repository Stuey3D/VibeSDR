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
    { key: 'd6', kind: 'meter', make: 'Kamstrup', model: 'Cold water meter', name: 'Cold water meter · Kamstrup · 87654321', id: '87654321',
      line: 'Cold water meter · Kamstrup · 87654321 · v0x1B · 123.456 m³ · flow 0.012 m³/h · 30 Sep 120.111 m³ · no alarms · every 16 s · signal 18 dB',
      extra: 'v0x1B · 123.456 m³ · flow 0.012 m³/h · 30 Sep 120.111 m³ · no alarms · every 16 s', mfr: 'KAM', wm: 'C1-A', ver: 27, devType: 22,
      sec: 0, secText: 'not encrypted', acc: 12, sts: 0, every: 16, rx: 92, vol: 123.456, flow: 0.012,
      records: [{ q: 'volume', v: 123.456, u: 'm³', st: 0, fn: 'inst' }, { q: 'date', t: '2026-09-30', st: 1, fn: 'inst' }, { q: 'nonsense' , st: 'x' }, 7],
      freq: 868_950_000, snr: 18, seen: 2, count: 3 },
    { key: 'd7', kind: 'meter', make: 'Sensus', model: 'Water meter', name: 'Water meter · Sensus · 12345678', id: '12345678',
      line: 'Water meter · Sensus · 12345678 · v0x68 · encrypted (AES) · every 16 s · signal 24 dB', mfr: 'SEN', wm: 'T1', ver: 104, devType: 7,
      sec: 5, secText: 'encrypted (AES)', encBlocks: 2, encLen: 32, acc: 42, sts: 4, batt: 0, freq: 868_951_000, snr: 24, seen: 9, count: 12 },
    { kind: 'sensor', name: 'no line — dropped' },
    'not an object',
  ],
};

const w = web.ismSanitise(report), a = app.ismSanitise(report);
eq('the same report through both sanitisers', a, w);
eq('a non-device is dropped', a?.devices.length, 7);
eq('not a report', app.ismSanitise({ type: 'adsb' }), null);

for (const u of [{ mph: false, pressure: 'psi' }, { mph: true, pressure: 'kpa' }, { mph: true, pressure: 'bar' }, { mph: false, pressure: 'bar' }] as const) {
  for (const d of a!.devices) eq(`line ${d.key} in ${u.mph ? 'mph' : 'km/h'} / ${u.pressure}`, app.ismLine(d, u), web.ismLine(d as any, u));
}
for (const s of [-1, 0, 1.9, 2, 59, 61, 3599, 3600, 3900, 7200]) eq(`ago ${s}`, app.ismAgo(s), web.ismAgo(s));
eq('status', app.ismStatus(a!), web.ismStatus(w!));
// ★ V12 meters: the detail, the line in every unit (none applies), the status's meter count and the 0.9 MS/s gap
for (const d of a!.devices.filter((x) => x.kind === 'meter')) eq(`meter detail ${d.key}`, app.ismMeterDetail(d), web.ismMeterDetail(d as any));
eq('a meter line ignores units', app.ismLine(a!.devices[5], { mph: true, pressure: 'bar' }), a!.devices[5].line);
eq('meter detail', app.ismMeterDetail(a!.devices[6]), ['T1 · 868.951 MHz · 24 dB over the noise · access number 42', 'encrypted (AES) · 2 blocks · 32 bytes']);
eq('meter detail with readings', app.ismMeterDetail(a!.devices[5]),
   ['C1-A · 868.950 MHz · 18 dB over the noise · heard 92 % · access number 12', 'not encrypted', 'volume: 123.456 m³', 'date: 2026-09-30 (stored 1)', 'nonsense: — ']
     .map((x) => x.trimEnd()));
const r868 = { ...report, band: '868', bandName: '868 MHz SRD', rate: 900_000, meters: 2, wmGap: 1 };
eq('status: meters and the 0.9 MS/s gap', app.ismStatus(app.ismSanitise(r868)!), web.ismStatus(web.ismSanitise(r868)!));
eq('status words', app.ismStatus(app.ismSanitise(r868)!).includes('2 meters · 868.95 not covered at 0.9 MS/s'), true);
eq('the meter filter', app.ismRows(a, 'meter').map((d) => d.key), ['d6', 'd7']);
eq('the empty meter line', app.ismEmptyText('meter'), web.ismEmptyText('meter'));
eq('ages tick on between reports', app.ismAged(a!, 4).devices.map((d) => d.seen), web.ismAged(w!, 4).devices.map((d) => d.seen));
eq('…by the seconds since the report', app.ismAged(a!, 4).devices[0].seen, a!.devices[0].seen + 4);
eq('…and not at all at 0', app.ismAged(a!, 0), a);
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

// ── ★ V12 LoRa (docs/v12/LORA-MESH.md): the mesh / LoRaWAN rows, the public messages, the MAP's items — both readers ──
{
  const lr = {
    type: 'ism', band: '868', bandName: '868 MHz SRD', centre: 868_600_000, bursts: 10,
    lora: { chans: 13, plan: 16, dets: 42, frames: 31, badCrc: 1, texts: 2, mapped: 2, dropped: 0 },
    msgs: [{ ago: 14, net: 'meshcore', from: 'Alice', node: '', ch: 'Public', freq: 869_617_000, text: 'anyone on the mesh tonight?' },
           { ago: 40, net: 'meshtastic', from: '!55667788', node: '!55667788', ch: 'LongFast', freq: 869_521_000, text: 'Hello <b>Racecourse</b>' },
           { ago: 'x', text: 5 }],
    devices: [
      { key: 'm1', kind: 'meshtastic', make: 'Meshtastic', model: 'Heltec V3', name: 'Northampton Base (NBAS)',
        line: 'Meshtastic · Northampton Base (NBAS) · Heltec V3 · router · 52.2400, -0.8950 · 1 hop', node: '!1a2b3c4d', short: 'NBAS',
        long: 'Northampton Base', hw: 'Heltec V3', role: 'router', chan: 'LongFast', lat: 52.24, lon: -0.895, alt: 68, prec: 32, hops: 1,
        battPct: 87, sf: 11, bw: 250000, cr: 1, sync: '2B', freq: 869_527_000, snr: 9.8, seen: 20, count: 2,
        msgs: [{ ago: 12, from: 'Northampton Base', text: 'net check' }] },
      { key: 'm2', kind: 'meshcore', make: 'MeshCore', model: 'repeater', name: 'Moulton Rptr', line: 'MeshCore · Moulton Rptr · repeater · 52.2900, -0.8400 · heard first-hand',
        node: '5a5b5c5d', long: 'Moulton Rptr', role: 'repeater', lat: 52.29, lon: -0.84, hops: 0, sf: 8, bw: 62500, cr: 4, freq: 869_619_000, snr: 9.5, seen: 1200, count: 3 },
      { key: 'm3', kind: 'lorawan', make: 'LoRaWAN', model: 'Unconfirmed up', name: 'TTN 260B1234',
        line: 'LoRaWAN · TTN 260B1234 · Unconfirmed up · FCnt 4211 · port 2 · 12 bytes encrypted · ADR · SF9', node: '260B1234', net: 'The Things Network',
        fcnt: 4211, fport: 2, encLen: 12, sf: 9, bw: 125000, freq: 868_103_000, snr: 9.2, seen: 5, count: 1 },
      { key: 'm4', kind: 'meshtastic', name: '!99aabbcc', line: 'Meshtastic · !99aabbcc · heard first-hand · private channel', node: '!99aabbcc', priv: 1,
        lat: 0, lon: 0, freq: 869_526_000, snr: 9.7, seen: 30, count: 1 },
      { key: 'm5', kind: 'lora', name: 'LoRa SF9', line: 'LoRa · SF9 · 125 kHz · sync 0x12 · 20 bytes', sf: 9, freq: 868_300_000, snr: 3, seen: 40, count: 1 },
    ],
  };
  const w = web.ismSanitise(lr)!, a2 = app.ismSanitise(lr)!;
  eq('LoRa: both sanitisers agree', a2, w);
  eq('the lora block', w.lora, lr.lora);
  eq('messages: the broken one dropped, text kept as text', w.msgs!.map((m) => m.text), ['anyone on the mesh tonight?', 'Hello <b>Racecourse</b>']);
  eq('0,0 is not a position', w.devices.find((d) => d.key === 'm4')!.lat, undefined);
  eq('two on the map', w.devices.filter(web.ismOnMap).map((d) => d.key), ['m1', 'm2']);
  eq('app: the same two', a2.devices.filter(app.ismOnMap).map((d) => d.key), ['m1', 'm2']);
  eq('the status line, both', app.ismStatus(a2), web.ismStatus(w));
  eq('…which counts the LoRa frames and channels', /LoRa 31 frames · 13 of 16 channels/.test(web.ismStatus(w)), true);
  const u = { mph: true, pressure: 'bar' as const };
  for (const d of w.devices) eq(`a LoRa line has no unit choice: ${d.key}`, web.ismLine(d, u), d.line);
  const rx = { lat: 52.2405, lon: -0.9025 };
  for (const d of w.devices) eq(`the detail, both: ${d.key}`, app.ismLoraDetail(a2.devices.find((x) => x.key === d.key)!, rx), web.ismLoraDetail(d, rx));
  const det = web.ismLoraDetail(w.devices[0], rx).join(' | ');
  eq('a node\'s detail: SF, hops, hardware, battery, range', /SF11 · 250 kHz · CR 4\/5 · SNR 9.8 dB · 1 hop/.test(det) && /Heltec V3 · router · channel LongFast · battery 87 %/.test(det)
     && /km [NESW]/.test(det) && /net check/.test(det), true);
  eq('LoRaWAN detail says encrypted, never bytes', web.ismLoraDetail(w.devices[2], rx).some((l) => /never its bytes/.test(l)), true);
  eq('LORAWAN chip shows unnamed LoRa too', app.ismRows(a2, 'lorawan').map((d) => d.key), ['m3', 'm5']);
  eq('MESHTASTIC chip', app.ismRows(a2, 'meshtastic').map((d) => d.key), ['m1', 'm4']);
  eq('MESSAGES chip: messages, not rows', app.ismRows(a2, 'msgs'), []);
  eq('the messages, app lines', app.ismMsgLines(a2).map((m) => m.text), ['MeshCore Public · Alice: anyone on the mesh tonight?', 'Meshtastic LongFast · !55667788: Hello <b>Racecourse</b>']);
  eq('the web\'s messages escape what came off the air', /Hello &lt;b&gt;Racecourse&lt;\/b&gt;/.test(web.ismListHtml(w, 'msgs', { mph: false, pressure: 'psi' })), true);
  eq('MESSAGES empty text, both', app.ismEmptyText('msgs'), web.ismEmptyText('msgs'));
  eq('the map status, both', app.ismMapStatus(a2, rx), web.ismMapStatus(w, rx));
  eq('…furthest named', /2 on the map · furthest Moulton Rptr \d/.test(web.ismMapStatus(w, rx)), true);
  eq('the map icon, both', app.ismMapIcon(a2.devices[0]), web.ismMapIcon(w.devices[0]));
  eq('the map card escapes', /&lt;|<b>Northampton Base<\/b>/.test(app.ismMapCard(a2.devices[0], rx)), true);
  const items = app.ismMapItems(a2, rx);
  eq('the app\'s pins', items.map((i) => [i.key, i.lat, i.lon, i._lb]), [['m1', 52.24, -0.895, 'NBAS'], ['m2', 52.29, -0.84, 'Moulton Rptr']]);
  eq('the app\'s rows nearest first', app.ismMapRows(a2, rx).map((r) => r.key), ['m1', 'm2']);
  eq('the MAP key is a stop only when offered', app.ismNavKeys({ on: true, big: false, chips: false, rows: 0, map: true }).map((k) => k.key), ['down', 'up', 'map', 'exit']);
  eq('…and not otherwise', app.ismNavKeys({ on: true, big: false, chips: false, rows: 0 }).map((k) => k.key), ['down', 'up', 'exit']);
  eq('the chips, both', app.ISM_KINDS, web.ISM_KINDS);
}

console.log(`ism app: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
