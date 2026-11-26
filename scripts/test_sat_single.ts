// test_sat_single.ts — ★★★ beta96 SINGLE MODE: SATELLITES, the clients' pure half (src/services/satSingle.ts — the web page and
// the app read the same rules; docs/v12/SAT-SINGLE.md): who watches, the messages parsed (and malformed ones refused), the
// header before AOS / during a pass / with nothing due / passes off, the line under it per decoder, the hybrid's words and
// window, the timetable rows, the last pass.
process.env.TZ = 'UTC';
import {
  parseSatHybrid, parseSatSingle, satHybridClamp, satHybridInWindow, satHybridText, satSingleHead, satSingleLastText, satSingleNext,
  satSinglePlanRows, satSingleSub, satSingleWatch, SAT_SINGLE_BETWEEN, SAT_SINGLE_ONLY, SAT_SINGLE_WHY,
} from '../src/services/satSingle';

let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };
const now = Date.UTC(2026, 10, 26, 18, 0, 0) / 1000;
const P = (norad: number, name: string, label: string, aosMin: number, durMin: number, maxEl: number, decoder?: string) =>
  ({ norad, tx: 0, name, label, aos: now + aosMin * 60, los: now + (aosMin + durMin) * 60, maxEl, ...(decoder ? { decoder } : {}) });
const HYB = { type: 'sat_hybrid', on: true, norad: 44909, tx: 0, name: 'RS-44', label: 'Transponder', mode: 'usb',
              lo: 435595737, hi: 435684263, passLo: 435610000, passHi: 435670000, centre: 435640000, los: now + 600 };

console.log('§1 who watches');
ok(satSingleWatch('sat', false), 'a listener on a SATELLITES radio watches');
ok(!satSingleWatch('sat', true), '…the signed-in admin never does');
ok(!satSingleWatch('adsb', false) && !satSingleWatch('', false) && !satSingleWatch(null, false) && !satSingleWatch(undefined, false), 'any other radio: no');
ok(/SATELLITES only/.test(SAT_SINGLE_ONLY) && /follows the satellite passes itself/.test(SAT_SINGLE_WHY), 'the note and the why');

console.log('§2 the messages');
const before = parseSatSingle({ type: 'sat_single', on: true, t: now, plan: [P(44909, 'RS-44', 'Transponder', 5, 20, 46), P(57166, 'Meteor-M2-3', 'LRPT', 40, 14, 62)] });
ok(!!before && before.plan.length === 2 && !before.now && !before.hybrid, 'a timetable, no pass now');
ok(parseSatSingle(null) === null && parseSatSingle('x') === null, 'not an object: refused');
const bad = parseSatSingle({ on: true, plan: [{ norad: 0, aos: 1, los: 2 }, { norad: 5, aos: 10, los: 5 }, 'x', P(1, 'A', '', 1, 2, 3)], now: { norad: -1 } });
ok(!!bad && bad.plan.length === 1 && !bad.now, 'malformed rows dropped (no norad, LOS before AOS, not an object), a bad `now` ignored');
ok(parseSatSingle({ plan: Array.from({ length: 30 }, (_, i) => P(1, 'A', '', i, 1, 10)) })!.plan.length === 12, 'at most 12 rows');
ok(parseSatSingle({ on: false, plan: [] })!.on === false && parseSatSingle({ plan: [] })!.on === true, '`on` false only when said');
ok(parseSatHybrid(HYB)?.lo === 435595737 && parseSatHybrid({ ...HYB, on: false }) === null && parseSatHybrid({ ...HYB, hi: 1 }) === null, 'sat_hybrid: on, off, a window upside down refused');
const withHyb = parseSatSingle({ on: true, plan: [], hybrid: { ...HYB, type: undefined } });
ok(withHyb?.hybrid?.centre === 435640000, 'the hybrid inside sat_single (no type of its own)');

console.log('§3 the header');
ok(satSingleHead(null, now) === 'Asking the receiver for its timetable…', 'nothing yet: asking');
ok(/not switched the satellite passes on/.test(satSingleHead(parseSatSingle({ on: false, plan: [] }), now)), 'passes off: said');
ok(satSingleHead(before, now) === 'NEXT · RS-44 · Transponder · AOS 18:05 (in 0:05) · max 46°', `before AOS: "${satSingleHead(before, now)}"`);
const during = parseSatSingle({ on: true, plan: [P(44909, 'RS-44', 'Transponder', -2, 20, 46)], now: P(44909, 'RS-44', 'Transponder', -2, 20, 46) });
ok(satSingleHead(during, now) === 'LIVE · RS-44 · Transponder · 18:00 left', `during: "${satSingleHead(during, now)}"`);
ok(satSingleNext(during, now)?.norad === 44909, 'the pass shown is the one running');
const over = parseSatSingle({ on: true, plan: [P(1, 'A', 'x', -30, 10, 20)], state: 'waiting — no pass for 3 h' });
ok(/^Waiting — waiting/.test(satSingleHead(over, now)) && satSingleNext(over, now) === null, `nothing to come: the server's words ("${satSingleHead(over, now)}")`);
ok(/next 24 hours/.test(satSingleHead(parseSatSingle({ on: true, plan: [] }), now)), 'nothing at all: next 24 hours');

console.log('§4 the line under it');
ok(satSingleSub(before, now) === SAT_SINGLE_BETWEEN, 'between passes: the timetable and the gallery');
const lrpt = parseSatSingle({ on: true, plan: [], now: P(57166, 'Meteor-M2-3', 'LRPT', -1, 14, 62, 'lrpt') });
ok(/picture builds below/.test(satSingleSub(lrpt, now)), 'Meteor: the picture builds');
ok(/SSTV/.test(satSingleSub(parseSatSingle({ on: true, plan: [], now: P(25544, 'ISS', 'SSTV', -1, 9, 50, 'sstv') }), now)), 'SSTV');
ok(/Packets/.test(satSingleSub(parseSatSingle({ on: true, plan: [], now: P(25544, 'ISS', 'APRS', -1, 9, 50, 'aprs') }), now)), 'APRS');
ok(/Telemetry/.test(satSingleSub(parseSatSingle({ on: true, plan: [], now: P(43803, 'JO-97', 'Telemetry', -1, 9, 50, 'fctlm') }), now)), 'telemetry');

console.log('§5 the hybrid');
const h = parseSatHybrid(HYB)!;
ok(satHybridText(h, now) === 'Your own VFO on RS-44 · Transponder: tune anywhere in 435.596–435.684 MHz, SSB or CW, with your own AUTO DOPPLER · 10:00 to LOS.', `"${satHybridText(h, now)}"`);
ok(satHybridInWindow(h, 435640000) && satHybridInWindow(h, 435595737) && !satHybridInWindow(h, 435800000) && !satHybridInWindow(null, 435640000), 'the window: in, on the edge, out, none');
ok(satHybridClamp(h, 435800000) === 435684263 && satHybridClamp(h, 435000000) === 435595737 && satHybridClamp(h, 435640000) === 435640000, 'a drag off the edge stops at it');
ok(satSingleSub({ ...during!, hybrid: h }, now) === satHybridText(h, now), 'during the hybrid the line says it');

console.log('§6 the timetable and the last pass');
const rows = satSinglePlanRows(during, now);
ok(rows.length === 1 && rows[0].live && rows[0].text === '17:58–18:18 · RS-44 · Transponder · max 46° · now', `the running pass first, marked now: "${rows[0]?.text}"`);
ok(satSinglePlanRows(over, now).length === 0, 'passes that have set are left out');
ok(satSinglePlanRows(before, now).map((r) => r.live).join() === 'false,false', 'to come: not live');
ok(satSingleLastText(parseSatSingle({ plan: [], last: 'RS-44 Transponder, max 46° — ended (LOS)' })) === 'Last pass: RS-44 Transponder, max 46° — ended (LOS)', 'the last pass, in the server\'s words');
ok(satSingleLastText(before) === '' && satSingleLastText(null) === '', 'no last pass: nothing');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
