// test_satellite_mode.ts — ★★★ SATELLITE MODE, the clients' pure half (src/services/satellites.ts, docs/v12/SATELLITE-MODE.md):
// the contract's new fields parsed (source.synced / fetching, inRangeFor, tx.use / sstv / why), the pick reply, the
// timetable's ORDER (up now SHORTEST remaining first, then next AOS) and GREYING (reach false / dead orbit, with why),
// the words ("UP · 6:12 left", "AOS 16:42 (in 0:38)", "Orbits synced 16:08 · 2 h ago", "Fetching orbit data…",
// "No orbit data — why"), the box size by use, and the half-globe's projection.
process.env.TZ = 'UTC';
import {
  satPassQuality, satPassText, satDopplerBadge,
  globeScene, hmIn, hhmm, mss, orthoXYZ, parseSatList, parseSatPicked, satAltKm, satBoxSize, satCorrectedHz, satCountdownText,
  satDecoderFor, satDopplerMsg, satHzText, satListEveryMs, satModeRows, satPacketBaud, satPickMsg, satSyncedText, satWhenText,
  SAT_DROP_MSG, txUse, type SatTx,
  satBandLetter, satTxModeLabel, satTxDetail, satRemindOffered, satRemindKeyText, satRemindLead, SatReminders, SAT_REMIND_MAX, satRemindText,
  satRemindWhy, SAT_REMIND_SHARED_WHY, satRemindEnds, parseSatTelemetry, satTelemetryLine, satTelemetryHead, parseSatTrack, globeLabels,
  globeLabelText, GLOBE_RX_DOT, GLOBE_KEY_TEXT, satMapOffered, satViaText, satTelemetryMerge, satTelemetryHex, satMapOverlay,
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

console.log('— the pass rating and the readout badge (11-21)');
{
  ok(satPassQuality(75) === 'EXCELLENT' && satPassQuality(50) === 'EXCELLENT', '≥ 50° EXCELLENT');
  ok(satPassQuality(49) === 'GOOD' && satPassQuality(30) === 'GOOD', '30–49° GOOD');
  ok(satPassQuality(29) === 'MARGINAL' && satPassQuality(15) === 'MARGINAL', '15–29° MARGINAL');
  ok(satPassQuality(11) === 'POOR', '< 15° POOR (Stuart\'s 11° METEOR)');
  ok(satPassText({ aos: 0, tca: 0, los: 0, maxEl: 55.6, aosAz: 0, losAz: 0 }) === 'max 56° · EXCELLENT', 'the row text');
  ok(satPassText(undefined) === '', 'no pass → no rating');
  ok(satDopplerBadge(null) === null && satDopplerBadge({ state: 'OFF', time: 'ok' }) === null, 'no lock → no badge');
  const armed = satDopplerBadge({ state: 'ARMED', name: 'ISS', time: 'ok' });
  ok(!!armed && armed.dir === 0 && armed.dim, 'ARMED → the glyph alone, dim');
  const up = satDopplerBadge({ state: 'TRACK', name: 'ISS', offset: 8123, time: 'ok' });
  ok(!!up && up.dir === 1 && !up.dim && /approaching 8\.12 kHz/.test(up.label), `TRACK +8.1 kHz → ▲ (${up?.label})`);
  const dn = satDopplerBadge({ state: 'COAST', name: 'ISS', offset: -3210, time: 'ok' });
  ok(!!dn && dn.dir === -1 && dn.dim, 'COAST −3.2 kHz → ▼ dim');
  ok(satDopplerBadge({ state: 'ENDED', name: 'ISS', time: 'ok' }) === null, 'ENDED → no badge');
}

console.log('— 1.4 transponder mode labels');
{
  ok(satBandLetter(29.45e6) === 'A' && satBandLetter(145.9e6) === 'V' && satBandLetter(435.6e6) === 'U' && satBandLetter(1268e6) === 'L' && satBandLetter(50e6) === '', 'AMSAT band letters');
  const l2 = parseSatList({ ...raw, sats: [{ ...raw.sats[4], tx: [
    { label: 'Mode B', hz: 145925000, hiHz: 145975000, mode: 'usb', loop: 'none', reach: true, band: 'U/v', modeName: 'Mode B' },
    { label: 'Transponder', hz: 29400000, hiHz: 29500000, mode: 'usb', loop: 'none', reach: true, band: 'V/a', modeName: 'Mode A' },
    { label: 'Bad', hz: 1, mode: 'usb', loop: 'none', reach: true, band: '<script>' }] }] })!;
  const [mb, ma, bad] = l2.sats[0].tx;
  ok(mb.band === 'U/v' && mb.modeName === 'Mode B' && bad.band === undefined, 'band / modeName parsed, junk band dropped');
  ok(satTxModeLabel(mb) === 'U/v', `the label already says "Mode B" → just the bands (${satTxModeLabel(mb)})`);
  ok(satTxModeLabel(ma) === 'Mode A · V/a', `"Mode A · V/a" (${satTxModeLabel(ma)})`);
  ok(satTxDetail(ma) === '29.400–29.500 MHz USB · Mode A · V/a', `the row's detail line (${satTxDetail(ma)})`);
  ok(satTxModeLabel(tx({ hz: 435610000, hiHz: 435670000, upHz: 145935000 })) === 'V/u', 'an older server: worked out from uplink + downlink');
  ok(satTxModeLabel(tx({ hz: 145800000 })) === '' && satTxDetail(tx({ hz: 145800000 })) === '145.800 MHz FM', 'a plain FM downlink: nothing added');
}

console.log('— 1.6 REMIND: offered only where a pick is private');
{
  const priv = parseSatList({ ...raw, source: { ...raw.source, pickPrivate: true } })!;
  const shared = parseSatList({ ...raw, source: { ...raw.source, pickPrivate: false } })!;
  ok(priv.source?.pickPrivate === true && shared.source?.pickPrivate === false && l.source?.pickPrivate === undefined, 'pickPrivate true / false / absent');
  const so50 = satModeRows(priv, now).find((r) => r.sat.id === 'SO-50')!;
  const issUp = satModeRows(priv, now).find((r) => r.sat.id === 'ISS' && r.i === 0)!;
  const iss2p = satModeRows(priv, now).find((r) => r.sat.id === 'ISS' && r.i === 2)!;
  const ao7p = satModeRows(priv, now).find((r) => r.sat.id === 'AO-7')!;
  ok(satRemindOffered(priv, so50), 'single-user / locked range, a pass to come → REMIND');
  ok(!satRemindOffered(shared, so50), 'a shared dial → hidden');
  ok(!satRemindOffered(l, so50), 'an older server (no pickPrivate) → hidden (never guessed)');
  ok(!satRemindOffered(priv, iss2p), 'a greyed row → hidden');
  ok(!satRemindOffered(priv, issUp), 'up now → nothing to remind of');
  ok(!satRemindOffered(priv, ao7p), 'no pass → hidden');
  ok(satRemindKeyText(false, 2) === 'REMIND' && satRemindKeyText(true, 5) === 'REMINDING · 5 min', 'the key\'s words');
  ok(satRemindLead('5') === 5 && satRemindLead(3) === 2 && satRemindLead(null) === 2 && satRemindLead(10) === 10, 'lead 1 / 2 / 5 / 10, default 2');
}

console.log('— 1.6 REMIND: the timer (a fake clock)');
{
  const priv = parseSatList({ ...raw, source: { ...raw.source, pickPrivate: true } })!;
  const book = new SatReminders();
  const so50 = satModeRows(priv, now).find((r) => r.sat.id === 'SO-50')!;
  const aos = now + 38 * 60;
  ok(book.add('srv', so50, now) && book.size === 1, 'held');
  ok(!!book.find('srv', 27607, 0) && !book.find('other', 27607, 0), 'keyed to the server');
  ok(book.due(aos - 121, 2, true).length === 0, '2:01 before AOS: not yet');
  ok(Math.round(book.nextIn(aos - 200, 2)) === 80, 'the next is due in 80 s');
  const d = book.due(aos - 120, 2, true);
  ok(d.length === 1 && d[0].norad === 27607 && book.size === 0, '2:00 before AOS: fires once, leaves the book');
  ok(satRemindText(d[0], aos - 120) === 'SO-50 rises in 2:00 — LISTEN?', `the pop-up's line (${satRemindText(d[0], aos - 120)})`);
  ok(satRemindText({ ...d[0], label: 'Voice / SSTV', name: 'ISS' }, aos - 120) === 'ISS · Voice / SSTV rises in 2:00 — LISTEN?', 'with the downlink');
  ok(satRemindText(d[0], aos + 5) === 'SO-50 is up now — LISTEN?', 'once AOS has gone by');
  // the element set changes: the reminder follows its pass
  book.add('srv', so50, now);
  const moved = parseSatList({ ...raw, source: { ...raw.source, pickPrivate: true }, sats: raw.sats.map((s) => s.id === 'SO-50' ? { ...s, passes: [{ aos: aos + 40, los: aos + 640, maxEl: 55 }] } : s) })!;
  book.refresh('srv', moved);
  ok(book.all()[0].aos === aos + 40 && book.all()[0].key.endsWith(String(aos + 40)), 'a new element set moves the reminder 40 s, re-keyed');
  ok(book.due(aos - 120, 2, true).length === 0 && book.due(aos - 80, 2, true).length === 1, '…and it fires at the NEW time');
  // a different lead
  book.add('srv', so50, now);
  ok(book.due(aos - 601, 10, true).length === 0 && book.due(aos - 600, 10, true).length === 1, 'lead 10 min');
  // no connection: never fires, dropped silently
  book.add('srv', so50, now);
  ok(book.due(aos - 60, 2, false).length === 0 && book.size === 0, 'no connection at fire time → dropped silently');
  // expiry and cap
  book.add('srv', so50, now);
  ok(book.due(aos + 601, 2, true).length === 0 && book.size === 0, 'a pass that is over → dropped unfired');
  ok(!book.add('srv', { ...so50, next: { ...so50.next!, aos: now - 1 } }, now), 'an AOS already gone is refused');
  for (let i = 0; i < 25; i++) book.add('srv', { ...so50, i, sat: { ...so50.sat, norad: 1000 + i } }, now);
  ok(book.size === SAT_REMIND_MAX, `bounded at ${SAT_REMIND_MAX}`);
  book.clearServer('srv');
  ok(book.size === 0, 'leaving the server cancels every reminder for it');
  book.add('srv', so50, now); book.add('srv', so50, now);
  ok(book.size === 1, 'one per downlink (pressing again re-holds, never doubles)');
  ok(book.cancel(book.all()[0].key) && book.size === 0, 'cancel');
}

console.log('— 1.6 REMIND: the re-check as it fires');
{
  const r = { norad: 27607, tx: 0, name: 'SO-50' };
  const priv = parseSatList({ ...raw, source: { ...raw.source, pickPrivate: true } })!;
  ok(satRemindWhy(priv, r) === '', 'still pickable → no why');
  ok(satRemindWhy(parseSatList({ ...raw, source: { ...raw.source, pickPrivate: false } }), r) === SAT_REMIND_SHARED_WHY, 'now shared → "This radio is now shared…"');
  const off = parseSatList({ ...raw, source: { ...raw.source, pickPrivate: true }, sats: raw.sats.map((s) => s.id === 'SO-50' ? { ...s, tx: [{ ...s.tx[0], reach: false, why: 'Out of this receiver’s range — it is locked to 130–170 MHz' }] } : s) })!;
  ok(satRemindWhy(off, r) === 'Out of this receiver’s range — it is locked to 130–170 MHz', 'reach false → the server\'s why');
  ok(/no longer listed/.test(satRemindWhy(parseSatList({ ...raw, source: { ...raw.source, pickPrivate: true }, sats: [] }), r)), 'gone from the list → says so');
  ok(satRemindEnds({ norad: 25544, tx: 0 }, 'ISS', r) === 'picking this ends ISS' && satRemindEnds(null, undefined, r) === '' && satRemindEnds({ norad: 27607, tx: 0 }, 'SO-50', r) === '', 'two at once: "picking this ends ISS"');
}

console.log('— 1.3 telemetry values (the server fctlm_status + sat_tlm)');
{
  const st = parseSatTelemetry({ sync: true, frames: 14, good: 13, failed: 1, afcHz: -12.5, snrDb: 7.5, centreHz: 1200, auto: true })!;
  ok(st.frames === 14 && st.good === 13 && st.failed === 1 && st.sync === true && st.values === undefined, 'fctlm_status: the counts only');
  const fr = parseSatTelemetry({ sat: 'AO-73', format: 'funcube-rt', t: 1763700000, frame: 'RT1', mode: 'Safe', hex: '0a1B2c3d4e5f60718293zz',
    values: [{ k: 'eps.vbat', name: 'Battery voltage', v: 8.1349, unit: 'V' }, { k: 'eps.tbat', name: 'Battery temperature', v: 143, unit: 'C', raw: true },
             { k: 'x', name: 'Bad', v: NaN }, { k: 'y', v: 3 }, { name: 'Text', v: 'ON' }] })!;
  ok(fr.frames === undefined && fr.sat === 'AO-73' && fr.mode === 'Safe' && fr.at === 1763700000, 'sat_tlm: the frame (no counts)');
  ok(fr.values!.map(satTelemetryLine).join(' | ') === 'Battery voltage 8.135 V | Battery temperature 143 raw | y 3 | Text ON',
     `values with units; raw = the count, labelled, no unit: ${fr.values!.map(satTelemetryLine).join(' | ')}`);
  ok(fr.hex === '0a1B2c3d4e5f60718293', 'hex kept to hex digits');
  let t = satTelemetryMerge(null, st);
  ok(satTelemetryHead(t) === 'TELEMETRY · 14 frames · 13 good · 1 failed', `counts first (${satTelemetryHead(t)})`);
  t = satTelemetryMerge(t, fr);
  ok(t!.frames === 14 && t!.values.length === 4 && satTelemetryHead(t) === 'TELEMETRY · 14 frames · 13 good · 1 failed · Safe', 'a frame keeps the counts');
  t = satTelemetryMerge(t, parseSatTelemetry({ frames: 15, good: 14, failed: 1 }));
  ok(t!.frames === 15 && t!.values.length === 4, 'a status keeps the last frame\'s values');
  ok(satTelemetryHex(t) === '0a1B2c3d 4e5f6071 8293', 'RAW: the bytes in groups');
  ok(satTelemetryHead(null) === 'TELEMETRY · waiting for a frame' && satTelemetryHead(satTelemetryMerge(null, { sync: true })) === 'TELEMETRY · in sync — waiting for a good frame', 'waiting');
  const aprsT = satTelemetryMerge(null, parseSatTelemetry({ sat: 'ISS', format: 'aprs-t', values: [{ k: 'a1', name: 'A1', v: 12 }] }));
  ok(satTelemetryHead(aprsT) === 'TELEMETRY', 'an APRS T# frame: values, no counts');
  ok(parseSatTelemetry(null) === null && parseSatTelemetry({ values: 'x' })!.values === undefined, 'junk is safe');
  ok(parseSatPicked({ type: 'sat', state: 'picked', norad: 39444, tx: 0, decoder: 'fctlm' })?.decoder === 'fctlm'
     && parseSatPicked({ type: 'sat', state: 'picked', norad: 44909, tx: 1, decoder: 'cw' })?.decoder === 'cw', 'a pick may attach fctlm / cw (beta90)');
  const lt = parseSatList({ ...raw, sats: [{ ...raw.sats[0], tx: [{ ...raw.sats[0].tx[0], decoder: 'fctlm' }, { ...raw.sats[0].tx[0], decoder: 'evil' }] }] })!;
  ok(lt.sats[0].tx[0].decoder === 'fctlm' && lt.sats[0].tx[1].decoder === undefined && satDecoderFor(lt.sats[0].tx[0]) === 'fctlm', 'tx.decoder: the server\'s word first');
}

console.log('— 1.7 the orbit, dotted');
{
  const rx = { lat: 52.2, lon: -0.9 };
  // a pass straight over the receiver (north → south along its meridian) and a low one far to the east
  const over: [number, number, number][] = [], low: [number, number, number][] = [];
  for (let k = 0; k <= 60; k++) { over.push([72 - k * 0.66, -0.9 + (k - 30) * 0.02, 420]); low.push([70 - k * 0.6, 30 + k * 0.1, 420]); }
  const g = globeScene(640, 300, rx, { lat: 52.2, lon: -0.9, altKm: 420 }, over);
  ok(g.orbit.startsWith('M') && /^[ML0-9 .\-]+$/.test(g.orbit), 'the orbit is a plain M/L path');
  ok(!!g.tca && !!g.rx && Math.abs(g.tca.x - g.rx.x) < 6, `an overhead pass: its highest point is over the receiver's dot (Δx ${g.tca && g.rx ? (g.tca.x - g.rx.x).toFixed(1) : '?'})`);
  ok(g.tcaTick.startsWith('M'), 'the highest point gets its tick');
  const gl = globeScene(640, 300, rx, null, low);
  const dOver = Math.hypot(g.tca!.x - g.rx!.x, g.tca!.y - g.rx!.y), dLow = gl.tca && gl.rx ? Math.hypot(gl.tca.x - gl.rx.x, gl.tca.y - gl.rx.y) : 0;
  ok(dLow > 2.5 * dOver && dLow > 50, `a low pass stays well away from the dot (${dLow.toFixed(0)} px vs ${dOver.toFixed(0)} overhead)`);
  const far: [number, number, number][] = []; for (let k = 0; k < 40; k++) far.push([-50 + k, 170, 420]);
  const gf = globeScene(640, 300, rx, null, far);
  ok(gf.orbit === '' && gf.tca === null, 'the far side of the world is not drawn');
  ok(globeScene(640, 300, rx, null).orbit === '' && globeScene(640, 300, rx, null, null).tca === null, 'no path (an older server) → no line');
  const tr = parseSatTrack({ type: 'sat_track', norad: 25544, aos: 1, los: 2, pts: [[0, 1]], geo: [[52, -1, 420], [53, -1, 421], [99, 0, 1], null, [54, 'x', 1], [55, -1, 420]],
    geoT0: 1000, geoStep: 10, tca: 1010 })!;
  ok(tr.geo?.length === 6 && tr.geo[1]![2] === 421 && tr.geo[2] === null && tr.geo[3] === null && tr.geo[4] === null, 'geo parsed; null / bad entries stay gaps (index = time)');
  ok(tr.tcaIdx === 1, 'the server\'s tca → its geo index');
  const gapped = globeScene(640, 300, rx, null, [...over.slice(0, 20), null, ...over.slice(21)] as any);
  ok((gapped.orbit.match(/M/g) || []).length === (g.orbit.match(/M/g) || []).length + 1, 'a null entry lifts the pen');
  const gt = globeScene(640, 300, rx, null, over, 10);
  const q10 = globeScene(640, 300, rx, { lat: over[10][0], lon: over[10][1], altKm: 420 }).sat!;
  ok(!!gt.tca && Math.hypot(gt.tca.x - q10.x, gt.tca.y - q10.y) < 1, 'the tick goes where the server\'s tca says');
  ok(parseSatTrack({ type: 'sat_track', norad: 1, aos: 1, los: 2, pts: [] })!.geo === undefined, 'no geo → undefined');
  const big = { type: 'sat_track', norad: 1, aos: 1, los: 2, pts: [], geo: Array.from({ length: 500 }, (_, i) => [i / 10, 0, 400]) };
  ok(parseSatTrack(big)!.geo!.length === 200, 'bounded (200)');
}

console.log('— 1.8 the drawn bird');
{
  const rx = { lat: 52.2, lon: -0.9 };
  const path: [number, number, number][] = []; for (let k = 0; k <= 60; k++) path.push([30 + k * 0.7, -20 + k * 0.6, 420]);
  let allDown = true, allAlong = true, n = 0;
  for (const [la, lo] of [[40, -10], [50, 0], [60, 10], [45, -15], [55, 5]]) {
    const g = globeScene(640, 300, rx, { lat: la, lon: lo, altKm: 420 }, path);
    if (!g.bird || !g.sat) continue;
    n++;
    const toC = [g.cx - g.sat.x, g.cy - g.sat.y], m = Math.hypot(toC[0], toC[1]);
    if ((g.bird.d[0] * toC[0] + g.bird.d[1] * toC[1]) / m < 0.999) allDown = false;   // ★ the dish faces the globe centre
    const fx = g.bird.focus.x - g.sat.x, fy = g.bird.focus.y - g.sat.y;
    if ((fx * toC[0] + fy * toC[1]) / (Math.hypot(fx, fy) * m) < 0.999) allDown = false;
    // the boom lies along the path's screen heading near the bird
    const near = path.reduce((b, p, i) => (Math.hypot(p[0] - la, p[1] - lo) < Math.hypot(path[b][0] - la, path[b][1] - lo) ? i : b), 0);
    const a = globeScene(640, 300, rx, { lat: path[near][0], lon: path[near][1], altKm: 420 }).sat, b = globeScene(640, 300, rx, { lat: path[near + 1][0], lon: path[near + 1][1], altKm: 420 }).sat;
    if (a && b) { const hx = b.x - a.x, hy = b.y - a.y, hm = Math.hypot(hx, hy); if (Math.abs((g.bird.u[0] * hx + g.bird.u[1] * hy) / hm) < 0.99) allAlong = false; }
  }
  ok(n >= 4, `drawn at ${n} places on the dome`);
  ok(allDown, 'the dish faces the globe centre at every position');
  ok(allAlong, "the panels' axis follows the path's heading");
  const lvl = globeScene(640, 300, rx, { lat: 50, lon: 0, altKm: 420 });
  ok(!!lvl.bird && lvl.bird.u[0] === 1 && lvl.bird.u[1] === 0, 'no path yet → panels level');
  const b = lvl.bird!;
  ok([b.body, b.dish, b.panels, b.cells, b.boom].every((d) => /^[MLZ0-9 .\-]+$/.test(d) && d.startsWith('M')), 'the bird is plain M/L/Z paths (canvas + react-native-svg)');
  ok(b.reach > 35 && b.reach < 60, `about twice the old size (reach ${b.reach.toFixed(0)} scene px; old panels reached 20)`);
  ok(/^M[0-9.]+ [0-9.]+L/.test(lvl.beam) && lvl.beam.startsWith(`M${b.focus.x.toFixed(1)} ${b.focus.y.toFixed(1)}`), 'the beam leaves the dish');
  ok(GLOBE_RX_DOT.fill === '#5BE36B' && GLOBE_RX_DOT.r === 5 && GLOBE_RX_DOT.ringW === 1.5, 'the receiver dot: green #5BE36B, r 5, 1.5 px dark ring');
}

console.log('— 1.8 labels');
{
  const rx = { lat: 52.2, lon: -0.9 };
  const g = globeScene(640, 300, rx, { lat: 58, lon: 3, altKm: 820 });
  const L = globeLabels(g, 'Northampton RSP1B', 'METEOR-M2 3', 22);
  ok(L.rx?.text === 'Northampton RSP1B' && L.sat?.text === 'METEOR-M2 3', 'the server\'s name and the bird\'s');
  ok(globeLabels(g, '', 'ISS', 22).rx?.text === 'RECEIVER', 'no server name → RECEIVER');
  ok(globeLabelText('A very long server name indeed') === 'A very long serve…', 'a long name is cut');
  const box = (lb: { x: number; y: number; anchor: string; text: string }, f: number) => { const w = lb.text.length * f * 0.62; const l = lb.anchor === 'start' ? lb.x : lb.anchor === 'end' ? lb.x - w : lb.x - w / 2; return { l, r: l + w, t: lb.y - f * 0.85, b: lb.y + f * 0.2 }; };
  const a = box(L.rx!, 22), c = box(L.sat!, 22);
  ok(!(a.l < c.r && c.l < a.r && a.t < c.b && c.t < a.b), 'the two labels do not overlap');
  ok([a, c].every((q) => q.l >= 0 && q.r <= 640 && q.t >= 0 && q.b <= g.cy), 'both inside the picture');
  // the bird right over the dot: its own label goes ABOVE it, the receiver's keeps clear
  const gn = globeScene(640, 300, rx, { lat: 52.2, lon: -0.9, altKm: 420 });
  const Ln = globeLabels(gn, 'RX', 'ISS', 22);
  ok(!!Ln.sat && Ln.sat.y < gn.sat!.y, 'a bird near the dot → its label above it');
  const an = box(Ln.rx!, 22), cn = box(Ln.sat!, 22);
  ok(!(an.l < cn.r && cn.l < an.r && an.t < cn.b && cn.t < an.b), '…and the receiver\'s label stays clear of it');
  ok(/dotted: this pass.s orbit · shaded: where it can be heard/.test(GLOBE_KEY_TEXT), 'BIG\'s one-line key');
}

console.log('— 1.9 the MAP key');
{
  const ov = satMapOverlay('ISS', [[50, 170, 420], [51, 179, 420], [52, -179, 420], [53, -170, 420], null, [54, -160, 420], [55, -150, 420]], { lat: 51, lon: 0, altKm: 420 });
  ok(ov.label === 'ISS PASS' && ov.lines?.length === 3 && ov.lines[0].length === 2 && ov.lines[1].length === 2, `the ground track split at the antimeridian and at a gap (${ov.lines?.map((l) => l.length).join('/')})`);
  ok(!!ov.circles && Math.abs(ov.circles[0].km - 2250) < 60 && ov.points?.[0].label === 'ISS', `the footprint (${ov.circles?.[0].km} km) and the bird`);
  ok(satMapOverlay('X', null, null).lines === undefined, 'no orbit yet: no line');
  ok(satMapOffered(l.sats[1].tx[2], 1) && !satMapOffered(l.sats[1].tx[2], 0), 'packet: only once a station with a position is heard');
  ok(!satMapOffered(l.sats[1].tx[0], 5) && !satMapOffered(undefined, 5), 'voice / nothing picked: no MAP');
  ok(satViaText('ISS') === 'via ISS' && satViaText('SONATE-2') === 'via SONATE-2', '"via ISS"');
}

console.log(fail ? `\n${fail} FAILED, ${pass} passed` : `\nall ${pass} passed`);
process.exit(fail ? 1 : 0);
