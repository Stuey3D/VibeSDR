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
         parseAdsbMapPrefs, ADSB_MAP_PREFS_DEFAULT, ADSB_MAX_AIRCRAFT } from '../src/services/adsbTypes.ts';

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
eq('prefs kept', parseAdsbMapPrefs('{"autoZoom":false,"fit":true,"rings":false}'), { autoZoom: false, fit: true, rings: false });
eq('prefs junk → defaults per key', parseAdsbMapPrefs('{"autoZoom":"no","fit":true}'), { autoZoom: true, fit: true, rings: true });
eq('prefs unparseable', parseAdsbMapPrefs('{'), ADSB_MAP_PREFS_DEFAULT);

console.log(`test_adsb_report: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
