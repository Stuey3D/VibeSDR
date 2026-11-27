/* test_airsea_viewer.ts — ★★ V12 beta96 the clients' half of the standalone AIR & SEA map and of the aircraft deltas:
 *   A. AircraftDeltaStore (src/services/aircraftDelta.ts): a whole table, then deltas (a moved aircraft's y, a renamed one's
 *      s, a gone key), times back to ages against `now`, sharedSanitise reads the result; an older server's whole table
 *      passes through; junk and a delta with no table never corrupt anything.
 *   B. airSeaOffer (src/services/airSeaViewer.ts): the landing button's gate — aircraft, ships, sondes, VOR, a feed alone;
 *      nothing → no button; the most populated tab; a feed's label when it is all there is (HFDL).
 *   C. AirSeaViewer: the ticket on open, the cursor carried, the ticket sent with every poll; the map full (N/N); the time
 *      limit (countdown, the last minute's banner, ENDED stops it); the server forgetting the ticket (join again); an older
 *      server (no tickets, no deltas) still works; leave ends the ticket.
 * Silent, no network. */
import { AircraftDeltaStore } from '../src/services/aircraftDelta';
import { airSeaOffer, AirSeaViewer, mapFullText, mapEndedText, mapClockText } from '../src/services/airSeaViewer';
import { sharedSanitise, avCompose, avLayersOf } from '../src/services/aviationMap';
import { composeAirSeaStandalone, newMsgFeed } from '../src/services/airSeaStandalone';
import { shipLayersOf } from '../src/services/shippingMap';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

async function main() {
console.log('── A. the delta store ──');
{
  const st = new AircraftDeltaStore();
  const now = 1790000000;
  const full = { d: 1, c: 'ab.1', full: 1, now, count: 2, adsbLive: 1, adsbLast: 0, adsbNow: 1, adsbWin: 60,
    a: [{ k: '4CA2D1', s: { icao: '4CA2D1', flight: 'RYR12', op: 'RYR' }, y: { lat: 51.5, lon: -0.1, pt: now - 2, posSrc: 'ADS-B', alt: 31000, src: ['ADS-B'], st: { 'ADS-B': now - 1 }, lt: now - 1, n: 50 } },
        { k: 'AB12CD', s: { icao: 'AB12CD', flight: 'BAW123', label: 'H1' }, y: { lat: 50, lon: -20, pt: now - 120, posSrc: 'HFDL', src: ['HFDL'], st: { HFDL: now - 120 }, lt: now - 120, n: 3 } }],
    g: [], stations: [], feeds: [{ title: 'UberSDR HFDL', label: 'HFDL', state: 'connected', aircraft: 1 }] };
  const r = st.apply(full);
  const s = sharedSanitise(r.json);
  ok(!!s && s.aircraft.length === 2 && r.cursor === 'ab.1', 'a whole table → aircraft.json\'s shape, read by sharedSanitise (2 aircraft, cursor kept)');
  const ry = s!.aircraft.find((a) => a.key === '4CA2D1')!;
  ok(ry.posAge === 2 && ry.age === 1 && ry.srcAge['ADS-B'] === 1 && ry.flight === 'RYR12' && ry.info?.op === 'RYR', 'times back to ages (posAge 2, age 1, srcAge 1) and the static fields kept');
  ok(s!.feeds.length === 1 && s!.adsb.live === true, 'the header and the feeds come through');
  // a delta 3 s later
  const d = { d: 1, c: 'ab.4', full: 0, now: now + 3, count: 2,
    a: [{ k: '4CA2D1', y: { lat: 51.51, lon: -0.11, pt: now + 2, posSrc: 'ADS-B', alt: 31025, src: ['ADS-B'], st: { 'ADS-B': now + 2 }, lt: now + 2, n: 80 } }],
    g: [], stations: [] };
  const s2 = sharedSanitise(st.apply(d).json)!;
  const a2 = s2.aircraft.find((a) => a.key === '4CA2D1')!, b2 = s2.aircraft.find((a) => a.key === 'AB12CD')!;
  ok(a2.lat === 51.51 && a2.flight === 'RYR12' && a2.posAge === 1, 'a moved aircraft: its new position, its static part kept');
  ok(b2.age === 123 && b2.posAge === 123, 'an unchanged aircraft ages with the answer\'s now (123 s) — never re-sent');
  ok(s2.aircraft[0].key === '4CA2D1', 'the row this answer moved comes first (aircraft.json\'s newest-first)');
  const s3 = sharedSanitise(st.apply({ d: 1, c: 'ab.5', full: 0, now: now + 4, a: [{ k: 'AB12CD', s: { icao: 'AB12CD', flight: 'BAW9' }, y: { src: ['HFDL'], st: { HFDL: now + 4 }, lt: now + 4, n: 4 } }], g: ['4CA2D1'], stations: [] }).json)!;
  ok(s3.aircraft.length === 1 && s3.aircraft[0].flight === 'BAW9' && s3.aircraft[0].lat === undefined, 'a gone key goes; a renamed one\'s static part replaces the old (its position gone with its y)');
  const old = st.apply({ now, count: 1, aircraft: [{ key: 'X', src: [] }], stations: [] });
  ok(!!old.json && old.cursor === '', 'an older server\'s whole aircraft.json passes through (cursor "" — keep asking the old way)');
  const st2 = new AircraftDeltaStore();
  ok(st2.apply({ d: 1, c: 'x.9', full: 0, now, a: [], g: [] }).json === null && st2.cursor === '', 'a delta with no table to apply to: nothing drawn, start over');
  ok(st2.apply(null).json === null && st2.apply('junk').json === null, 'junk is ignored');
  const big = { d: 1, c: 'y.1', full: 1, now, a: Array.from({ length: 3000 }, (_, i) => ({ k: `K${i}`, s: { flight: `F${i}` }, y: { src: [], st: {}, lt: now, n: 1 } })), g: [] };
  ok((st2.apply(big).json!.aircraft as unknown[]).length <= 2048, 'bounded: never more than 2048 rows held');
  const off = st2.apply({ d: 1, off: 1, now, a: [], g: [] });
  ok((off.json as any)?.off === 1 && st2.size() === 0, 'the owner switching aircraft off: off:1, the table emptied');
}

console.log('── B. the landing button\'s gate ──');
{
  const mk = (o: Record<string, any>) => async (p: string) => {
    for (const [k, v] of Object.entries(o)) if (p.startsWith(k)) return v;
    return null;
  };
  const now = 1790000000;
  const none = await airSeaOffer(mk({ '/vibeserver/aircraft.json': { aircraft: [], stations: [] }, '/vibeserver/gallery/ais-ships.json': { vessels: [], count: 0, total: 0 } }), now);
  ok(!none.show, 'nothing anywhere: no button');
  const shipsOnly = await airSeaOffer(mk({ '/vibeserver/aircraft.json': { aircraft: [] }, '/vibeserver/gallery/ais-ships.json': { vessels: [{}], count: 1, total: 12 } }), now);
  ok(shipsOnly.show && shipsOnly.tab === 'ships' && /12 ships/.test(shipsOnly.line), `ships only: the button, opening on SHIPS ("${shipsOnly.line}")`);
  const sondes = await airSeaOffer(mk({ '/vibeserver/gallery.json?kind=sondes': { items: [{ t: now - 3600 }] } }), now);
  ok(sondes.show && sondes.tab === 'sondes', 'remembered radiosondes only (a day card under 24 h): the button, on SONDES');
  const oldSondes = await airSeaOffer(mk({ '/vibeserver/gallery.json?kind=sondes': { items: [{ t: now - 3 * 86400 }] } }), now);
  ok(!oldSondes.show, 'a sonde card older than its 24 h keep does not count');
  const vor = await airSeaOffer(mk({ '/vibeserver/gallery.json?kind=vormap': { items: [{ t: now - 3 * 86400 }] } }), now);
  ok(vor.show && vor.tab === 'navaids', 'VOR stations (7-day keep): the button, on NAVAIDS');
  const hfdl = await airSeaOffer(mk({ '/vibeserver/aircraft.json': { aircraft: [{ key: 'A', src: ['HFDL'] }, { key: 'B', src: ['HFDL'] }], feeds: [{ label: 'HFDL', title: 'UberSDR HFDL' }] } }), now);
  ok(hfdl.show && hfdl.tab === 'aircraft' && hfdl.label === 'HFDL' && hfdl.line === '2 HFDL aircraft now', `HFDL from a feed is all there is: labelled HFDL ("${hfdl.line}")`);
  const feedWait = await airSeaOffer(mk({ '/vibeserver/aircraft.json': { aircraft: [], feeds: [{ label: 'ADSB', title: 'PiAware' }] } }), now);
  ok(feedWait.show && feedWait.label === 'ADS-B', 'a feed with no aircraft yet still gets the button (the owner set one up)');
  const mixed = await airSeaOffer(mk({ '/vibeserver/aircraft.json': { aircraft: [{ key: 'A', src: ['ADS-B'] }], feeds: [{ label: 'HFDL' }] }, '/vibeserver/gallery/ais-ships.json': { total: 40, count: 1, vessels: [{}] } }), now);
  ok(mixed.tab === 'ships' && mixed.label === '', 'the most populated layer wins (40 ships over 1 aircraft); mixed sources: no label');
  const off = await airSeaOffer(mk({ '/vibeserver/aircraft.json': { aircraft: [], off: 1 }, '/vibeserver/gallery/ais-ships.json': { off: 1, total: 9 } }), now);
  ok(!off.show, 'the owner switched aircraft and AIS off: no button');

  /* ★★ V12 11-27 (Stuart: "lies on the landing screen" — the Sony's button said "243 aircraft", the map it opened "72
   *  aircraft (66 on the map)"): the button counted aircraft.json?since=3600, every aircraft heard in the HOUR. It now counts
   *  what the map draws when it opens — the same rule (aviationMap.ts avShown: ADS-B only while heard in the last minute and
   *  a radio is decoding it, the datalink within the table's hour), so the two can never disagree. */
  const T = 1790000000;
  const fixture = {
    now: T, count: 11, adsbLive: 1, adsbLast: 0, adsbNow: 4, aircraft: [
      // fresh ADS-B (heard seconds ago): on the map
      { key: '400001', icao: '400001', lat: 52.1, lon: -1.1, posSrc: 'ADS-B', posAge: 2, src: ['ADS-B'], srcAge: { 'ADS-B': 2 }, age: 2, n: 40 },
      { key: '400002', icao: '400002', lat: 52.2, lon: -1.2, posSrc: 'ADS-B', posAge: 9, src: ['ADS-B'], srcAge: { 'ADS-B': 9 }, age: 9, n: 40 },
      { key: '400003', icao: '400003', src: ['ADS-B'], srcAge: { 'ADS-B': 30 }, age: 30, n: 3 },   // heard, no position yet: counted, not placed
      // ADS-B only, not heard for minutes (the hour's leftovers): NOT on the map
      { key: '400004', icao: '400004', lat: 52.4, lon: -1.4, posSrc: 'ADS-B', posAge: 600, src: ['ADS-B'], srcAge: { 'ADS-B': 600 }, age: 600, n: 90 },
      { key: '400005', icao: '400005', lat: 52.5, lon: -1.5, posSrc: 'ADS-B', posAge: 1800, src: ['ADS-B'], srcAge: { 'ADS-B': 1800 }, age: 1800, n: 90 },
      { key: '400006', icao: '400006', lat: 52.6, lon: -1.6, posSrc: 'ADS-B', posAge: 3500, src: ['ADS-B'], srcAge: { 'ADS-B': 3500 }, age: 3500, n: 90 },
      { key: '400007', icao: '400007', lat: 52.7, lon: -1.7, posSrc: 'ADS-B', posAge: 61, src: ['ADS-B'], srcAge: { 'ADS-B': 61 }, age: 61, n: 90 },
      // ADS-B gone stale but VDL2 heard it: stays for its datalink
      { key: '400008', icao: '400008', lat: 52.8, lon: -1.8, posSrc: 'ADS-B', posAge: 900, src: ['ADS-B', 'VDL2'], srcAge: { 'ADS-B': 900, VDL2: 120 }, age: 120, n: 12 },
      // datalink within the hour: on the map (bursts minutes apart)
      { key: '400009', icao: '400009', lat: 50.0, lon: -20.0, posSrc: 'HFDL', posAge: 2400, src: ['HFDL'], srcAge: { HFDL: 2400 }, age: 2400, n: 2 },
      { key: 'r:GEZWK', reg: 'G-EZWK', src: ['ACARS'], srcAge: { ACARS: 1500 }, age: 1500, n: 1 },
      { key: '40000A', icao: '40000A', lat: 53.0, lon: -2.0, posSrc: 'VDL2', posAge: 300, src: ['VDL2'], srcAge: { VDL2: 300 }, age: 300, n: 4 },
    ], stations: [],
  };
  const mapN = (j: any) => avCompose(null, sharedSanitise(j), 'adsb', avLayersOf(null), null, 'app').planes.length;
  const standaloneStatus = (j: any) => composeAirSeaStandalone({
    shared: sharedSanitise(j), shipStore: null, dscGal: [], navReport: null, navWarnRep: null, navMetRep: null, navRange: 'all', sdStored: [], vmStored: [],
    avLayers: avLayersOf(null), shipLayers: shipLayersOf(null), rx: null, view: null, picked: null, opened: null, sort: 'nearest', msgKey: '', clock: '', nowMs: T * 1000,
  }, { setPicked: () => {}, setSort: () => {}, setMsgKey: () => {}, setAvLayers: () => {}, setShipLayers: () => {}, setNavRange: () => {}, onView: () => {},
       cardText: async () => ({ ok: false, text: '' }) }, newMsgFeed()) as any;
  const sony = await airSeaOffer(mk({ '/vibeserver/aircraft.json': fixture }), T);
  const n = mapN(fixture);
  ok(n === 7, `the fixture: 11 heard in the hour, 7 the map draws now (got ${n})`);
  ok(sony.counts.aircraft === n, `the button counts what the map shows (${sony.counts.aircraft} = ${n}), not the hour's 11`);
  ok(sony.line === '7 aircraft now', `…and says "now" ("${sony.line}")`);
  const st = JSON.stringify(standaloneStatus(fixture));
  ok(st.includes('7 aircraft ('), 'the standalone map it opens says the same 7 aircraft');
  // the ADS-B radio taken for something else: the ADS-B aircraft leave the map AND the button
  const busy = { ...fixture, adsbLive: 0, adsbLast: 300, adsbNow: 0 };
  const sonyBusy = await airSeaOffer(mk({ '/vibeserver/aircraft.json': busy }), T);
  ok(sonyBusy.counts.aircraft === mapN(busy) && mapN(busy) === 4, `no radio decoding ADS-B: the button and the map both say ${mapN(busy)} (the datalink's)`);
  // only stale ADS-B left from the hour: nothing to draw → no aircraft on the button (and no button without ships / a feed)
  const stale = { now: T, adsbLive: 1, adsbLast: 0, aircraft: fixture.aircraft.slice(3, 7), stations: [] };
  const sonyStale = await airSeaOffer(mk({ '/vibeserver/aircraft.json': stale }), T);
  ok(sonyStale.counts.aircraft === 0 && mapN(stale) === 0 && !sonyStale.show, 'only the hour\'s stale ADS-B: 0 now, no button (the map would be empty)');
  // a feed's label is judged on the aircraft drawn now: stale ADS-B rows do not spoil an HFDL feed's label
  const hfdlNow = await airSeaOffer(mk({ '/vibeserver/aircraft.json': { now: T, adsbLive: 1, aircraft: [fixture.aircraft[8], fixture.aircraft[4]], feeds: [{ label: 'HFDL', title: 'UberSDR HFDL' }], stations: [] } }), T);
  ok(hfdlNow.label === 'HFDL' && hfdlNow.line === '1 HFDL aircraft now', `the feed's label on what is drawn now ("${hfdlNow.line}")`);
}

console.log('── C. the viewer session ──');
{
  let t = 1_000_000;
  const clock = () => t;
  const calls: string[] = [];
  let joinAns: any = { ok: true, vt: 'aaaaaaaaaaaaaaaaaaaa', n: 1, max: 20, limit: 1800, left: 1800, warn: 60 };
  let pollAns: (p: string) => any = (p) => ({ d: 1, c: 'e.1', full: 1, now: 1790000000, count: 0, a: [], g: [], stations: [], viewer: { left: 1800, n: 1 } });
  const get = async (p: string) => { calls.push(p); return p.startsWith('/vibeserver/airsea/join') ? joinAns : p.startsWith('/vibeserver/airsea/leave') ? { ok: true } : pollAns(p); };
  const v = new AirSeaViewer(get, clock);
  ok(await v.join() && v.ticket() === 'aaaaaaaaaaaaaaaaaaaa', 'open: a ticket from /vibeserver/airsea/join');
  await v.poll();
  ok(/aircraft\.json\?d=&vt=aaaaaaaaaaaaaaaaaaaa/.test(calls[calls.length - 1]), `the first poll asks for everything with the ticket (${calls[calls.length - 1]})`);
  await v.poll();
  ok(/d=e\.1&vt=/.test(calls[calls.length - 1]), 'the next carries the cursor');
  ok(v.status().clock === 'Map time ends in 30:00' && v.status().banner === '', `a 30-minute limit: the clock ("${v.status().clock}"), no banner yet`);
  t += (30 * 60 - 50) * 1000;
  ok(/Map time ends in 0:50/.test(v.status().banner), `the last minute: the banner ("${v.status().banner}")`);
  pollAns = () => ({ viewer: 'ended', limit: 1800 });
  ok(await v.poll() === null && v.state === 'ended' && /time on the map is up/.test(v.status().banner), 'ENDED: the viewer stops, and says why');
  calls.length = 0;
  ok(await v.poll() === null && calls.length === 0, '…and polls no more');
  // full
  joinAns = { ok: false, full: true, n: 20, max: 20 };
  const f = new AirSeaViewer(get, clock);
  ok(!(await f.join()) && f.status().refusal === 'The map is full (20/20) — try again shortly', `the map full: "${f.status().refusal}"`);
  joinAns = { ok: false, ended: true, limit: 1800, back: 1500 };
  const e = new AirSeaViewer(get, clock);
  ok(!(await e.join()) && /25 minutes/.test(e.status().refusal), `a used-up turn: "${e.status().refusal}"`);
  // the server forgot the ticket
  joinAns = { ok: true, vt: 'bbbbbbbbbbbbbbbbbbbb', n: 2, max: 20, limit: 0, left: -1 };
  let first = true;
  pollAns = (p) => { if (first) { first = false; return { viewer: 'unknown' }; } return { d: 1, c: 'e.2', full: 1, now: 1, a: [], g: [], stations: [] }; };
  const u = new AirSeaViewer(get, clock);
  await u.join(); first = true;
  const got = await u.poll();
  ok(!!got && u.ticket() === 'bbbbbbbbbbbbbbbbbbbb', 'the server forgot the ticket (a restart): it joins again and carries on');
  ok(u.status().clock === '' && u.left() === -1, 'no time limit: no clock');
  // an older server: no join, no deltas
  joinAns = null;
  pollAns = () => ({ now: 1, count: 1, aircraft: [{ key: 'A', src: [] }], stations: [] });
  const o = new AirSeaViewer(get, clock);
  ok(await o.join() && o.ticket() === '', 'an older server (no tickets): the map opens');
  const oj = await o.poll();
  ok(!!oj && Array.isArray((oj as any).aircraft), 'its whole aircraft.json is used');
  calls.length = 0; await o.poll();
  ok(calls[0] === '/vibeserver/aircraft.json', `…and it stops asking for deltas (${calls[0]})`);
  // leave
  joinAns = { ok: true, vt: 'cccccccccccccccccccc', n: 1, max: 0, limit: 0, left: -1 };
  const l = new AirSeaViewer(get, clock);
  await l.join(); calls.length = 0; await l.leave();
  ok(calls[0] === '/vibeserver/airsea/leave?vt=cccccccccccccccccccc' && l.state === 'gone', 'CLOSE MAP ends the ticket at once');
  ok(mapClockText(61) === 'Map time ends in 1:01' && mapFullText(3, 3).includes('(3/3)') && /1 minute each/.test(mapEndedText(60)), 'the words');
}

console.log(`\ntest_airsea_viewer: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
}
void main();
