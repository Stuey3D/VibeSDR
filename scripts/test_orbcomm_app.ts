// test_orbcomm_app.ts — V12 2026-11-24 ORBCOMM + THE SKY PLOT, the clients' half (src/services/orbcomm.ts,
// src/utils/skyPlot.ts, src/services/satellites.ts): orbcomm_status / orbcomm_pkt parsed defensively (junk → null, never a
// throw), the sky plot's geometry and the server's SNR ramp, skyplot.json with its radio ids and grey dots, the listening
// mode and decoder of an ORBCOMM pick, and the words a subscriber message is shown with (never decoded — raw bytes only).
// The server side is vibeserver/test-orbcomm.cpp (the decoder) and test-sat-labels.cpp (the picks).
// Silent: no audio. Run: npx tsx scripts/test_orbcomm_app.ts
import fs from 'node:fs';
import { ORB_CAVEAT, orbAttachParams, orbCountsText, orbHeadText, orbHowText, orbPktLine, orbSatLines, orbViewMerge, parseOrbcommPkt, parseOrbcommStatus } from '../src/services/orbcomm';
import { parseSkyDotsLive, parseSkyPlotJson, skyPlotMerge, skyPlotQuery, skyPlotScene, skyPlotSvg, skyPlotXY, skyRadioFromBase, skySnrRgb, SKY_GREY } from '../src/utils/skyPlot';
import { parseSatList, parseSatPicked, satDecView, satDecWord, satDecoderFor, txListenMode, txUse } from '../src/services/satellites';
import { REACH_DECODERS } from '../src/services/sdrTypes';
import { CLIENT_DECODERS } from '../src/constants/modeBox';
import { orbHeldText, orbShownSats, orbViewEmpty, ORB_HOLD_SEC } from '../src/services/orbcomm';
import { orbOverlayFrom, orbOverlayKey, orbOverlayLayout, ORB_NO_TUNE, ORB_SLICE_LABEL } from '../src/utils/orbcommView';

let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };
const near = (a: number, b: number, e = 1e-6) => Math.abs(a - b) <= e;
const noThrow = (f: () => unknown) => { try { f(); return true; } catch { return false; } };

console.log('── orbcomm_status ──');
// as vibe_orbcomm_run.h tick_ writes it
const status = {
  t: 1795942863, good: 341, bad: 12, messages: 5, dropped: 0, maxChans: 4, located: true, birds: 8,
  counts: { sync: 40, fill: 900, ephemeris: 12, uplink: 3, downlink: 3, ncc: 1, elements: 2, message: 5 },
  sats: [{ norad: 41185, how: 'position', carrier: 0, rf: 137287500, snr: 12.4, dop: -2310, pred: -2290, az: 123.4, el: 34.5, dcn: 5,
           nominal: 137287500, scId: 26, ok: 120, messages: 4, age: 1, name: 'ORBCOMM FM114',
           eph: { lat: 51.2, lon: -0.4, altKm: 742.3, kmS: 7.081, clock: 1795942862, id: 26, week: 2394, tow: 432000 },
           down: { part: 1, of: 2, hz: [137287500, 137737500] }, up: { hz: [148010000, 148030000], retries: 3, slots: 7 },
           elements: { sc: 26, ma: 123.4567, mm: 14.12345678 } }],
  carriers: [{ id: 0, rf: 137287500, snr: 12.4, lock: true, ok: 120, bad: 3, fixed: 2 }],
  pick: 41185, up: 1,
  dots: [[1234, 345, 12, 1795942860, 41185], [900, 50, null, 1795942855, 41185], ['x', 1, 2, 3, 4], [10, 999, 1, 1, 1]],
};
const st = parseOrbcommStatus(status);
ok(!!st && st.good === 341 && st.bad === 12 && st.messages === 5 && st.located && st.birds === 8 && st.pick === 41185 && st.up === 1, 'a real status parses');
ok(!!st && st.sats.length === 1 && st.sats[0].name === 'ORBCOMM FM114' && st.sats[0].how === 'position' && st.sats[0].eph?.altKm === 742.3
   && st.sats[0].down?.hz.length === 2 && st.sats[0].up?.slots === 7 && st.sats[0].elements?.sc === 26, 'a bird with its housekeeping');
ok(!!st && st.carriers.length === 1 && st.carriers[0].lock && st.carriers[0].fixed === 2, 'the carriers');
ok(!!st && st.dots.length === 2 && near(st.dots[0].az, 123.4) && near(st.dots[0].el, 34.5) && st.dots[0].snr === 12 && st.dots[1].snr === null && st.dots[0].norad === 41185,
   'the live dots: tenths of a degree, null SNR kept as grey, junk and impossible elevations dropped');
for (const bad of [null, undefined, 3, 'x', [], [1, 2]]) ok(parseOrbcommStatus(bad) === null, `junk status ${JSON.stringify(bad)} → null`);
{
  const j = parseOrbcommStatus({ good: 'x', bad: -4, sats: [{ name: 'A'.repeat(200) + '\u0007', how: 'evil', snr: NaN, az: 725, el: 400, eph: { lat: 99 } }, 7, null],
                                 counts: { 'a b': 3, ok: -1, sync: 2 }, dots: 'nope', up: 9, carriers: 'x' });
  ok(!!j && j.good === 0 && j.bad === 0 && j.sats.length === 1 && j.sats[0].name.length === 40 && !/\u0007/.test(j.sats[0].name) && j.sats[0].how === ''
     && j.sats[0].snr === 0 && near(j.sats[0].az, 5) && j.sats[0].el === 90 && !j.sats[0].eph && Object.keys(j.counts).join() === 'sync' && j.dots.length === 0
     && j.up === -1 && j.carriers.length === 0, 'junk fields: bounded, cleaned, no NaN, no throw');
}
ok(noThrow(() => parseOrbcommStatus({ sats: new Array(500).fill({ norad: 1 }) })) && parseOrbcommStatus({ sats: new Array(500).fill({ norad: 1 }) })!.sats.length === 12, 'at most 12 birds');

console.log('── orbcomm_pkt ──');
const eph = parseOrbcommPkt({ t: 1795942863.4, norad: 41185, type: 'ephemeris', snr: 11.2, rf: 137287500, sat: 'ORBCOMM FM114',
  text: 'position 51.20N 0.40W, 742 km up, 7.08 km/s; clock 08:41:03 UTC (+0.3 s from ours)', hex: '1f 30 01 82 aa', raw: false,
  v: { lat: 51.2, lon: -0.4, altKm: 742, kmS: 7.08, clock: 1795942863.7 }, fixed: 1 });
ok(!!eph && eph.type === 'ephemeris' && !eph.raw && eph.hex === '1F 30 01 82 AA' && near(eph.clockOff!, 0.3, 1e-6), 'an ephemeris packet: its hex tidied, the clock against ours');
const msg = parseOrbcommPkt({ t: 1795942870, norad: 41185, type: 'message', snr: 9, rf: 137287500, sat: 'ORBCOMM FM114',
  text: 'subscriber message \u2014 not decoded (part 1 of 3)', hex: '1A300182DEADBEEF', raw: true, v: {}, fixed: 0 });
ok(!!msg && msg.raw && msg.hex === '1A 30 01 82 DE AD BE EF', 'a subscriber message: raw, its bytes in hex');
ok(!!msg && /^\d\d:\d\d:\d\d · FM114 · Subscriber message — not decoded \(part 1 of 3\)$/.test(orbPktLine(msg)), `its line: "${msg && orbPktLine(msg)}"`);
{
  // a server that said something else for a subscriber message is still shown as NOT DECODED — never its text
  const m2 = parseOrbcommPkt({ t: 5, type: 'message', text: 'HELLO PLAIN TEXT', hex: '1A00' });
  ok(!!m2 && m2.raw && /Subscriber message — not decoded$/.test(orbPktLine(m2)) && !/HELLO/.test(orbPktLine(m2)), 'a type "message" is ALWAYS shown as not decoded');
}
ok(/are not decoded/.test(ORB_CAVEAT) && /raw bytes only/.test(ORB_CAVEAT) && /private/.test(ORB_CAVEAT), `the caveat: "${ORB_CAVEAT}"`);
for (const bad of [null, 1, {}, { t: 5 }, { text: 'x' }, { t: -1, text: 'x' }]) ok(parseOrbcommPkt(bad) === null, `junk packet ${JSON.stringify(bad)} → null`);
ok(parseOrbcommPkt({ t: 1, text: 'x', type: 'weird', hex: 'zz 1g <script>' })!.type === 'unknown' && parseOrbcommPkt({ t: 1, text: 'x', hex: 'zz 1g <script>' })!.hex === '', 'unknown type, hex keeps hex only');

console.log('── the box ──');
let v = orbViewMerge(null, { status: st });
v = orbViewMerge(v, { pkt: eph });
v = orbViewMerge(v, { pkt: msg });
ok(v.status === st && v.pkts.length === 2 && v.pkts[0] === msg && near(v.clockOff[41185], 0.3), 'status + packets merge: newest first, the clock offset kept');
for (let i = 0; i < 60; i++) v = orbViewMerge(v, { pkt: msg });
ok(v.pkts.length === 40, 'the packet list is bounded (40)');
const lines = orbSatLines(st!.sats[0], v.clockOff[41185]);
ok(lines[0] === 'ORBCOMM FM114 · named by its own position report', `name + how: "${lines[0]}"`);
ok(/SNR 12\.4 dB · Doppler −2\.31 kHz \(predicted −2\.29 kHz\) · az 123° el 35°/.test(lines[1]), `SNR, Doppler measured vs predicted, az/el: "${lines[1]}"`);
ok(lines.some((l) => l === 'Position 51.20°N 0.40°W · 742 km up · 7.08 km/s'), 'the position report as values');
ok(lines.some((l) => /^Satellite clock \d\d:\d\d:\d\d UTC \(\+0\.3 s from ours\)$/.test(l)), 'the satellite clock and its offset');
ok(lines.some((l) => l === 'Downlink channels (1/2): 137.2875, 137.7375 MHz') && lines.some((l) => /^Uplink channels: 148\.0100, 148\.0300 MHz · 3 retries · 7 access slots$/.test(l)),
   'the channel lists in MHz');
ok(lines.some((l) => /^Elements for spacecraft 26: mean anomaly 123\.457° · mean motion 14\.123457 rev\/day$/.test(l)), 'the elements');
ok(orbHowText('doppler') === 'named by its channel and Doppler' && orbHowText('') === 'not yet named' && orbHowText('id') === 'named earlier by its own position report', 'how named, in words');
ok(orbHeadText(st) === 'ORBCOMM · 1 bird decoding · 341 good · 12 bad · 5 subscriber messages (raw)', `the head: "${orbHeadText(st)}"`);
ok(orbHeadText(null) === 'ORBCOMM · listening for carriers', 'the head before a status');
ok(orbCountsText(st!.counts) === 'position 12 · downlink lists 3 · uplink lists 3 · elements 2 · gateway 1 · subscriber (raw) 5 · sync 40 · fill 900', `the counts: "${orbCountsText(st!.counts)}"`);
ok(JSON.stringify(orbAttachParams({ width: 40000 }, 0)) === '{"width":40000}' && JSON.stringify(orbAttachParams({ width: 640000, max_carriers: 2 }, 3)) === '{"width":640000,"max_carriers":3}'
   && JSON.stringify(orbAttachParams(null, 9)) === '{}' && JSON.stringify(orbAttachParams({ width: 1 }, 0)) === '{"width":1}', 'the attach: the pick\'s own params, max_carriers 1–6 by hand, AUTO sends none');

console.log('── the sky plot ──');
{
  const R = 100, cx = 150, cy = 150;
  const [zx, zy] = skyPlotXY(77, 90, cx, cy, R);
  ok(near(zx, cx) && near(zy, cy), 'el 90 = the centre');
  const [nx, ny] = skyPlotXY(0, 0, cx, cy, R);
  ok(near(nx, cx) && near(ny, cy - R), 'az 0, el 0 = the top of the rim (north up)');
  const [ex, ey] = skyPlotXY(90, 0, cx, cy, R);
  ok(near(ex, cx + R) && near(ey, cy), 'az 90 = right (east)');
  const [sx, sy] = skyPlotXY(180, 30, cx, cy, R);
  ok(near(sx, cx, 1e-9) && near(sy, cy + R * 60 / 90), 'az 180, el 30 = below the centre, on the 30° ring');
  const [wx] = skyPlotXY(270, -5, cx, cy, R);
  ok(near(wx, cx - R), 'below the horizon held on the rim (west)');
}
{
  const eq = (a: number[], b: number[]) => a.join() === b.join();
  ok(eq(skySnrRgb(0), [50, 60, 200]) && eq(skySnrRgb(6), [30, 170, 190]) && eq(skySnrRgb(12), [60, 200, 80]) && eq(skySnrRgb(18), [250, 225, 40]), 'the four stops exactly');
  ok(eq(skySnrRgb(-5), [50, 60, 200]) && eq(skySnrRgb(40), [250, 225, 40]), 'clamped 0…18');
  ok(eq(skySnrRgb(3), [40, 115, 195]) && eq(skySnrRgb(15), [155, 212, 60]), 'linear between (the server\'s truncation)');
  ok(eq(skySnrRgb(null), SKY_GREY) && eq(SKY_GREY, [110, 110, 110]) && eq(skySnrRgb(NaN), SKY_GREY), 'null → grey');
}
{
  const j = { v: 1, kind: 'orbcomm', dots: [[1234, 345, 12, 1795942860, 1, 41185, 7, 1], [900, 50, null, 1795942855, 1, 41186, 0, -1],
    [10, 20, 3, 1795940000, 1, 41185, 2, 0], [10, 20, 3, 1, 1, 2, 3, 9], ['bad'], [1], [5, 'x']],
    radioIds: ['a1b2c3d4', 'e5f6a7b8'], radios: [{ id: 'a1b2c3d4', name: 'RTL VHF', n: 100 }, { id: 'e5f6a7b8', name: 'RSP1A', n: 50 }],
    count: 4, truncated: false };
  const d = parseSkyPlotJson(j);
  ok(!!d && d.kind === 'orbcomm' && d.dots.length === 4 && d.count === 4 && !d.truncated, 'skyplot.json parses (junk dots dropped)');
  ok(!!d && d.dots[0].radio === 'e5f6a7b8' && d.dots[1].radio === '' && d.dots[2].radio === 'a1b2c3d4' && d.dots[3].radio === '', 'radioIndex → the id (−1 / out of range = unknown)');
  ok(!!d && d.dots[1].snr === null && d.dots[0].snr === 12 && d.dots[0].count === 7 && d.dots[0].norad === 41185, 'null SNR kept (grey), count and norad');
  ok(!!d && !!d.radios && d.radios.length === 2 && parseSkyPlotJson({ ...j, radios: [{ id: 'a1b2c3d4', name: 'one', n: 3 }] })!.radios === null, 'radio chips only with 2+ radios');
  for (const bad of [null, 1, {}, { dots: 'x' }]) ok(parseSkyPlotJson(bad) === null, `junk skyplot ${JSON.stringify(bad)} → null`);
  ok(noThrow(() => parseSkyPlotJson({ dots: [[1, 2, 3, 4, 5, 6, 7, 8]], radioIds: [5, '<x>'] })), 'bad radioIds do not throw');
  const sc = skyPlotScene(d!.dots, 300);
  ok(sc.dots.length === 4 && sc.dots[0].grey && !sc.dots[1].grey && sc.dots[0].fill === 'rgb(110,110,110)', 'the scene: grey dots drawn first, underneath');
  ok(sc.rings.map((r) => r.el).join() === '0,30,60' && sc.labels.map((l) => l.text).slice(0, 4).join() === 'N,S,E,W', 'rings at 0/30/60°, N/E/S/W');
  const svg = skyPlotSvg(sc, 'Sky <plot>');
  ok(svg.startsWith('<svg') && (svg.match(/<circle/g) ?? []).length === 1 + 3 + 4 && !/<plot>/.test(svg), 'the web SVG: dome + rings + dots, the label escaped');
  const merged = skyPlotMerge(d!.dots, parseSkyDotsLive([[0, 900, 18, 1795999999, 41185]]));
  ok(merged.length === 5 && merged[0].t === 1795999999, 'live dots join on top (newest first)');
  ok(skyPlotMerge(merged, parseSkyDotsLive([[0, 10, 1, 1, 1], [0, 10, 1, 2, 1]]), 3).length === 3, 'bounded');
}
ok(skyPlotQuery('orbcomm', 'a1b2c3d4') === '/vibeserver/skyplot.json?kind=orbcomm&radio=a1b2c3d4' && skyPlotQuery('orbcomm', '../x') === '/vibeserver/skyplot.json?kind=orbcomm'
   && skyPlotQuery('orbcomm', '', 3, 41185, true) === '/vibeserver/skyplot.png?kind=orbcomm&days=3&norad=41185', 'the queries (a bad radio id never sent)');
ok(skyRadioFromBase('http://10.0.0.5:48000/r/a1b2c3d4') === 'a1b2c3d4' && skyRadioFromBase('/r/a1b2c3d4') === 'a1b2c3d4' && skyRadioFromBase('https://x.net/r/a1b2c3d4/') === 'a1b2c3d4'
   && skyRadioFromBase('http://10.0.0.5:48000') === '' && skyRadioFromBase(null) === '', 'this radio from its …/r/<id>');

console.log('── an ORBCOMM pick in SATELLITE ──');
{
  const list = parseSatList({ type: 'sat_list', time: 'ok', located: true, now: 1, sats: [{ id: 'ORBCOMM-FM114', name: 'ORBCOMM FM114', norad: 41185, note: '', age: 1, ageState: 'fresh',
    tx: [{ label: 'Downlink', hz: 137200000, hiHz: 137800000, mode: 'orbcomm', loop: 'none', consumer: 'orbcomm', decoder: 'orbcomm', use: 'packet', reach: true }], passes: [] }] });
  const t = list?.sats[0]?.tx[0];
  ok(!!t && t.decoder === 'orbcomm' && t.mode === 'orbcomm' && txUse(t) === 'packet', 'sat_list keeps an ORBCOMM downlink (decoder orbcomm, use packet) — never dropped');
  ok(!!t && satDecoderFor(t) === 'orbcomm', 'its decoder is ORBCOMM (it fell to "" and was guessed as APRS before)');
  ok(!!t && txListenMode(t) === 'nfm' && txListenMode({ label: '', hz: 1, mode: 'orbcomm', loop: 'none', reach: true }) === 'nfm', 'txListenMode("orbcomm") = nfm (the server\'s listenModeOf)');
  ok(txUse({ label: '', hz: 1, mode: 'orbcomm', loop: 'none', reach: true }) === 'packet', 'an older server with no `use`: mode orbcomm → packet');
  const pk = parseSatPicked({ type: 'sat', state: 'picked', norad: 41185, tx: 0, decoder: 'orbcomm', params: { width: 640000 } });
  ok(!!pk && pk.decoder === 'orbcomm' && pk.params?.width === 640000, 'the pick: decoder orbcomm with its slice');
  ok(satDecView('orbcomm') === 'orbcomm' && satDecWord('orbcomm', 'packet', false, false) === 'listening for ORBCOMM carriers', 'its view and its word');
}
ok((REACH_DECODERS as readonly string[]).includes('orbcomm') && (CLIENT_DECODERS as readonly string[]).includes('orbcomm'), 'greyed with its reason off 137 MHz (REACH_DECODERS); a client decoder');
{
  const web = fs.readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
  ok(/const REACH_DECODERS = \[[^\]]*'orbcomm'/.test(web), 'web: in the reach list');
  const dec = fs.readFileSync(new URL('../web/client/src/decoders.ts', import.meta.url), 'utf8');
  ok(/orbcomm: 'orbcomm'/.test(dec) && /orbcomm_status/.test(dec) && /orbcomm_pkt/.test(dec), 'web: the decoder socket attaches it and hands its messages on');
  const ws = fs.readFileSync(new URL('../web/client/src/satellites.ts', import.meta.url), 'utf8');
  ok(/ORB_CAVEAT/.test(ws) && /skyPlotSvg/.test(ws), 'web: the SATELLITE box shows the caveat and the sky plot');
}

console.log('── the slice on the waterfall (2026-11-24) ──');
{
  const base = { t: 1, good: 0, bad: 0, located: true, birds: 8, sats: [], dots: [] };
  const st2 = parseOrbcommStatus({ ...base,
    slice: { rate: 912000, centre: 137500000, lo: 137180000, hi: 137820000, fedSec: 12.5, blocks: 300, moves: 1, floorDb: -42.5 },
    peaks: [{ rf: 137287500, db: 14.2 }, { rf: 'x', db: 1 }, null, { rf: -5, db: 3 }],
    carriers: [{ id: 0, rf: 137287500, snr: 12, lock: true, ok: 5, bad: 0, fixed: 0, sat: 'ORBCOMM FM114' },
               { id: 1, rf: 137662500, snr: 4, lock: false, ok: 0, bad: 0, fixed: 0 },
               { id: 2, rf: 139000000, snr: 4, lock: true, ok: 0, bad: 0, fixed: 0, sat: 'ORBCOMM FM999' }],
    refused: { spurs: 2, notSdpsk: 1, last: [{ rf: 137400000, why: 'spur', ago: 3, ban: 60 }, { rf: 'x' }] } });
  ok(!!st2 && !!st2.slice && st2.slice.lo === 137180000 && st2.slice.hi === 137820000 && st2.slice.moves === 1 && st2.slice.floorDb === -42.5, 'the slice parses');
  ok(!!st2 && st2.peaks.length === 1 && st2.peaks[0].db === 14.2, 'the peaks: junk dropped');
  ok(!!st2 && st2.carriers[0].sat === 'ORBCOMM FM114' && st2.carriers[1].sat === '', 'carriers[].sat');
  ok(!!st2 && !!st2.refused && st2.refused.spurs === 2 && st2.refused.last.length === 1 && st2.refused.last[0].why === 'spur', 'refused parses (junk dropped)');
  for (const bad of [{ lo: 5, hi: 3 }, { lo: 'x', hi: 9 }, { lo: 1e9, hi: 1.1e9 }, 'slice', null])
    ok(parseOrbcommStatus({ ...base, slice: bad })!.slice === null, `a bad slice ${JSON.stringify(bad)} → none drawn`);
  ok(parseOrbcommStatus(base)!.slice === null && parseOrbcommStatus(base)!.peaks.length === 0 && parseOrbcommStatus(base)!.refused === null, 'an older server: no slice, no peaks');
  ok(orbOverlayFrom(parseOrbcommStatus(base)) === null && orbOverlayFrom(null) === null, 'no slice → no overlay');
  const ov = orbOverlayFrom(st2)!;
  ok(ov.lo === 137180000 && ov.hi === 137820000 && ov.carriers.length === 3 && ov.carriers[0].locked && ov.carriers[0].label === 'FM114'
     && !ov.carriers[1].locked && ov.carriers[1].label === '', 'the overlay: the slice, carriers locked / not, named (the "ORBCOMM " prefix dropped)');
  // a 1 MHz view 137.0–138.0 on 1000 px: 1 kHz a pixel
  const hzToX = (hz: number) => (hz - 137000000) / 1000;
  const L = orbOverlayLayout(ov, hzToX, 1000, 200);
  ok(!!L.band && near(L.band.x0, 180) && near(L.band.x1, 820) && L.band.loIn && L.band.hiIn, 'the band: lo…hi at its x');
  ok(!!L.label && near(L.label.x, 184) && L.label.w === 200, 'the label inside the band, at its left');
  ok(L.marks.length === 2 && near(L.marks[0].x, 287.5) && L.marks[0].locked && L.marks[0].label === 'FM114' && !L.marks[1].locked && near(L.marks[1].x, 662.5),
     'the carriers on screen: solid locked, hollow acquiring, the off-screen one left out');
  // zoomed in on 137.6–137.9: the band runs off the left, its label stays on screen
  const L2 = orbOverlayLayout(ov, (hz) => (hz - 137600000) / 300, 1000, 200);
  ok(!!L2.band && L2.band.x0 === 0 && !L2.band.loIn && L2.band.hiIn && near(L2.band.x1, 733.333, 0.01) && L2.label!.x === 4, 'a band off the left edge is clamped, its label kept on screen');
  const L3 = orbOverlayLayout(ov, (hz) => (hz - 140000000) / 1000, 1000, 200);
  ok(L3.band === null && L3.label === null && L3.marks.length === 0, 'a slice off screen draws nothing');
  ok(orbOverlayLayout(null, hzToX, 1000).band === null && orbOverlayLayout(ov, hzToX, 0).band === null, 'no overlay / no width → nothing');
  ok(orbOverlayKey(ov) !== orbOverlayKey({ ...ov, carriers: [{ ...ov.carriers[0], locked: false }, ov.carriers[1], ov.carriers[2]] }) && orbOverlayKey(null) === '', 'the redraw key changes with a lock');
  ok(ORB_SLICE_LABEL === 'ORBCOMM · every carrier decoded here' && ORB_NO_TUNE === 'The whole ORBCOMM band is decoded at once — no need to tune.', 'the words');
}

console.log('── the band view and the hold (2026-11-24) ──');
{
  const pk = parseSatPicked({ type: 'sat', state: 'picked', norad: 41185, tx: 0, decoder: 'orbcomm', view: 700000, viewCentre: 137500000 });
  ok(!!pk && pk.view === 700000 && pk.viewCentre === 137500000, 'viewCentre parses (the band\'s centre)');
  for (const bad of ['137500000', -5, NaN, 1e12, null])
    ok(parseSatPicked({ type: 'sat', state: 'picked', norad: 1, tx: 0, decoder: 'orbcomm', view: 700000, viewCentre: bad })!.viewCentre === undefined, `a bad viewCentre ${String(bad)} → none (centre on the downlink)`);
  ok(parseSatPicked({ type: 'sat', state: 'picked', norad: 1, tx: 0, decoder: 'lrpt', view: 410000 })!.viewCentre === undefined, 'absent → today\'s behaviour');

  const bird = (norad: number, name: string, age = 0) => ({ norad, name, how: 'position', carrier: 0, rf: 137287500, snr: 10, dop: 0, pred: 0, az: 10, el: 20, dcn: 0,
    nominal: 0, scId: 1, ok: 5, messages: 0, age, eph: { lat: 51.2, lon: -0.4, altKm: 742, kmS: 7.08, clock: 1000 } });
  const S = (t: number, sats: unknown[], pick = 41185) => parseOrbcommStatus({ t, good: 1, bad: 0, located: true, birds: 8, sats, pick, dots: [] });
  let h = orbViewMerge(null, { status: S(1000, [bird(41185, 'ORBCOMM FM114'), bird(41186, 'ORBCOMM FM115')]) });
  ok(orbShownSats(h).length === 2 && orbShownSats(h).every((x) => x.heldSecs === undefined), 'both listed: neither held');
  h = orbViewMerge(h, { status: S(1040, [bird(41185, 'ORBCOMM FM114')]) });
  let shown = orbShownSats(h);
  ok(shown.length === 2 && shown[1].sat.name === 'ORBCOMM FM115' && shown[1].heldSecs === 40, 'FM115\'s carrier dropped: its block stays, held 40 s');
  const lines = orbSatLines(shown[1].sat, undefined, shown[1].heldSecs);
  ok(/· last heard 40 s ago$/.test(lines[0]) && lines.some((l) => /^Position 51\.20°N/.test(l)), `held: its last block, "last heard" (${lines[0]})`);
  ok(orbHeldText(30) === 'last heard 30 s ago' && orbHeldText(300) === 'last heard 5 min ago', 'the age in words');
  h = orbViewMerge(h, { status: S(1000 + ORB_HOLD_SEC, [bird(41185, 'ORBCOMM FM114')]) });
  ok(orbShownSats(h).length === 2 && orbShownSats(h)[1].heldSecs === ORB_HOLD_SEC, 'still held at 10 min');
  h = orbViewMerge(h, { status: S(1001 + ORB_HOLD_SEC, [bird(41185, 'ORBCOMM FM114')]) });
  ok(orbShownSats(h).length === 1 && !h.held[41186], 'gone after 10 min');
  h = orbViewMerge(h, { status: S(2000, []) });
  ok(orbShownSats(h).length === 1 && orbShownSats(h)[0].heldSecs === 399,
     'the pick\'s own bird held too while its carrier is down');
  h = orbViewMerge(h, { status: S(2001, [], 41190) });
  ok(orbShownSats(h).length === 0, 'a status for another pick: the hold goes (the pick ended)');
  ok(orbShownSats(orbViewEmpty()).length === 0 && orbShownSats(null).length === 0, 'a fresh box (a new pick) holds nothing');
}

console.log(`\ntest_orbcomm_app: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
