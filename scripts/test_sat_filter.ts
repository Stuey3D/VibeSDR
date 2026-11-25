// test_sat_filter.ts — ★★ beta94 SHOW (Stuart 11-25: "on the satellite list can we have a dropdown to show only passes from
// certain satellites"), the shared rules in src/services/satellites.ts that the web page and the app both use: the groups
// worked out from each downlink (no satellite named), the tick boxes, group shortcuts, what is stored (by NORAD number,
// carried server to server, a missing bird ignored), the header's words, the rows kept (the pick always stays) and the
// reminders' check.
//     npx tsx scripts/test_sat_filter.ts
process.env.TZ = 'UTC';
import {
  parseSatList, satModeRows, SAT_SHOW_GROUPS, satShowChoices, satShowGroup, satShowGroupsOf, satShowGroupsOffered, satShowParse,
  satShowRemindOk, satShowRows, satShowShows, satShowStore, satShowText, satShowToggle, satTxShowGroups, type SatList, type SatShow, type SatTx,
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

console.log('— reminders follow SHOW');
ok(satShowRemindOk(null, L, 25544), 'All: every reminder fires');
ok(!satShowRemindOk(W, L, 25544) && satShowRemindOk(W, L, 57166), 'Weather: the ISS reminder does not, METEOR\'s does');
ok(!satShowRemindOk({ norads: [57166] }, L, 25544) && satShowRemindOk({ norads: [25544] }, L, 25544), 'a list: only ticked birds');
ok(satShowRemindOk(W, L, 12345), 'a bird the list no longer has is let through (the fire-time check says why)');

console.log('— the empty list');
ok(satShowChoices(null, parseSatList(raw([]))).length === 0 && satShowChoices(null, null).length === 0, 'no satellites → no tick boxes (the drop-down is not drawn)');

console.log(fail ? `\n${fail} FAILED, ${pass} passed` : `\nall ${pass} passed`);
process.exit(fail ? 1 : 0);
