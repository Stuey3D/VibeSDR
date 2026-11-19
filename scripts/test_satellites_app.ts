// test_satellites_app.ts — V12 satellite engine, the clients' half (src/services/satellites.ts; the web's satellites.ts
// panel and the app's SatellitePanel use it): the server's three messages parsed defensively (bounded, control characters
// out, bad numbers dropped), the readout words (brief §10: "ISS · ▼ 3.21 kHz", TRACK / COAST / ARMED / NO TIME / STALE),
// the picker's rows (up first, a downlink this radio cannot hear left out), the cap (absent = no button), the sky plot
// geometry. Shapes captured from the real server (scripts/test-server-sat.mjs, 2026-11-19).
import {
  ageText, clockIn, parseSatList, parseSatReadout, parseSatTrack, pickerRows, satCapFrom, satChanText, satStateWord,
  satStatusLine, skyPath, skySvg, skyXY, txListenMode, txText, txTuneHz, satCreditText, satTxCreditText,
} from '../src/services/satellites';

let fail = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

const now = 1795600000;
// a real {"type":"sat"} from the server (TRACK on the shared dial)
const real = { shared: true, type: 'sat', state: 'TRACK', id: 'ISS', name: 'ISS', norad: 25544, tx: 'FM repeater', loop: 'fll',
  nominal: 437800000, offset: -3214.6, rate: -169.2, trim: 12.5, az: 123.4, el: 34.2, aos: now - 200, los: now + 372, maxEl: 56.5,
  age: 0.2, ageState: 'fresh', time: 'ok' };
const r = parseSatReadout(real);
ok(!!r && r.state === 'TRACK' && r.name === 'ISS' && r.nominal === 437800000 && r.shared === true, 'a real readout parses');
ok(satChanText(r) === 'ISS · ▼ 3.21 kHz', `the white text: ${satChanText(r)}`);
ok(satChanText({ ...r!, offset: 8123 }) === 'ISS · ▲ 8.12 kHz', `approaching: ${satChanText({ ...r!, offset: 8123 })}`);
ok(satStateWord(r) === 'TRACK', 'TRACK');
ok(satStateWord({ ...r!, state: 'COAST' }) === 'COAST', 'COAST');
ok(satStateWord({ ...r!, ageState: 'stale', age: 4.2 }) === 'STALE', 'STALE (3–7 days)');
ok(satStateWord({ ...r!, ageState: 'old', age: 9.4 }) === 'ELEMENTS 9 d', `old elements show the age: ${satStateWord({ ...r!, ageState: 'old', age: 9.4 })}`);
ok(satStateWord({ ...r!, time: 'none' }) === 'NO TIME', 'NO TIME');
ok(satStateWord({ ...r!, time: 'system' }) === 'TRACK · TIME: SYSTEM', 'an unmeasured clock is said');
ok(satStatusLine(r, now) === 'SAT TRACK · ISS · EL 34° · LOS 6:12', `the status line: ${satStatusLine(r, now)}`);
const armed = parseSatReadout({ type: 'sat', state: 'ARMED', name: 'SO-50 (SaudiSat-1C)', aos: now + 225, time: 'ok' });
ok(!!armed && satStatusLine(armed, now) === 'SO-50 (SaudiSat-1C) · AOS 3:45', `armed: ${armed && satStatusLine(armed, now)}`);
ok(satChanText(parseSatReadout({ type: 'sat', state: 'OFF', why: 'x', time: 'ok' })) === '', 'OFF draws nothing beside the digits');
ok(parseSatReadout({ type: 'sat', state: 'BOGUS', time: 'ok' })?.state === 'OFF', 'an unknown state reads as OFF');
ok(parseSatReadout({ type: 'nope' }) === null && parseSatReadout(null) === null, 'not a readout');
const evil = parseSatReadout({ type: 'sat', state: 'TRACK', name: 'ISS\u0007<script>' + 'x'.repeat(200), offset: 'NaN', time: 'ok' });
ok(!!evil && !/\u0007/.test(evil.name!) && evil.name!.length <= 40 && evil.offset === undefined, 'control characters out, bounded, bad numbers dropped');

// the list
const list = parseSatList({ type: 'sat_list', time: 'ok', located: true, now, sats: [
  { id: 'ISS', name: 'ISS', norad: 25544, note: 'Crew contacts', age: 0.3, ageState: 'fresh', az: 200, el: 12.4, rangeKm: 1500, lat: 10, lon: 20,
    tx: [{ label: 'FM repeater', hz: 437800000, mode: 'nfm', loop: 'fll', upHz: 145990000, ctcss: 67, reach: true },
         { label: 'Voice / SSTV', hz: 145800000, mode: 'nfm', loop: 'fll', reach: false }],
    passes: [{ aos: now - 300, tca: now, los: now + 300, maxEl: 40, aosAz: 230, losAz: 60 }] },
  { id: 'RS-44', name: 'RS-44', norad: 44909, note: '', age: 1, ageState: 'fresh', el: -20,
    tx: [{ label: 'Transponder', hz: 435610000, hiHz: 435670000, mode: 'usb', loop: 'none', reach: true }],
    passes: [{ aos: now + 3600, tca: now + 3900, los: now + 4200, maxEl: 25, aosAz: 10, losAz: 170 }] },
  { id: 'SO-50', name: 'SO-50', norad: 27607, note: '', age: 1, ageState: 'fresh', el: -5,
    tx: [{ label: 'FM repeater', hz: 436795000, mode: 'nfm', loop: 'fll', reach: true }],
    passes: [{ aos: now + 600, tca: now + 900, los: now + 1200, maxEl: 60, aosAz: 10, losAz: 170 }] },
  { id: 'AO-7', name: 'AO-7', norad: 7530, note: '', age: 20, ageState: 'expired', el: -40,
    tx: [{ label: 'Mode A', hz: 29400000, hiHz: 29500000, mode: 'usb', loop: 'none', reach: false }], passes: [] },
  { id: '', name: 'bad', norad: 1, tx: [] },
] });
ok(!!list && list.sats.length === 4 && list.located && list.time === 'ok', 'the list parses (a row with no id dropped)');
const rows = pickerRows(list, now);
ok(rows.map(x => x.sat.id).join(',') === 'ISS,SO-50,RS-44', `rows: up first, then by AOS; unreachable birds out (${rows.map(x => x.sat.id).join(',')})`);
ok(rows[0].reach.length === 1, 'only the downlinks this radio hears are offered');
ok(txText(list!.sats[1].tx[0]) === '435.610–435.670 MHz USB' && txText(list!.sats[0].tx[0]) === '437.800 MHz FM', 'downlinks in words');
ok(txTuneHz(list!.sats[1].tx[0]) === 435640000 && txTuneHz(list!.sats[0].tx[0]) === 437800000, 'TRACK tunes a passband to its middle');
ok(txListenMode({ label: '', hz: 1, mode: 'lrpt', loop: 'costas', reach: true }) === 'nfm' && txListenMode(list!.sats[1].tx[0]) === 'usb', 'the listening mode');
ok(ageText(list!.sats[3]) === 'orbit data 20 days old — too old to track' && ageText(list!.sats[0]) === '', 'element age in words');
ok(clockIn(now + 3725, now) === '1:02:05' && clockIn(now - 1, now) === 'now', 'countdowns');
const t = parseSatTrack({ type: 'sat_track', norad: 25544, aos: now, los: now + 600, pts: [[230, 0], [180, 40], [60, 0], ['x', 3]] });
ok(!!t && t.pts.length === 3, 'a sky track (bad points dropped)');
ok(skyPath(t!.pts).startsWith('M') && /^<svg/.test(skySvg(t, { az: 180, el: 40 })) && skySvg(null).includes('</svg>'), 'the sky plot draws');
const [x0, y0] = skyXY(0, 90), [xn, yn] = skyXY(0, 0), [xe] = skyXY(90, 0);
ok(Math.abs(x0) < 1e-9 && Math.abs(y0) < 1e-9 && Math.abs(yn + 1) < 1e-9 && Math.abs(xe - 1) < 1e-9 && Math.abs(xn) < 1e-9, 'zenith centre, north up, east right');

// the cap
ok(satCapFrom({}) === null && satCapFrom({ doppler: 0 }) === null, 'no `doppler` = no button at all');
ok(JSON.stringify(satCapFrom({ doppler: 1 })) === JSON.stringify({ ok: true, why: '' }), 'doppler 1 = live');
ok(satCapFrom({ doppler: 1, dopplerWhy: 'NO SAT DATA — the satellite orbit data has not been downloaded yet' })?.ok === false, 'greyed with the reason');

// ★ the credit (vibesdr.net's sats.json: CelesTrak's orbits, SatNOGS's transmitter data) and six-digit numbers
{
  const l = parseSatList({ type: 'sat_list', time: 'ok', located: true, now,
    source: { orbits: 'Orbits: CelesTrak (celestrak.org) GP data', transmitters: 'Transmitters: curated by VibeSDR from AMSAT and SatNOGS DB (db.satnogs.org, CC BY-SA 4.0)', via: 'vibesdr.net', fetched: now - 5 * 3600, newestEpoch: now - 3600 },
    sats: [{ id: 'TEST-101095', name: 'Six', norad: 101095, note: '', age: 0.1, ageState: 'fresh',
      tx: [{ label: 'FM', hz: 435500000, mode: 'nfm', loop: 'fll', reach: true, status: 'inactive' }], passes: [] }] });
  ok(!!l && l.sats[0].norad === 101095 && l.sats[0].tx[0].status === 'inactive', 'six digits and a SatNOGS status parsed');
  ok(satCreditText(l, now) === 'Orbits: CelesTrak · 5 h old', `credit: ${satCreditText(l, now)}`);
  ok(satTxCreditText(l) === 'Transmitters: SatNOGS DB (CC BY-SA 4.0)', 'SatNOGS credited where used');
  ok(satCreditText({ ...l!, source: { orbits: 'Orbits: CelesTrak', via: 'built-in', newestEpoch: now - 3 * 86400 } }, now) === 'Orbits: CelesTrak · 3 days old', 'the built-in list: the newest epoch\'s age');
  ok(satCreditText({ ...l!, source: undefined }, now) === '' && satTxCreditText(null) === '', 'nothing to credit = nothing shown');
  const r6 = parseSatReadout({ type: 'sat', state: 'ARMED', norad: 123456789, name: 'Nine', time: 'ok' });
  ok(r6?.norad === 123456789, 'nine digits kept whole');
}

console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
