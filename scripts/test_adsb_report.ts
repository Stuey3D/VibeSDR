/**
 * VibeServer ADS-B in the app (src/services/adsbTypes.ts, docs/v12/ADSB.md) — the report's dump1090
 * fields mapped into the app's existing Aircraft shape, and the per-radio capability.
 *
 * ★★ The mapping is what the decoder box AND the watch read (WatchLink.Aircraft is a Codable keyed on
 *    icao/altitude/speed/vspeed/course/msgs …), so a renamed field here is an empty column on the
 *    phone and a blank list on the wrist. ★ The capability decides between NO control (older server),
 *    a working one, and a DISABLED one with the server's reason — never hidden when refused.
 *
 * Run: node --no-warnings scripts/test_adsb_report.ts   (run-tests.sh does)
 */
import { parseAdsbCapability, parseAdsbMessage, adsbToAircraft, adsbStatusText,
         parseAdsbMapPrefs, ADSB_MAP_PREFS_DEFAULT, ADSB_MAX_AIRCRAFT,
         adsbGainView, adsbGainStep, gainCapIdx, nearestGainIdx } from '../src/services/adsbTypes.ts';
import { readFileSync } from 'node:fs';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};

// ── capability ──────────────────────────────────────────────────────────────────────────────
eq('adsb true', parseAdsbCapability({ server: 'vibeserver', adsb: true }), { ok: true });
eq('adsb false + why', parseAdsbCapability({ adsb: false, adsbWhy: 'ADS-B needs a capture of 2.4 MS/s or more — this receiver runs at 2.048 MS/s' }),
   { ok: false, why: 'ADS-B needs a capture of 2.4 MS/s or more — this receiver runs at 2.048 MS/s' });
eq('adsb false, no why → a reason anyway', parseAdsbCapability({ adsb: false }),
   { ok: false, why: 'ADS-B is not available on this receiver' });
eq('older server: no field → no control', parseAdsbCapability({ server: 'vibeserver', dab: true }), undefined);
eq('modes list counts as true', parseAdsbCapability({ modes: ['dab', 'adsb'] }), { ok: true });
eq('a string "true" is not true', parseAdsbCapability({ adsb: 'true' }), undefined);
eq('why is cleaned of markup', parseAdsbCapability({ adsb: false, adsbWhy: '<b>owner</b> switched it off' })?.why,
   'bowner/b switched it off');
eq('not an object', parseAdsbCapability(null), undefined);

// ── the report → Aircraft ───────────────────────────────────────────────────────────────────
const RX = { lat: 52.2, lon: -0.9 };   // Northampton-ish
const msg = {
  type: 'adsb', now: 1761300000.5, messages: 12345, rate: 2400000, noise: -38.2, msgRate: 210, dropped: 0,
  aircraft: [
    { hex: '4CA1B2', flight: 'RYR4KL  ', category: 'A3', lat: 52.5, lon: -0.5, seen_pos: 0.4,
      alt_baro: 37000, alt_geom: 37650, gs: 451.2, track: 123.4, baro_rate: -64, squawk: '7700',
      emergency: 'general', seen: 0.1, messages: 412, rssi: -12.3 },
    { hex: '400abc', alt_baro: 'ground', gs: 12, mag_heading: 270, lat: 52.21, lon: -0.91, messages: 3 },
    { hex: '~1a2b3c', alt_baro: 2500, rssi: -30 },                         // non-ICAO, no position
    { hex: 'zzzzzz', flight: 'BAD' },                                     // not hex → dropped
    { flight: 'NOHEX' },                                                   // no hex → dropped
    { hex: '406123', lat: 'x', lon: 2, gs: 'fast', squawk: 1200, rssi: -5 },   // junk typed fields
  ],
};
const r = parseAdsbMessage(msg);
eq('report header', [r.now, r.messages, r.rate, r.noise, r.msgRate, r.dropped], [1761300000.5, 12345, 2400000, -38.2, 210, 0]);
eq('bad rows dropped', r.aircraft.map(a => a.hex), ['4ca1b2', '400abc', '~1a2b3c', '406123']);
eq('callsign trimmed', r.aircraft[0].flight, 'RYR4KL');
eq('junk lat → no position', [r.aircraft[3].lat, r.aircraft[3].lon], [undefined, undefined]);
eq('junk gs → unknown', r.aircraft[3].gs, undefined);
eq('numeric squawk → string', r.aircraft[3].squawk, '1200');

const list = adsbToAircraft(r.aircraft, RX);
eq('nearest first; no-position sorted by signal after', list.map(a => a.icao), ['400abc', '4ca1b2', '406123', '~1a2b3c']);
const ryr = list.find(a => a.icao === '4ca1b2')!;
eq('the field mapping', {
  icao: ryr.icao, flight: ryr.flight, altitude: ryr.altitude, speed: ryr.speed, vspeed: ryr.vspeed,
  course: ryr.course, squawk: ryr.squawk, rssi: ryr.rssi, msgs: ryr.msgs, lat: ryr.lat, lon: ryr.lon,
  category: ryr.category, emergency: ryr.emergency, seen: ryr.seen,
}, { icao: '4ca1b2', flight: 'RYR4KL', altitude: 37000, speed: 451.2, vspeed: -64, course: 123.4,
     squawk: '7700', rssi: -12.3, msgs: 412, lat: 52.5, lon: -0.5, category: 'A3', emergency: 'general', seen: 0.1 });
// ~43 km NE of the receiver (0.3° N = 33 km, 0.4° E at 52° = 27 km); bearing ~39°
eq('distance computed from the receiver', ryr.distKm, 43);
eq('bearing computed from the receiver', ryr.bearing, 39);
const gnd = list.find(a => a.icao === '400abc')!;
eq('ground → altitude 0 + onGround', [gnd.altitude, gnd.onGround], [0, true]);
eq('no track → the heading', gnd.course, 270);
const nopos = list.find(a => a.icao === '~1a2b3c')!;
eq('no position → no distance', [nopos.distKm, nopos.bearing], [undefined, undefined]);
eq('undefined keys dropped (JSON to the watch)', Object.keys(nopos).sort(), ['altitude', 'icao', 'rssi']);

eq('unknown receiver → no distances', adsbToAircraft(r.aircraft, null).every(a => a.distKm === undefined), true);
eq('Null Island receiver = unknown', adsbToAircraft(r.aircraft, { lat: 0, lon: 0 }).every(a => a.distKm === undefined), true);

const many = parseAdsbMessage({ aircraft: Array.from({ length: ADSB_MAX_AIRCRAFT + 50 }, (_, i) => ({ hex: (0x100000 + i).toString(16) })) });
eq('a report is capped', many.aircraft.length, ADSB_MAX_AIRCRAFT);
eq('not an array → empty', parseAdsbMessage({ aircraft: 'x' }).aircraft, []);

// ── status line + map prefs ─────────────────────────────────────────────────────────────────
eq('status: none yet', adsbStatusText(null), 'starting…');
eq('status: listening', adsbStatusText(parseAdsbMessage({ aircraft: [], msgRate: 3.4 })), 'listening… · 3 msg/s');
eq('status: counts', adsbStatusText(r), '4 aircraft (2 placed) · 210 msg/s');
eq('prefs default', parseAdsbMapPrefs(null), ADSB_MAP_PREFS_DEFAULT);
eq('prefs kept', parseAdsbMapPrefs('{"autoZoom":false,"fit":true,"rings":false,"labels":false}'), { autoZoom: false, fit: true, rings: false, labels: false });
eq('an old save without labels keeps them on', parseAdsbMapPrefs('{"autoZoom":false,"fit":true,"rings":false}'), { autoZoom: false, fit: true, rings: false, labels: true });
eq('prefs junk → defaults per key', parseAdsbMapPrefs('{"autoZoom":"no","fit":true}'), { autoZoom: true, fit: true, rings: true, labels: true });
eq('prefs unparseable', parseAdsbMapPrefs('{'), ADSB_MAP_PREFS_DEFAULT);

// ── ★★ V12 THE GAIN KEY (19f2d603) — shown only where the radio has a gain list and the owner has not fixed it ──
{
  const rtl = [0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496];
  const rep = parseAdsbMessage({ type: 'adsb', aircraft: [], gain: 49.6, gainAuto: 1, gainBusy: 1 });
  eq('the report carries the gain fields', [rep.gain, rep.gainAuto, rep.gainBusy], [49.6, 1, 1]);
  eq('junk gain fields are absent', parseAdsbMessage({ gain: 'high', gainAuto: null }).gain, undefined);
  const v = adsbGainView(rep, rtl, false)!;
  eq('auto + busy: the key, TRAINING', [v.key, v.training, v.now], ['GAIN 49.6 · AUTO', true, '49.6 dB']);
  eq('nearest step to 49.6 dB', v.idx, rtl.indexOf(496));
  eq('auto, settled', adsbGainView({ gain: 33.8, gainAuto: 1, gainBusy: 0 }, rtl, false)!.training, false);
  eq('by hand', adsbGainView({ gain: 33.8, gainAuto: 0 }, rtl, false)!.key, 'GAIN 33.8 dB');
  // AGENTS.md: a control that only works on one radio must not be there
  eq('no gain list (Airspy HF+): no key', adsbGainView(rep, [], false), null);
  eq('a one-step list: no key', adsbGainView(rep, [0], false), null);
  eq('the owner fixed the gain: no key', adsbGainView(rep, rtl, true), null);
  eq('an older server (no gain in the report): no key', adsbGainView(parseAdsbMessage({ aircraft: [] }), rtl, false), null);
  eq('no report: no key', adsbGainView(null, rtl, false), null);
  // the owner's per-band ceiling (the web's applyGainCap)
  eq('ceiling 30.0 dB → highest step ≤ 300', gainCapIdx(rtl, 300), rtl.indexOf(297));
  eq('no ceiling → the top', gainCapIdx(rtl, -1), rtl.length - 1);
  eq('every step above it → the lowest', gainCapIdx([100, 200], 50), 0);
  eq('the view clamps to the ceiling', adsbGainView(rep, rtl, false, 300)!.idx, rtl.indexOf(297));
  eq('nearest', nearestGainIdx(rtl, 300), rtl.indexOf(297));
  // − / + from the listener's own last step for 2.5 s, else the report's
  const hv = adsbGainView({ gain: 33.8, gainAuto: 0 }, rtl, false)!;
  eq('+ from the report', adsbGainStep(hv, null, 0, 1), hv.idx + 1);
  eq('+ again within 2.5 s: from the last press', adsbGainStep(hv, { idx: hv.idx + 1, at: 1000 }, 2000, 1), hv.idx + 2);
  eq('+ after 2.5 s: from the report', adsbGainStep(hv, { idx: hv.idx + 5, at: 0 }, 3000, 1), hv.idx + 1);
  eq('− clamps at 0', adsbGainStep(adsbGainView({ gain: 0, gainAuto: 0 }, rtl, false)!, null, 0, -1), 0);
  eq('+ clamps at the ceiling', adsbGainStep(adsbGainView({ gain: 30, gainAuto: 0 }, rtl, false, 300)!, null, 0, 1), rtl.indexOf(297));
  // ★ ONE RULE, TWO READERS: the notes are the web's words
  const main = readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
  for (const n of [v.note, hv.note])
    eq(`note is the web's: ${n.slice(0, 30)}…`, main.includes(`'${n}'`), true);
  // ★★ the settled note carries the server's reason (GainControl::why) — the same template on both readers
  const why = 'noise floor at its target (-31 dBFS, 30 dB under full scale) — more gain would only fill the ADC with noise';
  const wr = parseAdsbMessage({ type: 'adsb', aircraft: [], gain: 40.2, gainAuto: 1, gainBusy: 0, gainWhy: why });
  eq('the report carries the reason', wr.gainWhy, why);
  const settled = adsbGainView(wr, rtl, false)!.note;
  eq('the settled note says why', settled, `Automatic: ${why}. Checked again every ten minutes.`);
  eq('the web uses the same template', main.includes('`Automatic: ${why || \'chosen by the noise floor and messages decoded\'}. Checked again every ten minutes.`'), true);
  eq('no reason (an older server): the fallback', adsbGainView({ gain: 1, gainAuto: 1, gainBusy: 0 }, rtl, false)!.note,
     'Automatic: chosen by the noise floor and messages decoded. Checked again every ten minutes.');
  // ★★ the feed warning: a cleaned string, absent when the server sends none
  const fw = 'The receiver is at full gain and hearing little: its noise floor is 8 dB below where a working ADS-B feed sits.';
  eq('feedWarn carried', parseAdsbMessage({ aircraft: [], feedWarn: fw }).feedWarn, fw);
  eq('no feedWarn: absent', parseAdsbMessage({ aircraft: [] }).feedWarn, undefined);
  eq('junk feedWarn: absent', parseAdsbMessage({ aircraft: [], feedWarn: 42 }).feedWarn, undefined);
}

console.log(`test_adsb_report: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
