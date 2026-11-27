// test_map_chrome.ts — ★★★ beta97 THE APP'S MAP CONTROL CARDS (src/utils/mapChrome.ts; docs/v12/MAP-CONTROL-CARDS.md): the
// layout from the WINDOW and the real insets (never a device), the short-landscape safe rectangle and its one top row, the
// LIST drawer's width and whole rows, a card's box, the LIVE badge rule, the RATE chip's pick, the gain row per driver,
// the app's spectrum rule under a map, the mini waterfall's crop and budget, the pill's short words.
//   npx tsx scripts/test_map_chrome.ts
import { chromeGeom, cardBox, listRowsFor, listWidthFor, liveBadges, rateRows, rateChip, gainRowFor, mapSpecWanted, mwfCrop, mwfDue, mwfDims, mwfX,
         pillWords, padOrder, CHIP_ORDER, TAB_H, type ChipId, type ChromeGeom } from '../src/utils/mapChrome';
import { wfModelChannels } from '../src/utils/mapWfModel';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const ALL: ChipId[] = ['servers', 'tune', 'layers', 'stats', 'map', 'chat', 'health', 'time', 'rate'];
const inside = (g: ChromeGeom) => Object.values(g.boxes).every((b) => b!.x >= g.safe.l - 0.5 && b!.x + b!.w <= g.safe.r + 0.5 && b!.y >= 0 && b!.y + b!.h <= g.safe.b + 0.5);
const overlaps = (g: ChromeGeom) => { const bs = Object.values(g.boxes) as Array<{ x: number; y: number; w: number; h: number }>;
  for (let i = 0; i < bs.length; i++) for (let j = i + 1; j < bs.length; j++) { const a = bs[i], b = bs[j];
    if (a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h) return true; } return false; };

// ── tall windows: the window decides, not the device ──
{
  const pro = chromeGeom({ w: 440, h: 956, insets: { top: 62, bottom: 34, left: 0, right: 0 }, chips: ALL });
  ok(!pro.short && pro.oneSide && pro.cardFrom.map === 'L', '17 Pro Max portrait (440×956): everything down the left, MAP included');
  const seFull = chromeGeom({ w: 375, h: 667, insets: { top: 20, bottom: 0, left: 0, right: 0 }, chips: ALL });
  ok(seFull.oneSide, 'SE portrait, the app full screen (375×667 — no browser bars): the stack fits, one side');
  const se = chromeGeom({ w: 375, h: 560, insets: { top: 20, bottom: 0, left: 0, right: 0 }, chips: ALL });
  ok(!se.short && !se.oneSide && se.cardFrom.map === 'R', 'a 375×560 window (split view / slide over): flank — MAP on the right');
  ok(se.boxes.exit!.x === 0 && se.boxes.exit!.y === 0 && se.boxes.exit!.h === 52 + 20, 'the ✕ is the top-left corner piece, padding the top inset');
  ok(se.boxes.rate!.x + se.boxes.rate!.w === 375 && se.boxes.rate!.y === 0, 'RATE is the top-right corner piece');
  ok(se.boxes.servers!.y > se.boxes.exit!.h && se.boxes.tune!.y > se.boxes.servers!.y && se.boxes.layers!.y > se.boxes.tune!.y, 'SERVERS, TUNE, LAYERS down the left under the ✕, in order');
  ok(se.boxes.chat!.y >= se.boxes.rate!.h && se.boxes.health!.y > se.boxes.chat!.y && se.boxes.time!.y > se.boxes.health!.y, 'CHAT under RATE, then HEALTH, then TIME under it');
  ok(!overlaps(se) && !overlaps(pro), 'no two chips overlap (tall)');
  ok(se.pill.at === 'top' && se.pill.x >= se.boxes.exit!.w && se.pill.x + se.pill.w <= se.boxes.rate!.x, 'the pill top-centre, clear of the ✕ and RATE');
  const tall = chromeGeom({ w: 440, h: 956, insets: { top: 62, bottom: 34, left: 0, right: 0 }, chips: ALL, healthH: 90 });
  ok(tall.boxes.time!.y >= tall.boxes.health!.y + 90, 'TIME moves down by HEALTH’s measured height (the EdgeChip pair)');
  const noRate = chromeGeom({ w: 390, h: 844, insets: { top: 59, bottom: 34, left: 0, right: 0 }, chips: ['servers', 'tune', 'layers', 'chat'] });
  ok(!noRate.boxes.rate && !noRate.boxes.map && noRate.boxes.chat!.y >= 59, 'a chip not offered is not placed (no RATE, no MAP); CHAT stays under the inset');
  const ipad = chromeGeom({ w: 820, h: 1180, insets: { top: 24, bottom: 20, left: 0, right: 0 }, chips: ALL });
  ok(ipad.oneSide && !ipad.short, 'iPad portrait: one side');
}
// ── the LIST drawer: width by the window, whole rows 3–7 ──
ok(listWidthFor(390) === 390 && listWidthFor(1440) === 432 && listWidthFor(2560) === 640 && listWidthFor(820) === 420, `widths: 390 → full, 820 → 420, 1440 → 432, 2560 → 640`);
ok(listRowsFor(844) === 4 && listRowsFor(500) === 3 && listRowsFor(2000) === 7, `rows: 22 % in whole rows, 3–7 (${listRowsFor(844)}, ${listRowsFor(500)}, ${listRowsFor(2000)})`);
{
  const g = chromeGeom({ w: 390, h: 844, insets: { top: 59, bottom: 34, left: 0, right: 0 }, chips: ALL });
  ok(g.list.x === 0 && g.list.bottom === 0 && g.list.w === 390 && !g.list.side, 'portrait: the drawer flush left / bottom, the phone’s full width');
  const lastLeft = Math.max(...(['servers', 'tune', 'layers', 'stats'] as ChipId[]).map((id) => g.boxes[id]!.y + g.boxes[id]!.h));
  ok(844 - g.list.maxH >= lastLeft, 'maximised: up to just under the left chips');
}
// ── SHORT LANDSCAPE: the safe rectangle, one top row ──
for (const prof of [{ name: 'Stuart’s 17 Pro Max', w: 956, h: 376, ins: { top: 0, bottom: 20, left: 62, right: 62 } },
                    { name: 'SE', w: 667, h: 320, ins: { top: 0, bottom: 0, left: 0, right: 0 } },
                    { name: 'Android cutout left', w: 915, h: 412, ins: { top: 0, bottom: 0, left: 32, right: 0 } }]) {
  const g = chromeGeom({ w: prof.w, h: prof.h, insets: prof.ins, chips: ALL, layersW: 120, layersCompactW: 60, rateW: 96 });
  ok(g.short, `${prof.name} ${prof.w}×${prof.h}: short landscape`);
  ok(inside(g), `${prof.name}: every chip inside the safe rectangle (L${prof.ins.left} R${prof.ins.right} B${prof.ins.bottom})`);
  ok(!overlaps(g), `${prof.name}: no two tabs overlap`);
  const ids = (Object.keys(g.boxes) as ChipId[]).sort((a, b) => g.boxes[a]!.x - g.boxes[b]!.x);
  ok(ids[0] === 'exit' && ids[ids.length - 1] === 'rate', `${prof.name}: ✕ first, RATE last (${ids.join(' ')})`);
  ok(ids.join() === CHIP_ORDER.filter((c) => c === 'exit' || ALL.includes(c)).join(), `${prof.name}: the row in Stuart’s order`);
  ok(Object.values(g.boxes).every((b) => b!.y === 0 && b!.h === TAB_H), `${prof.name}: one row hanging from the top edge, one height`);
  ok(Object.values(g.cardFrom).every((f) => f === 'D'), `${prof.name}: every card drops down`);
  ok(g.list.side && g.list.x === prof.ins.left && g.list.top >= TAB_H && g.list.w >= 300 && g.list.w <= 360, `${prof.name}: LIST a side panel from the safe left, under the row (${g.list.w} pt)`);
  ok(g.pill.at === 'bottom' && g.pill.x >= g.list.x + g.list.w && g.pill.x + g.pill.w <= g.safe.r && g.pill.y + 30 <= g.safe.b, `${prof.name}: the pill at the bottom centre, inside the safe rectangle`);
  ok(g.br.horizontal && g.br.right >= prof.ins.right && g.br.bottom >= prof.ins.bottom, `${prof.name}: the bottom-right group horizontal, inside the insets`);
  const c = cardBox(g, 'tune', prof.w, prof.h);
  ok(c.y >= TAB_H && c.x >= g.safe.l && c.x + c.w <= g.safe.r && c.y + c.maxH <= g.safe.b, `${prof.name}: TUNE’s card drops from the row, inside the safe rectangle`);
}
{
  const roomy = chromeGeom({ w: 956, h: 376, insets: { top: 0, bottom: 20, left: 62, right: 62 }, chips: ALL, layersW: 120, layersCompactW: 60, rateW: 96 });
  const tight = chromeGeom({ w: 667, h: 320, insets: { top: 0, bottom: 0, left: 0, right: 0 }, chips: ALL, layersW: 200, layersCompactW: 60, rateW: 96 });
  ok(!roomy.famCompact && tight.famCompact, `a row too long for the window drops LAYERS' counts together (956: ${roomy.famCompact}, 667: ${tight.famCompact})`);
  ok(!chromeGeom({ w: 667, h: 500, insets: { top: 0, bottom: 0, left: 0, right: 0 }, chips: ALL }).short, 'a landscape window 500 tall is not short');
  ok(!chromeGeom({ w: 300, h: 400, insets: { top: 0, bottom: 0, left: 0, right: 0 }, chips: ALL }).short, 'a short window taller than wide is not short landscape');
}
// ── a card's box: beside its chip, inside the window ──
{
  const g = chromeGeom({ w: 390, h: 844, insets: { top: 59, bottom: 34, left: 0, right: 0 }, chips: ALL });
  const t = cardBox(g, 'tune', 390, 844), r = cardBox(g, 'chat', 390, 844);
  ok(t.x >= g.boxes.tune!.x + g.boxes.tune!.w && t.x + t.w <= 390 && t.y === g.boxes.tune!.y, 'TUNE’s card opens to the right of its chip, within the window');
  ok(r.x + r.w <= g.boxes.chat!.x && r.x >= 0, 'CHAT’s card opens to the left of its chip');
}
// ── the LIVE badges ──
{
  const b = liveBadges([{ live: true }, { others: ['RSP'] }, {}, { others: ['Pi 500 V4L', 'feed'] }]);
  ok(b[0]?.kind === 'mine' && b[0].text === 'LIVE', 'filled LIVE = this radio');
  ok(b[1]?.kind === 'other' && b[1].text === 'RSP', 'outlined, named when one short name = another radio');
  ok(b[2] === null, 'none = stored');
  ok(b[3]?.kind === 'other' && b[3].text === 'LIVE' && /Pi 500 V4L, feed/.test(b[3].words), 'outlined LIVE with the names in its words when several / long');
  ok(liveBadges([{ live: true }, { live: true }]).every((x) => x === null) && liveBadges([{}, {}]).every((x) => x === null), 'all rows alike → no badges at all');
}
// ── the RATE chip ──
{
  const rr = rateRows({ mine: { name: 'VDL2', perMin: 18 }, feeds: [{ title: 'PiAware', state: 'connected', perSec: 42.5 }, { title: 'Old feed', state: 'stalled', perSec: 0 }, { title: 'No rate', state: 'connected' }] });
  ok(rr.length === 3 && rr[0].mine && rr[0].rate === '18/min', `this radio's mode first, marked yours (${rr.map((r) => `${r.name} ${r.rate}`).join(' · ')})`);
  ok(!rr.some((r) => r.name === 'No rate'), 'a feed without its own rate is left out (never a count)');
  const c = rateChip(rr);
  ok(c.show && c.text === 'VDL2 18/min' && c.more === 2, `the chip: "${c.text}" +${c.more}`);
  const viewer = rateRows({ feeds: [{ title: 'A', state: 'connected', perSec: 3 }, { title: 'B', state: 'connected', perSec: 120 }] });
  ok(viewer[0].name === 'B', 'a viewer (nothing of its own): the busiest first');
  ok(!rateChip(rateRows({ feeds: [{ title: 'A', state: 'stalled', perSec: 0 }] })).show && !rateChip([]).show, 'nothing live → hidden');
  ok(rateChip(rateRows({ mine: { name: 'ADS-B', perSec: 210 } })).more === 0, 'one source = a static reading (no card)');
}
// ── the gain row, per driver ──
{
  const rtl = gainRowFor({ driver: 'rtl', gains: [0, 90, 197, 296, 402, 496], gainTenthDb: 296, autoGain: true, liveTenthDb: 402, more: true });
  ok(rtl.kind === 'auto' && rtl.name === 'AUTO' && rtl.value === '40.2 dB' && rtl.more, `RTL, AUTO on: [AUTO] value ⌄ — the live gain (${rtl.kind === 'auto' ? rtl.value : ''})`);
  const rtlM = gainRowFor({ driver: 'rtl', gains: [0, 90, 197, 296, 402, 496], gainTenthDb: 296, autoGain: false });
  ok(rtlM.kind === 'manual' && rtlM.at === 3 && rtlM.max === 5 && rtlM.value === '29.6 dB', 'RTL manual: a slider over the gain list');
  const rsp = gainRowFor({ driver: 'sdrplay', rspIfGr: 40, rspIfAgc: true, rspSys: 0 });
  ok(rsp.kind === 'auto' && rsp.name === 'IF AGC' && rsp.value === 'GR 40 dB', 'RSP: IF gain reduction + its IF AGC');
  const ahf = gainRowFor({ driver: 'airspyhf', ahfAtt: 2, ahfAgc: false, attStepDb: 6, attSteps: 9 });
  ok(ahf.kind === 'manual' && ahf.name === 'AGC' && ahf.value === 'ATT 12 dB' && ahf.max === 8, 'HF+: the attenuator + its AGC (no variable gain)');
  ok(gainRowFor({ driver: 'hackrf', more: true }).kind === 'none' && gainRowFor({ driver: 'usrp' }).kind === 'none', 'anything else: no row (⌄ only)');
  ok(gainRowFor({ driver: 'rtl', gains: [] }).kind === 'none', 'an RTL with no gain list: no row');
  const lk = gainRowFor({ driver: 'rtl', gains: [0, 100], gainTenthDb: 100, autoGain: false, locked: true });
  ok(lk.kind === 'locked', 'owner-locked: 🔒 (no slider)');
  const un = gainRowFor({ driver: 'rtl', gains: [0, 100], gainTenthDb: 100, autoGain: false, locked: true, admin: true });
  ok(un.kind === 'manual' && un.unlocked, 'unlocked with the admin password: the slider, marked unlocked');
}
// ── the spectrum under a map (the app's rule) ──
ok(mapSpecWanted({ mapUp: false, live: true, wfOpen: false }), 'no map: spectrum on');
ok(mapSpecWanted({ mapUp: true, live: false, wfOpen: false }), 'the NON-live viewer over the radio: on (the waterfall keeps its history)');
ok(!mapSpecWanted({ mapUp: true, live: true, wfOpen: false }), 'a LIVE map mode: off');
ok(mapSpecWanted({ mapUp: true, live: true, wfOpen: true }), 'a LIVE map with the mini waterfall open: on');
// ── the mini waterfall ──
{
  const bins = new Float32Array(1000).map((_, i) => (i === 500 ? -20 : -100));
  const row = mwfCrop(bins, 100e6, 1e6, 99.9e6, 100.1e6, 100);
  ok(row.length === 100 && Math.max(...Array.from(row)) === -20, 'the crop keeps a one-bin carrier (max under each column)');
  const out = mwfCrop(bins, 100e6, 1e6, 101e6, 102e6, 10);
  ok(Array.from(out).every((v) => Number.isNaN(v)), 'a span outside the frame reads as nothing (NaN), never stale bins');
  ok(mwfDue(0, 100) && !mwfDue(0, 50), '≤ 10 rows a second');
  const d = mwfDims(390, 390, 844);
  ok(d.cols === 390 && d.rows === 844 && mwfDims(3000, 3000, 2000).cols === 1024 && mwfDims(10, 10, 4000).rows === 1600, 'about one column a point (≤ 1024); history = the screen’s longer side (≤ 1600)');
  const vdl = wfModelChannels({ id: 'vdl2', name: 'VDL2', chans: [136.7e6, 136.975e6] })!;
  const xs = vdl.marks.map((m) => mwfX(m.hz, vdl.lo, vdl.hi));
  ok(xs.every((x) => x != null && x > 0 && x < 1) && vdl.marks[0].label === 'Ch1 136.700', 'a labelled marker per decoder channel, inside the span');
  ok(mwfX(1, 2, 3) === null, 'a marker outside the span is not drawn');
}
// ── the pill's words ──
{
  const t = 'Shared VFO: Ask before changing mode or exiting 2/5';
  ok(pillWords(t, 80) === t, 'room: the whole line');
  ok(pillWords(t, 14) === 'Ask · 2/5', `narrow: the short form "${pillWords(t, 14)}"`);
  ok(pillWords(t, 4) === '2/5', 'very narrow: the count, never cut');
  ok(pillWords('Shared VFO: Free to change mode or exit (You+1)', 12).endsWith('(You+1)'), 'the (You+N) count kept whole');
}
// ── the pad's order ──
{
  const g = chromeGeom({ w: 956, h: 376, insets: { top: 0, bottom: 20, left: 62, right: 62 }, chips: ALL });
  const o = padOrder(g, { cardOpen: true, listRows: 2, br: true });
  ok(o[0] === 'chip:exit' && o.indexOf('chip:rate') < o.indexOf('card') && o.indexOf('card') < o.indexOf('row:0') && o[o.length - 1] === 'help',
     `short: the top row → the card → LIST rows → bottom-right (${o.join(' ')})`);
  const t = chromeGeom({ w: 375, h: 560, insets: { top: 20, bottom: 0, left: 0, right: 0 }, chips: ALL });
  const ot = padOrder(t, { cardOpen: false, listRows: 0, br: false });
  ok(ot[0] === 'chip:exit' && ot.indexOf('chip:stats') < ot.indexOf('chip:rate'), `tall: the left family, then the right column (${ot.join(' ')})`);
}

console.log(`\n${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
