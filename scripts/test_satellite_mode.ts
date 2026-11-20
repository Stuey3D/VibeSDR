// test_satellite_mode.ts — ★★★ SATELLITE MODE, the clients' pure half (src/services/satellites.ts, docs/v12/SATELLITE-MODE.md):
// the contract's new fields parsed (source.synced / fetching, inRangeFor, tx.use / sstv / why), the pick reply, the
// timetable's ORDER (up now SHORTEST remaining first, then next AOS) and GREYING (reach false / dead orbit, with why),
// the words ("UP · 6:12 left", "AOS 16:42 (in 0:38)", "Orbits synced 16:08 · 2 h ago", "Fetching orbit data…",
// "No orbit data — why"), the box size by use, and the half-globe's projection.
process.env.TZ = 'UTC';
import {
  globeScene, hmIn, hhmm, mss, orthoXYZ, parseSatList, parseSatPicked, satAltKm, satBoxSize, satCorrectedHz, satCountdownText,
  satDecoderFor, satDopplerMsg, satHzText, satListEveryMs, satModeRows, satPacketBaud, satPickMsg, satSyncedText, satWhenText,
  SAT_DROP_MSG, txUse, type SatTx,
} from '../src/services/satellites';

let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };

const now = Date.UTC(2026, 10, 20, 16, 4, 0) / 1000;   // 16:04:00 UTC
const tx = (o: Partial<SatTx> & { hz: number }): SatTx => ({ label: 'x', mode: 'nfm', loop: 'fll', reach: true, ...o });

const raw = {
  type: 'sat_list', time: 'ok', located: true, now,
  source: { orbits: 'Orbits: CelesTrak', via: 'vibesdr.net', synced: now - 2 * 3600 - 300, fetching: false },
  sats: [
    { id: 'METEOR-M2-3', name: 'METEOR-M2 3', norad: 57166, note: '', age: 0.5, ageState: 'fresh', el: 30, inRangeFor: 9 * 60 + 55, lat: 55, lon: 2, rangeKm: 1400,
      tx: [{ label: 'LRPT', hz: 137900000, mode: 'lrpt', loop: 'costas', reach: true, use: 'image' }], passes: [{ aos: now - 300, los: now + 595, maxEl: 60 }] },
    { id: 'ISS', name: 'ISS', norad: 25544, note: '', age: 0.2, ageState: 'fresh', el: 12, inRangeFor: 6 * 60 + 12, lat: 50, lon: -5,
      tx: [{ label: 'FM repeater', hz: 437800000, mode: 'nfm', loop: 'fll', reach: true, use: 'voice' },
           { label: 'Voice / SSTV', hz: 145800000, mode: 'nfm', loop: 'fll', reach: true, use: 'voice', sstv: true },
           { label: 'APRS', hz: 145825000, mode: 'ax25-1200', loop: 'fll', reach: false, use: 'packet', why: 'outside this radio’s 400–470 MHz range' }],
      passes: [{ aos: now - 200, los: now + 372, maxEl: 40 }] },
    { id: 'SO-50', name: 'SO-50', norad: 27607, note: '', age: 1, ageState: 'fresh', el: -5,
      tx: [{ label: 'FM', hz: 436795000, mode: 'nfm', loop: 'fll', reach: true, use: 'voice' }], passes: [{ aos: now + 38 * 60, los: now + 48 * 60, maxEl: 56 }] },
    { id: 'RS-44', name: 'RS-44', norad: 44909, note: '', age: 1, ageState: 'fresh', el: -20,
      tx: [{ label: 'Transponder', hz: 435610000, hiHz: 435670000, mode: 'usb', loop: 'none', reach: true, use: 'voice' }], passes: [{ aos: now + 3600 + 600, los: now + 4800, maxEl: 25 }] },
    { id: 'AO-7', name: 'AO-7', norad: 7530, note: '', age: 20, ageState: 'expired', el: -40,
      tx: [{ label: 'Mode A', hz: 29400000, hiHz: 29500000, mode: 'usb', loop: 'none', reach: true }], passes: [] },
  ],
};
const l = parseSatList(raw)!;

console.log('— the contract parsed');
ok(l.source?.synced === now - 7500 && l.source?.fetching === false, 'source.synced / fetching');
ok(l.sats[1].inRangeFor === 372 && l.sats[2].inRangeFor === undefined, 'inRangeFor while up, absent otherwise');
ok(l.sats[1].tx[1].sstv === true && l.sats[1].tx[0].sstv === undefined && l.sats[1].tx[2].use === 'packet', 'tx.use and sstv');
ok(l.sats[1].tx[2].why === 'outside this radio’s 400–470 MHz range', 'tx.why kept');
ok(parseSatList({ ...raw, sats: [], source: { synced: 0, fetching: true } })?.source?.fetching === true, 'a source with no orbits credit but fetching: still parsed');
ok(parseSatList({ ...raw, sats: [{ ...raw.sats[0], inRangeFor: -4, tx: [{ ...raw.sats[0].tx[0], use: 'bogus' }] }] })!.sats[0].inRangeFor === undefined
   && parseSatList({ ...raw, sats: [{ ...raw.sats[0], tx: [{ ...raw.sats[0].tx[0], use: 'bogus' }] }] })!.sats[0].tx[0].use === undefined, 'bad inRangeFor / use dropped');

console.log('— the pick reply and the requests');
ok(JSON.stringify(parseSatPicked({ type: 'sat', state: 'picked', norad: 57166, tx: 0, decoder: 'lrpt' })) === '{"norad":57166,"tx":0,"decoder":"lrpt"}', 'picked lrpt');
ok(parseSatPicked({ type: 'sat', state: 'picked', norad: 25544, tx: 0, decoder: '' })?.decoder === '', 'picked, no decoder');
ok(parseSatPicked({ type: 'sat', state: 'picked', norad: 25544, tx: 0, decoder: 'evil' })?.decoder === '', 'an unknown decoder reads as none');
ok(parseSatPicked({ type: 'sat', state: 'TRACK', norad: 1 }) === null && parseSatPicked({ type: 'sat', state: 'picked' }) === null, 'not a pick');
ok(JSON.stringify(satPickMsg(25544, 1, true)) === '{"type":"sat","op":"pick","norad":25544,"tx":1,"doppler":true}', 'op pick');
ok(JSON.stringify(satDopplerMsg(false)) === '{"type":"sat","op":"doppler","on":false}' && JSON.stringify(SAT_DROP_MSG) === '{"type":"sat","op":"drop"}', 'op doppler / drop');

console.log('— the timetable: order');
const rows = satModeRows(l, now);
const order = rows.map((r) => `${r.sat.id}#${r.i}`).join(',');
ok(order === 'ISS#0,ISS#1,ISS#2,METEOR-M2-3#0,SO-50#0,RS-44#0,AO-7#0', `up now SHORTEST remaining first (ISS 6:12 before METEOR 9:55), then by AOS, no pass last: ${order}`);
ok(rows.length === 7, 'nothing left out (every downlink listed)');
{ // up decided by elevation when an older server sends no inRangeFor: remaining from the pass's LOS
  const old = parseSatList({ ...raw, sats: raw.sats.map((s) => ({ ...s, inRangeFor: undefined })) })!;
  const r2 = satModeRows(old, now);
  ok(r2[0].sat.id === 'ISS' && r2[0].left === 372 && r2[3].sat.id === 'METEOR-M2-3' && r2[3].left === 595, 'an older server: remaining from the current pass');
}
ok(satModeRows(null, now).length === 0, 'no list, no rows');

console.log('— greying');
const iss2 = rows.find((r) => r.sat.id === 'ISS' && r.i === 2)!;
ok(iss2.grey && iss2.why === 'outside this radio’s 400–470 MHz range', 'reach false → greyed with tx.why');
const ao7 = rows.find((r) => r.sat.id === 'AO-7')!;
ok(ao7.grey && /too old to track/.test(ao7.why), `an expired orbit → greyed with why (${ao7.why})`);
ok(!rows.find((r) => r.sat.id === 'ISS' && r.i === 0)!.grey, 'a reachable downlink is live');
ok(satModeRows(parseSatList({ ...raw, sats: [{ ...raw.sats[2], tx: [{ ...raw.sats[2].tx[0], reach: false }] }] }), now)[0].why === 'out of this receiver’s range', 'reach false, no why → a plain reason');

console.log('— the words');
ok(satWhenText(rows[0], now) === 'UP · 6:12 left', `UP · 6:12 left (${satWhenText(rows[0], now)})`);
ok(satWhenText(rows.find((r) => r.sat.id === 'SO-50')!, now) === 'AOS 16:42 (in 0:38)', `AOS 16:42 (in 0:38) (${satWhenText(rows.find((r) => r.sat.id === 'SO-50')!, now)})`);
ok(satWhenText(rows.find((r) => r.sat.id === 'RS-44')!, now) === 'AOS 17:14 (in 1:10)', 'over an hour: h:mm');
ok(satWhenText(ao7, now) === 'No pass in the next day', 'no pass');
ok(satCountdownText(l.sats[0], now) === 'In range for 9:55', `compact countdown, up (${satCountdownText(l.sats[0], now)})`);
ok(satCountdownText(l.sats[2], now) === 'Next pass in 0:38 · AOS 16:42 · max 56°', `compact countdown, next (${satCountdownText(l.sats[2], now)})`);
ok(satCountdownText(l.sats[4], now) === 'No pass in the next day' && satCountdownText(undefined, now) === '', 'compact countdown, none');
ok(hhmm(now) === '16:04' && hmIn(now + 59, now) === '0:00' && hmIn(now + 12 * 3600 + 5 * 60, now) === '12:05', 'hh:mm / h:mm');
ok(mss(0) === '0:00' && mss(372) === '6:12' && mss(3725) === '1:02:05', 'm:ss');
ok(satSyncedText(l, now) === 'Orbits synced 13:59 · 2 h ago', `synced (${satSyncedText(l, now)})`);
ok(satSyncedText({ ...l, source: { ...l.source!, synced: now - 30 } }, now) === 'Orbits synced 16:03 · just now', 'just now');
ok(satSyncedText({ ...l, source: { ...l.source!, synced: now - 25 * 60 } }, now) === 'Orbits synced 15:39 · 25 min ago', 'minutes');
ok(satSyncedText({ ...l, source: { ...l.source!, synced: now - 3 * 86400 } }, now) === 'Orbits synced 16:04 · 3 days ago', 'days');
ok(satSyncedText({ ...l, source: { ...l.source!, synced: 0, fetching: true } }, now) === 'Fetching orbit data…', 'fetching');
ok(satSyncedText({ ...l, why: 'vibesdr.net did not answer (retry in 15 min)', source: { ...l.source!, synced: 0, fetching: false } }, now)
   === 'No orbit data — vibesdr.net did not answer (retry in 15 min)', 'no orbit data — why');
ok(/^No orbit data — /.test(satSyncedText({ ...l, source: { ...l.source!, synced: 0 } }, now)), 'no orbit data, no why: a plain reason');
ok(satSyncedText({ ...l, source: { orbits: 'Orbits: CelesTrak', via: 'x' } }, now) === '', 'an older server (no synced): nothing');
ok(satListEveryMs({ ...l, source: { ...l.source!, fetching: true } }) === 3000 && satListEveryMs(l) === 30000, 're-ask every 3 s while fetching');

console.log('— frequency, box size, decoder');
const ro = { state: 'TRACK' as const, nominal: 145800000, offset: 3210, time: 'ok' };
ok(satCorrectedHz(l.sats[1].tx[1], ro) === 145803210 && satCorrectedHz(l.sats[1].tx[1], null) === 145800000, 'the corrected frequency');
ok(satCorrectedHz(l.sats[3].tx[0], null) === 435640000, 'a passband: its middle');
ok(satHzText(145803210) === '145.803 21 MHz', `MHz to 10 Hz (${satHzText(145803210)})`);
ok(satBoxSize(l.sats[0].tx[0], false) === 'big', 'image → big');
ok(satBoxSize(l.sats[1].tx[0], false) === 'compact' && satBoxSize(l.sats[1].tx[0], true) === 'compact', 'voice → compact');
ok(satBoxSize(l.sats[1].tx[1], false) === 'compact' && satBoxSize(l.sats[1].tx[1], true) === 'big', 'ISS 145.800 sstv: compact, big once a picture starts');
ok(satBoxSize(l.sats[1].tx[2], true) === 'compact' && satBoxSize(undefined, true) === 'compact', 'packet → compact');
ok(satDecoderFor(l.sats[0].tx[0]) === 'lrpt' && satDecoderFor(l.sats[1].tx[1]) === 'sstv' && satDecoderFor(l.sats[1].tx[2]) === 'aprs' && satDecoderFor(l.sats[1].tx[0]) === '', 'the decoder by use');
ok(txUse(tx({ hz: 1, mode: 'lrpt' })) === 'image' && txUse(tx({ hz: 1, mode: 'ax25-9600' })) === 'packet' && txUse(tx({ hz: 1, mode: 'usb' })) === 'voice', 'use from an older server');
ok(satPacketBaud(tx({ hz: 1, mode: 'ax25-9600' })) === '9600' && satPacketBaud(tx({ hz: 1, mode: 'ax25-1200' })) === '1200', 'packet baud');

console.log('— the half-globe');
{
  const [x, y, z] = orthoXYZ(52, -1, 52, -1);
  ok(Math.abs(x) < 1e-9 && Math.abs(y) < 1e-9 && Math.abs(z - 1) < 1e-9, 'the view centre projects to the middle, facing');
  const [, yn] = orthoXYZ(60, -1, 52, -1), [xe] = orthoXYZ(52, 10, 52, -1);
  ok(yn > 0 && xe > 0, 'north up, east right');
  ok(orthoXYZ(-52, 179, 52, -1)[2] < 0, 'the antipode is behind');
  const alt = satAltKm(1400, 30)!;
  ok(alt > 700 && alt < 900, `height from range + elevation (${alt.toFixed(0)} km)`);
  ok(satAltKm(undefined, 3) === undefined, 'no range: no height');
  const g = globeScene(240, 120, { lat: 52.2, lon: -0.9 }, { lat: 55, lon: 2, altKm: alt });
  ok(g.rx != null && g.rx.y < g.cy && g.rx.y > g.cy - g.r, 'the receiver sits on the dome, above the cut');
  ok(g.sat != null && g.sat.y < g.rx!.y, 'a bird to the north and up stands above the receiver');
  ok(g.beam.startsWith('M') && g.foot.endsWith('Z') && g.grid.length > 200, 'the beam cone, its footprint and the graticule are drawn');
  ok(/^[MLZ0-9 .\-]+$/.test(g.grid + g.beam + g.foot), 'paths are plain M/L/Z (Path2D and react-native-svg both take them)');
  const far = globeScene(240, 120, { lat: 52.2, lon: -0.9 }, { lat: -40, lon: 175 });
  ok(far.sat === null && far.beam === '', 'a bird on the far side of the world is not drawn');
  const none = globeScene(240, 120, { lat: 52.2, lon: -0.9 }, null);
  ok(none.sat === null && none.rx != null, 'no position: the dome and the receiver');
  const all = [g.sat!, g.rx!];
  ok(all.every((p) => p.x >= 0 && p.x <= 240 && p.y >= 0 && p.y <= 120), 'everything inside the box');
}

console.log(fail ? `\n${fail} FAILED, ${pass} passed` : `\nall ${pass} passed`);
process.exit(fail ? 1 : 0);
