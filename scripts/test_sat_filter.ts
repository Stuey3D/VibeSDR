// test_sat_filter.ts — ★★ beta94 SHOW (Stuart 11-25: "on the satellite list can we have a dropdown to show only passes from
// certain satellites"), the shared rules in src/services/satellites.ts that the web page and the app both use: the groups
// worked out from each downlink (no satellite named), the tick boxes, group shortcuts, what is stored (by NORAD number,
// carried server to server, a missing bird ignored), the header's words, the rows kept (the pick always stays) and the
// reminders' check.
//     npx tsx scripts/test_sat_filter.ts
process.env.TZ = 'UTC';
import {
  parseSatList, satModeRows, SAT_SHOW_GROUPS, satShowChoices, satShowGroup, satShowGroupsOf, satShowGroupsOffered, satShowParse,
  satShowRows, satShowShows, satShowStore, satShowText, satShowToggle, satTxShowGroups, type SatList, type SatShow, type SatTx,
  SAT_SHOW_EMPTY, SAT_SHOW_QS, satShowCur, satShowEmptyText, satShowHiddenRemindText, satShowHidesPass, satShowPassOk, satShowQ, satShowWithQ,
  SatReminders, satRemindKeyText, SAT_REMIND_FORGET_WHAT, type SatPass,
} from '../src/services/satellites';

let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };
const eq = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);

const now = Date.UTC(2026, 10, 25, 12, 0, 0) / 1000;
const pas = (inMin: number) => [{ aos: now + inMin * 60, tca: now + inMin * 60 + 300, los: now + inMin * 60 + 600, maxEl: 40, aosAz: 0, losAz: 180 },
                                 { aos: now + (inMin + 100) * 60, tca: now + (inMin + 105) * 60, los: now + (inMin + 110) * 60, maxEl: 20, aosAz: 0, losAz: 180 }];
// the shapes the server's sat_list sends (vibe_sat_engine.h listJson) — the catalogue's own kinds
const raw = (sats: unknown[]) => ({ type: 'sat_list', time: 'ok', located: true, now, source: { orbits: 'x', via: 'vibesdr.net', pickPrivate: true }, sats });
const METEOR = { id: 'M', name: 'METEOR-M2 3', norad: 57166, note: '', age: 0.5, ageState: 'fresh', passes: pas(10),
  tx: [{ label: 'LRPT', hz: 137900000, mode: 'lrpt', loop: 'costas', consumer: 'lrpt', use: 'image', decoder: 'lrpt', reach: true }] };
const ISS = { id: 'ISS', name: 'ISS', norad: 25544, note: '', age: 0.2, ageState: 'fresh', passes: pas(20),
  tx: [{ label: 'FM repeater', hz: 437800000, mode: 'nfm', loop: 'fll', use: 'voice', reach: true },
       { label: 'APRS', hz: 145825000, mode: 'nfm', loop: 'fll', consumer: 'ax25-1200', use: 'packet', decoder: 'aprs', reach: true }] };
const RS44 = { id: 'RS-44', name: 'RS-44', norad: 44909, note: '', age: 1, ageState: 'fresh', passes: pas(30),
  tx: [{ label: 'Transponder', hz: 435610000, hiHz: 435670000, mode: 'usb', loop: 'none', use: 'voice', decoder: 'xpdr', reach: true },
       { label: 'CW beacon', hz: 435605000, mode: 'cw', loop: 'none', use: 'voice', decoder: 'xpdr', reach: true }] };
const AO73 = { id: 'AO-73', name: 'AO-73 (FUNcube-1)', norad: 39444, note: '', age: 1, ageState: 'fresh', passes: pas(40),
  tx: [{ label: 'Transponder', hz: 145950000, hiHz: 145970000, mode: 'usb', loop: 'none', use: 'voice', reach: true },
       { label: 'Telemetry', hz: 145935000, mode: 'usb', loop: 'none', consumer: 'fc-bpsk', use: 'voice', decoder: 'fctlm', reach: true }] };
const SO50 = { id: 'SO-50', name: 'SO-50', norad: 27607, note: '', age: 1, ageState: 'fresh', passes: pas(50),
  tx: [{ label: 'FM repeater', hz: 436795000, mode: 'nfm', loop: 'fll', use: 'voice', reach: true }] };
const L: SatList = parseSatList(raw([METEOR, ISS, RS44, AO73, SO50]))!;
const tx = (o: Partial<SatTx> & { hz: number }): SatTx => ({ label: 'x', mode: 'nfm', loop: 'fll', reach: true, ...o });

console.log('— the groups, from each downlink (no satellite named)');
ok(eq(satTxShowGroups(tx({ hz: 1, mode: 'lrpt', use: 'image' })), ['weather']), 'an image downlink (LRPT) → Weather');
ok(eq(satTxShowGroups(tx({ hz: 1, mode: 'lrpt' })), ['weather']), '…also from an older server (no use: worked out from the mode)');
ok(eq(satTxShowGroups(tx({ hz: 1, hiHz: 2, mode: 'usb', use: 'voice' })), ['xpdr']), 'a passband (linear) → Transponders');
ok(eq(satTxShowGroups(tx({ hz: 1, mode: 'nfm', use: 'voice' })), ['fm']), 'an FM voice downlink → FM voice');
ok(eq(satTxShowGroups(tx({ hz: 1, mode: 'nfm', use: 'packet', consumer: 'ax25-1200' })), ['tlm']), 'APRS packet → Telemetry (not FM voice)');
ok(eq(satTxShowGroups(tx({ hz: 1, mode: 'usb', use: 'voice', consumer: 'fc-bpsk', decoder: 'fctlm' })), ['tlm']), 'BPSK telemetry → Telemetry');
ok(eq(satTxShowGroups(tx({ hz: 1, mode: 'cw', use: 'voice' })), ['tlm']), 'a CW beacon → Telemetry');
ok(eq(satTxShowGroups(tx({ hz: 1, mode: 'cw', use: 'voice', decoder: 'xpdr' })), ['xpdr', 'tlm']), 'a transponder\'s own beacon → Transponders and Telemetry');
ok(eq(satTxShowGroups(tx({ hz: 1, mode: 'orbcomm', use: 'packet', consumer: 'orbcomm', decoder: 'orbcomm' })), ['tlm']), 'ORBCOMM data → Telemetry');
ok(eq(satShowGroupsOf(L.sats[1]), ['fm', 'tlm']), 'a bird is in every group any downlink is in (ISS: FM voice + Telemetry)');
ok(eq(satShowGroupsOf(L.sats[3]), ['xpdr', 'tlm']), 'AO-73: Transponders + Telemetry');
ok(eq(satShowGroupsOffered(L), ['weather', 'xpdr', 'fm', 'tlm']), 'every group offered on a list that has each');
ok(eq(satShowGroupsOffered(parseSatList(raw([SO50]))), ['fm']), 'a group no listed bird is in is not offered');
ok(eq(SAT_SHOW_GROUPS.map((g) => g.label), ['Weather', 'Transponders', 'FM voice', 'Telemetry']), 'the shortcuts\' words');

console.log('— All (the default) changes nothing');
const rows = satModeRows(L, now);
ok(satShowRows(rows, null) === rows, 'null = All: the same rows, untouched');
ok(satShowParse(null) === null && satShowParse('') === null && satShowParse('rubbish{') === null && satShowParse('{"group":"nope"}') === null, 'nothing / unreadable stored = All');
ok(satShowText(null, L) === 'All', 'header: "All"');
ok(satShowChoices(null, L).every((c) => c.on) && satShowChoices(null, L).length === 5, 'every tick box ticked');

console.log('— a group shortcut');
const W = satShowGroup('weather');
ok(eq(W, { group: 'weather' }) && satShowGroup('all') === null, 'the shortcut is kept AS the group');
ok(satShowText(W, L) === 'Weather', 'header: "Weather"');
const wr = satShowRows(rows, W);
ok(wr.length > 0 && wr.every((r) => r.sat.norad === 57166), `only METEOR's passes (${wr.length} rows)`);
ok(eq(satShowChoices(W, L).filter((c) => c.on).map((c) => c.norad), [57166]), 'only METEOR ticked');
const T = satShowGroup('tlm');
ok(eq([...new Set(satShowRows(rows, T).map((r) => r.sat.norad))].sort(), [25544, 39444, 44909].sort()), 'Telemetry: ISS (APRS), RS-44 (beacon), AO-73 (BPSK)');
ok(satShowRows(rows, T).filter((r) => r.sat.norad === 25544).length === rows.filter((r) => r.sat.norad === 25544).length, '…a shown bird keeps ALL its rows (the ISS FM repeater too)');
ok(satShowText(satShowGroup('xpdr'), L) === 'Transponders' && satShowText(satShowGroup('fm'), L) === 'FM voice', 'header names the group');
// a group on ANOTHER server means that server's birds
const L2 = parseSatList(raw([SO50, { ...METEOR, norad: 40069, name: 'METEOR-M2' }]))!;
ok(eq([...new Set(satShowRows(satModeRows(L2, now), W).map((r) => r.sat.norad))], [40069]), 'Weather on another server = its own weather bird');

console.log('— tick boxes');
let f: SatShow = satShowToggle(null, L, 27607);   // untick SO-50 from All
ok(eq(f, { norads: [57166, 25544, 44909, 39444] }), `untick one from All → the rest, by NORAD: ${JSON.stringify(f)}`);
ok(satShowText(f, L) === '4 satellites', 'header: "4 satellites"');
ok(!satShowRows(rows, f).some((r) => r.sat.norad === 27607), 'its passes are gone');
ok(satShowToggle(f, L, 27607) === null, 'tick it again → back to All');
f = satShowToggle(W, L, 25544);   // from Weather, tick ISS
ok(eq(f, { norads: [57166, 25544] }), `from a group, a tick adds to the group's birds: ${JSON.stringify(f)}`);
ok(satShowText(f, L) === '2 satellites', 'header: "2 satellites"');
f = satShowToggle(f, L, 57166);
ok(eq(f, { norads: [25544] }) && satShowText(f, L) === 'ISS', 'one ticked: the header names it ("ISS")');
f = satShowToggle(f, L, 25544);
ok(eq(f, { norads: [] }) && satShowText(f, L) === 'None' && satShowRows(rows, f).length === 0, 'none ticked: "None", no rows');

console.log('— stored by NORAD, across servers');
const iss: SatShow = { norads: [25544, 99999] };   // 99999: a bird only another server lists
ok(satShowStore(null) === null, 'All stores nothing (the key is removed)');
ok(eq(satShowParse(satShowStore(iss)), iss) && eq(satShowParse(satShowStore(W)), W), 'store → parse round trip (list and group)');
ok(eq(satShowParse('{"norads":[25544,25544,-3,"x",1.5,7530]}'), { norads: [25544, 7530] }), 'stored list cleaned (duplicates, junk dropped)');
ok(satShowText(iss, L) === 'ISS', 'a ticked bird this server does not list is ignored in the header');
ok(eq([...new Set(satShowRows(rows, iss).map((r) => r.sat.norad))], [25544]), '…and in the rows');
ok(eq(satShowToggle(iss, L, 27607), { norads: [25544, 99999, 27607] }), 'a tick here keeps the other server\'s bird');
ok(eq(satShowToggle({ norads: [25544, 57166, 44909, 39444, 99999] }, L, 27607), { norads: [25544, 57166, 44909, 39444, 99999, 27607] }),
   '…so every bird here ticked PLUS another server\'s is still a list, not All');
ok(satShowText({ norads: [25544, 57166, 44909, 39444, 99999, 27607] }, L) === 'All', '…whose header here reads "All" (all of them here)');

console.log('— the pick always stays');
ok(satShowRows(rows, W, 25544).some((r) => r.sat.norad === 25544), 'Weather, ISS picked: ISS stays in the list');
ok(satShowShows(W, L.sats[1], 25544) && !satShowShows(W, L.sats[1], null), 'satShowShows: kept only while picked');
ok(satShowRows(rows, { norads: [] }, 44909).every((r) => r.sat.norad === 44909) && satShowRows(rows, { norads: [] }, 44909).length > 0, 'None ticked, RS-44 picked: only RS-44');

console.log('— SHOW filters the LIST only: every reminder set fires (Stuart 11-25)');
ok(!satShowHidesPass(null, L, 25544, now + 20 * 60), 'All: no pass hidden');
ok(satShowHidesPass(W, L, 25544, now + 20 * 60) && !satShowHidesPass(W, L, 57166, now + 10 * 60), 'Weather: an ISS pass is hidden, METEOR\'s is not');
ok(!satShowHidesPass(W, L, 25544, now + 20 * 60, 25544), '…the picked bird is never hidden');
ok(!satShowHidesPass(W, L, 12345, now), 'a bird the list does not have is not "hidden" (its fire-time check says why)');
ok(satShowHiddenRemindText(null, L, [{ norad: 25544, aos: now + 20 * 60 }]) === '', 'no note when nothing is hidden');
ok(satShowHiddenRemindText(W, L, [{ norad: 25544, aos: now + 20 * 60 }, { norad: 57166, aos: now + 10 * 60 }]) === '1 reminder set on a hidden pass', 'Weather + an ISS reminder: "1 reminder set on a hidden pass"');
ok(satShowHiddenRemindText({ norads: [] }, L, [{ norad: 25544, aos: now + 20 * 60 }, { norad: 27607, aos: now + 50 * 60 }]) === '2 reminders set on hidden passes', 'two: "2 reminders set on hidden passes"');
// the book itself never looks at SHOW: a reminder set, SHOW then hiding its pass, still comes due
{
  const book = new SatReminders(), so = satModeRows(L, now).find((r) => r.sat.norad === 27607 && !r.later)!;
  ok(book.add('srv', so, now), 'a reminder set on SO-50\'s next pass');
  const f2: SatShow = { group: 'weather', q: 'EXCELLENT' };   // SHOW then hides it (not weather, and a 40° pass)
  ok(satShowRows(rows, f2).every((r) => r.sat.norad !== 27607), '…SHOW then hides that pass');
  const due = book.due(so.next!.aos - 120, 2, true);
  ok(due.length === 1 && due[0].norad === 27607, '…and it still fires (hiding never cancels a reminder)');
}

console.log('— REMIND is a toggle: REMIND / lit FORGET');
{
  const book = new SatReminders(), so = satModeRows(L, now).find((r) => r.sat.norad === 27607 && !r.later)!;
  ok(satRemindKeyText(false) === 'REMIND' && satRemindKeyText(!!book.find('srv', 27607, 0)) === 'REMIND', 'not set: "REMIND"');
  book.add('srv', so, now);
  ok(satRemindKeyText(!!book.find('srv', 27607, 0)) === 'FORGET' && SAT_REMIND_FORGET_WHAT === 'Reminder set — tap to forget it', 'set: "FORGET", "Reminder set — tap to forget it"');
  book.add('srv', so, now);   // pressed again through add (a double tap that missed the toggle) — still one
  ok(book.size === 1, 'adding the same pass twice holds ONE reminder, not two');
  const held = book.find('srv', 27607, 0)!; book.cancel(held.key);   // what FORGET does (both clients: find → cancel)
  ok(book.size === 0 && satRemindKeyText(!!book.find('srv', 27607, 0)) === 'REMIND', 'FORGET: cancelled, back to "REMIND"');
  ok(book.due(so.next!.aos - 120, 2, true).length === 0, '…and nothing fires');
}

console.log('— QUALITY: the pass\'s rating, at or above a level');
// a list whose passes cover every rating: 10° POOR, 20° MARGINAL, 40° GOOD, 60° EXCELLENT — one bird each
const pp = (inMin: number, maxEl: number): SatPass => ({ aos: now + inMin * 60, tca: now + inMin * 60 + 300, los: now + inMin * 60 + 600, maxEl, aosAz: 0, losAz: 180 });
const Q = parseSatList(raw([{ ...METEOR, passes: [pp(10, 10), pp(110, 60)] }, { ...ISS, passes: [pp(20, 20)] }, { ...SO50, passes: [pp(30, 40)] }]))!;
const qr = satModeRows(Q, now);
const els = (f: SatShow, keep?: number | null) => satShowRows(qr, f, keep).map((r) => Math.round(r.pass!.maxEl)).sort((a, b) => a - b);
ok(eq(SAT_SHOW_QS.map((x) => x.label), ['Marginal or better', 'Good or better', 'Excellent only']), 'the choices\' words');
ok(eq(els(null), [10, 20, 20, 40, 60]), `All passes: every row (${els(null)})`);
ok(eq([...new Set(els({ q: 'MARGINAL' }))], [20, 40, 60]), 'Marginal or better: 15° and up');
ok(eq([...new Set(els({ q: 'GOOD' }))], [40, 60]), 'Good or better: 30° and up');
ok(eq(els({ q: 'EXCELLENT' }), [60]), 'Excellent only: 50° and up');
ok(satShowPassOk({ q: 'GOOD' }, 30) && !satShowPassOk({ q: 'GOOD' }, 29.9) && satShowPassOk({ q: 'EXCELLENT' }, 50) && !satShowPassOk({ q: 'MARGINAL' }, 14.9),
   'the thresholds are satPassQuality\'s (15 / 30 / 50°)');
ok(!satShowPassOk({ q: 'MARGINAL' }, undefined) && satShowPassOk(null, undefined), 'a row with no pass to rate goes only while a floor is set');
ok(eq(els({ group: 'weather', q: 'EXCELLENT' }), [60]) && eq(els({ group: 'weather', q: 'GOOD' }), [60]), 'AND with a group: Weather · Good or better = METEOR\'s 60° pass only');
ok(eq(els({ norads: [25544, 27607], q: 'GOOD' }), [40]), 'AND with ticked satellites: ISS + SO-50, Good or better = SO-50\'s 40°');
ok(eq(els({ q: 'EXCELLENT' }, 25544), [20, 20, 60]), 'the picked bird keeps its passes whatever their quality');
ok(satShowText({ group: 'weather', q: 'GOOD' }, Q) === 'Weather · Good or better', 'header: "Weather · Good or better"');
ok(satShowText({ q: 'EXCELLENT' }, Q) === 'Excellent only' && satShowText({ norads: [25544], q: 'MARGINAL' }, Q) === 'ISS · Marginal or better', 'header: "Excellent only" / "ISS · Marginal or better"');
ok(satShowCur({ q: 'GOOD' }) === 'all' && satShowCur({ group: 'fm', q: 'GOOD' }) === 'fm' && satShowCur({ norads: [1], q: 'GOOD' }) === '', 'the lit shortcut ignores the quality');
console.log('— QUALITY keeps the satellite choice, and the other way round');
ok(eq(satShowWithQ(W, 'GOOD'), { group: 'weather', q: 'GOOD' }) && eq(satShowWithQ({ group: 'weather', q: 'GOOD' }, null), W), 'setting / clearing quality keeps the group');
ok(eq(satShowWithQ(null, 'GOOD'), { q: 'GOOD' }) && satShowWithQ({ q: 'GOOD' }, null) === null, 'from All: { q } alone; cleared = All (null)');
ok(eq(satShowGroup('fm', { q: 'GOOD' }), { group: 'fm', q: 'GOOD' }) && eq(satShowGroup('all', { group: 'fm', q: 'GOOD' }), { q: 'GOOD' }), 'a group shortcut keeps the quality');
ok(eq(satShowToggle({ q: 'GOOD' }, L, 27607), { norads: [57166, 25544, 44909, 39444], q: 'GOOD' }), 'a tick box keeps the quality');
ok(eq(satShowToggle({ norads: [57166, 25544, 44909, 39444], q: 'GOOD' }, L, 27607), { q: 'GOOD' }), '…and ticking every bird back = All satellites, quality kept');
ok(satShowQ(W) === null && satShowQ({ group: 'weather', q: 'EXCELLENT' }) === 'EXCELLENT', 'satShowQ');
console.log('— QUALITY remembered (same key), older stored values still load');
for (const f of [{ q: 'GOOD' }, { group: 'weather', q: 'EXCELLENT' }, { norads: [25544, 99999], q: 'MARGINAL' }] as SatShow[])
  ok(eq(satShowParse(satShowStore(f)), f), `round trip ${satShowStore(f)}`);
ok(satShowStore({ q: 'GOOD' }) === '{"q":"GOOD"}', 'stored as { q } alongside the rest');
ok(eq(satShowParse('{"group":"weather"}'), { group: 'weather' }) && eq(satShowParse('{"norads":[25544]}'), { norads: [25544] }), 'a value stored before QUALITY (no q) loads as it did');
ok(eq(satShowParse('{"group":"weather","q":"POOR"}'), { group: 'weather' }) && eq(satShowParse('{"q":"nope"}'), null), 'an unknown / POOR quality is dropped (every pass)');
console.log('— QUALITY: the empty list says why');
ok(satShowEmptyText({ q: 'GOOD' }, satModeRows(parseSatList(raw([{ ...ISS, passes: [pp(20, 20)] }])), now)) === 'No passes Good or better in the next 24 h', '"No passes Good or better in the next 24 h"');
ok(satShowEmptyText({ q: 'EXCELLENT' }, qr.filter((r) => r.pass!.maxEl < 50)) === 'No Excellent passes in the next 24 h', '"No Excellent passes in the next 24 h"');
ok(satShowEmptyText({ norads: [] , q: 'GOOD' }, qr) === SAT_SHOW_EMPTY && satShowEmptyText({ norads: [] }, qr) === SAT_SHOW_EMPTY, 'no satellites chosen at all: the satellite note, not the quality one');
ok(satShowHidesPass({ q: 'GOOD' }, Q, 25544, now + 20 * 60) && !satShowHidesPass({ q: 'GOOD' }, Q, 27607, now + 30 * 60), 'a 20° pass is hidden under Good or better (for the reminder note)');
ok(satShowHiddenRemindText({ q: 'GOOD' }, Q, [{ norad: 25544, aos: now + 20 * 60 }]) === '1 reminder set on a hidden pass', '…so its reminder is noted (and still fires)');

console.log('— the empty list');
ok(satShowChoices(null, parseSatList(raw([]))).length === 0 && satShowChoices(null, null).length === 0, 'no satellites → no tick boxes (the drop-down is not drawn)');

console.log(fail ? `\n${fail} FAILED, ${pass} passed` : `\nall ${pass} passed`);
process.exit(fail ? 1 : 0);
