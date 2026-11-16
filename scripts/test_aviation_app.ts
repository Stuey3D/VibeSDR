// test_aviation_app.ts — V12 aviation, the clients' half (src/services/aviation.ts): the datalink report parsed
// defensively (bounded, control characters out, bad numbers dropped), the bands list, the list line (never the body),
// the HFDL status line. The server's own shapes are checked end to end in scripts/test-server-avdata.mjs.
import { avAge, avLine, hfdlStatusText, parseAvBands, parseAvReport } from '../src/services/aviation';

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
console.log(fail ? `${fail} FAILED` : 'all passed');
process.exit(fail ? 1 : 0);
