// test_aviation_app.ts — V12 aviation, the clients' half (src/services/aviation.ts): the datalink report parsed
// defensively (bounded, control characters out, bad numbers dropped), the bands list, the list line (never the body),
// the HFDL status line. The server's own shapes are checked end to end in scripts/test-server-avdata.mjs.
import { avAge, avAppsText, avCard, avDetail, avFix, avGainText, avLine, avRoute, avTrackItems, avTrackRows, hfdlStatusText, parseAvBands, parseAvReport, avRateText } from '../src/services/aviation';

let fail = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

const real = { type: 'avdata', band: 'vdl2', bandName: 'VDL2', hz: 136862500, dropped: 0, msgs: 33, gain: 43.4, gainAuto: 1,
  aircraft: [{ icao: '4CA97B', reg: 'EI-EST', flight: 'FR1234', label: 'H1', labelName: 'message to/from crew or ops', level: -29.8, n: 3, age: 4, via: 'VDL2 136.775' },
             { reg: 'G-JZHU', lat: 51.5, lon: -1.25, posAge: 10, level: -40, n: 1, age: 70, via: 'ACARS 131.725', text: 'POS N51' }] };
const r = parseAvReport(real);
ok(!!r && r.band === 'vdl2' && r.aircraft.length === 2 && r.msgs === 33 && r.gain === 43.4, 'a real report parses');
ok(!!r && avLine(r.aircraft[0]) === 'FR1234 · EI-EST · 4CA97B — message to/from crew or ops', `the line: ${r && avLine(r.aircraft[0])}`);
ok(!!r && !avLine(r.aircraft[1]).includes('POS N51') && r.aircraft[1].text === 'POS N51', 'the body is its own field, never in the line');
ok(!!r && r.aircraft[1].lat === 51.5 && r.aircraft[1].lon === -1.25, 'a position kept');
ok(parseAvReport({ type: 'ais', aircraft: [] }) === null && parseAvReport(null) === null && parseAvReport({ type: 'avdata' }) === null, 'not a report: null');
const hostile = parseAvReport({ type: 'avdata', aircraft: [{ reg: 'A\u0000B\u001b[31m', lat: 999, lon: NaN, level: 'x', via: 5 }, 7, null] });
ok(!!hostile && hostile.aircraft.length === 1 && hostile.aircraft[0].reg === 'AB[31m' && hostile.aircraft[0].lat === undefined
   && hostile.aircraft[0].level === -120 && hostile.aircraft[0].via === '', `hostile fields cleaned: ${JSON.stringify(hostile && hostile.aircraft[0])}`);
const many = parseAvReport({ type: 'avdata', aircraft: Array.from({ length: 5000 }, () => ({ reg: 'X', level: 0, n: 1, age: 0, via: '' })) });
ok(!!many && many.aircraft.length === 2000, 'bounded at 2000 rows (the server\'s own table limit)');
ok(avLine({ level: 0, n: 0, age: 0, via: '' }) === 'unknown aircraft', 'nothing known: says so');
const b = parseAvBands([{ id: 'acars', name: 'ACARS', region: 'Europe', hz: 131675000, channels: 4 }, { id: 'vdl2' }, 'x']);
ok(b.length === 1 && b[0].id === 'acars' && b[0].channels === 4, 'bands: a malformed one dropped');
ok(hfdlStatusText(null) === 'listening for HFDL' && hfdlStatusText({ msgs: 1, bursts: 3 }) === '1 message · 3 bursts heard', 'the HFDL status line');
ok(avAge(4) === '4 s ago' && avAge(130) === '2 min ago' && avAge(7300) === '2 h ago', 'ages in words');

// ★ V12 2026-11-16 the ACARS applications' fields (vibe_acars_apps.h → Tracker::json)
const apps = parseAvReport({ type: 'avdata', band: 'vdl2', bandName: 'VDL2', hz: 136862500, msgs: 9, dropped: 0,
  apps: { pos: 2, oooi: 1, adsc: 1, cpdlc: 1, unparsed: 4 },
  aircraft: [
    { icao: '3C6548', reg: 'D-AIJH', flight: 'LH4567', lat: 51.75, lon: -0.5, posAge: 3, alt: 36000, posSrc: 'ADS-C', posTime: ':20:34', app: 'ADS-C', label: 'B6', level: -30, n: 4, age: 3, via: 'VDL2 136.875' },
    { reg: 'G-EZWK', flight: 'U22468', from: 'EGLL', to: 'LEMD', oooi: 'out', oooiTime: '10:15', app: 'OOOI', label: 'QP', level: -30, n: 1, age: 9, via: 'VDL2 136.975' },
    { reg: 'EI-EST', lat: 53.357, lon: -2.273, alt: 37000, posSrc: 'FMS', posTime: '21:20:00', level: -30, n: 1, age: 1, via: 'ACARS 131.725' },
    { reg: 'X', from: 'egll', to: 'LEMD<script>', oooi: 'sideways', alt: 1e9, level: 0, n: 0, age: 0, via: '' },
  ] });
ok(!!apps && apps.apps?.pos === 2 && apps.apps?.cpdlc === 1, 'the apps counters parse');
ok(!!apps && avFix(apps.aircraft[0]) === '36,000 ft · fix at :20:34 past the hour · ADS-C', `ADS-C fix line: ${apps && avFix(apps.aircraft[0])}`);
ok(!!apps && avFix(apps.aircraft[2]) === '37,000 ft · fix 21:20:00 UTC · FMS', `FMS fix line: ${apps && avFix(apps.aircraft[2])}`);
ok(!!apps && avRoute(apps.aircraft[1]) === 'EGLL → LEMD · OUT 10:15 UTC' && avFix(apps.aircraft[1]) === '', `route + OOOI: ${apps && avRoute(apps.aircraft[1])}`);
ok(!!apps && avDetail(apps.aircraft[1]).startsWith('EGLL → LEMD') && avDetail(apps.aircraft[1]).endsWith('VDL2 136.975'), `detail: ${apps && avDetail(apps.aircraft[1])}`);
const bad = apps && apps.aircraft[3];
ok(!!bad && bad.from === undefined && bad.to === undefined && bad.oooi === undefined && bad.alt === undefined, `hostile app fields dropped: ${JSON.stringify(bad)}`);
ok(!!apps && avAppsText(apps) === '3 position messages · 1 OOOI · 1 CPDLC', `status tail: ${apps && avAppsText(apps)}`);
const items = avTrackItems(apps);
ok(items.length === 2 && items[0].key === '3C6548' && items[0]._lb === 'LH4567' && items[0]._cd.includes('36,000 ft'), `map items: ${items.map((i) => i.key).join(',')}`);
ok(!avCard({ reg: '<b>x', level: 0, n: 0, age: 0, via: '<img>' }).includes('<img>'), 'the card is escaped');
const rows = avTrackRows(apps, { lat: 51.5, lon: -0.12 });
ok(rows.length === 4 && rows[0].sp === 'FL360' && / NM$/.test(rows[0].range) && rows[1].range === 'no position' && rows[1].noPos, `map rows: ${rows.map((r) => `${r.key}:${r.sp}:${r.range}`).join(' ')}`);

// ★★ V12 2026-11-24 the Airspy HF+ (no gain figure: its own AGC / attenuator named) and ACARS + VDL2 at a raised rate
const hf = parseAvReport({ type: 'avdata', band: 'vdl2', bandName: 'VDL2', hz: 136862500, msgs: 2, dropped: 0, aircraft: [],
  gainNote: 'HF+ AGC \u00b7 attenuator 12 dB' });
ok(!!hf && hf.gain === undefined && avGainText(hf) === ' · HF+ AGC · attenuator 12 dB', `HF+ status: "${hf && avGainText(hf)}"`);
ok(!!r && avGainText(r) === ' · gain 43.4 dB auto', `an RTL's status unchanged: "${r && avGainText(r)}"`);
const wide = parseAvReport({ type: 'avdata', band: 'acars+vdl2', bandName: 'ACARS + VDL2', hz: 134250000, msgs: 0, dropped: 0, aircraft: [], gain: 30, gainAuto: 1,
  rateNote: 'capture raised to 8 MS/s for ACARS + VDL2 \u2014 back to 2 MS/s when you leave' });
ok(!!wide && wide.band === 'acars+vdl2' && wide.bandName === 'ACARS + VDL2', 'the combined band\'s id and name survive the parser\'s bounds');
ok(!!wide && avGainText(wide).endsWith(' · capture raised to 8 MS/s for ACARS + VDL2 \u2014 back to 2 MS/s when you leave'), `the rate note: "${wide && avGainText(wide)}"`);
const b3 = parseAvBands([{ id: 'acars', name: 'ACARS', region: 'x', hz: 131675000, channels: 4 }, { id: 'vdl2', name: 'VDL2', region: 'y', hz: 136862500, channels: 12 },
                         { id: 'acars+vdl2', name: 'ACARS + VDL2', region: 'z', hz: 134250000, channels: 16 }]);
ok(b3.length === 3 && b3[2].id === 'acars+vdl2' && b3[2].channels === 16, 'three bands: the third is the combined one');
// ★★ beta96 messages per minute (Stuart 11-26: "like ais … check antenna performance quickly")
{
  const r1 = parseAvReport({ type: 'avdata', band: 'vdl2', bandName: 'VDL2', hz: 136862500, msgs: 400, msgRate: 23.4, aircraft: [] });
  ok(r1?.msgRate === 23.4 && avRateText(r1) === ' · 23 msg/min', `the rate parsed and worded as AIS's: "${avRateText(r1)}"`);
  const r2 = parseAvReport({ type: 'avdata', band: 'acars', bandName: 'ACARS', hz: 131725000, msgs: 3, msgRate: 2.5, aircraft: [] });
  ok(avRateText(r2) === ' · 2.5 msg/min', 'under 10: one decimal');
  const r3 = parseAvReport({ type: 'avdata', band: 'acars', bandName: 'ACARS', hz: 131725000, msgs: 3, aircraft: [] });
  ok(avRateText(r3) === '', 'an older server (no msgRate): nothing said, never "0 msg/min"');
}
console.log(fail ? `${fail} FAILED` : 'all passed');
process.exit(fail ? 1 : 0);
