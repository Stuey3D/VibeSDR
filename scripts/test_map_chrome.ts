// test_map_chrome.ts — ★★★ beta97 THE APP'S MAP CONTROL CARDS (src/utils/mapChrome.ts; docs/v12/MAP-CONTROL-CARDS.md): the
// layout from the WINDOW and the real insets (never a device), the short-landscape safe rectangle and its one top row, the
// LIST drawer's width and whole rows, a card's box, the LIVE badge rule, the RATE chip's pick, the gain row per driver,
// the app's spectrum rule under a map, the mini waterfall's crop and budget, the pill's short words.
//   npx tsx scripts/test_map_chrome.ts
import { readFileSync } from 'node:fs';
import { chromeGeom, cardBox, listRowsFor, listWidthFor, liveBadges, rateRows, rateChip, gainRowFor, mapSpecWanted, mwfCrop, mwfDue, mwfDims, mwfX,
         pillWords, padOrder, listFlowStep, chromeScale, chromeGeomAt, cardBoxAt, CHIP_ORDER, TAB_H, serversRows, sourceRows, layerOthers, fitGroupOf, fitGroups, fitMainOf, BR_ROW_W, GAP, cardRise, itemCardArea, listStops, FTAB_H, LIST_CHIP_H, layerOtherCounts, layerSplit, layerFamilies as layerFamiliesX, pillLeft, feedNotice, type ChipId, type ChromeGeom } from '../src/utils/mapChrome';
import { scopeRadioOk, scopeList } from '../src/services/mapScope';
import { wfModelChannels, wfModelNdb } from '../src/utils/mapWfModel';
import { revealScrollTarget } from '../src/utils/navReveal';
import { ndbSanitise } from '../src/services/ndb';
import { fitDefault } from '../src/utils/mapCards';

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
  const hrf = gainRowFor({ driver: 'hackrf', hrfLna: 17 });
  ok(hrf.kind === 'manual' && hrf.name === '' && hrf.label === 'LNA' && hrf.step === 8 && hrf.at === 16 && hrf.more, 'HackRF: the LNA slider in 8 dB steps, NO AGC key (it has none); VGA / amp under ⌄');
  const usrp = gainRowFor({ driver: 'usrp', gains: [0, 380, 760], gainTenthDb: 380, autoGain: true, hwAgc: true });
  ok(usrp.kind === 'auto' && usrp.name === 'AGC', 'USRP with its own AGC: [AGC] value');
  const usrpNo = gainRowFor({ driver: 'usrp', gains: [0, 380, 760], gainTenthDb: 380, autoGain: true, hwAgc: false });
  ok(usrpNo.kind === 'manual' && usrpNo.name === '', 'USRP without an AGC: the slider alone, never a dead AGC key');
  ok(gainRowFor({ driver: 'usrp' }).kind === 'none', 'USRP with no gain list: no row');
  const asp = gainRowFor({ driver: 'airspy', aspGainMode: 1, gainTenthDb: 150, aspLimit: { max: 12 }, more: true });
  ok(asp.kind === 'manual' && asp.label === 'LINEAR' && asp.max === 12 && asp.at === 12 && asp.name === '', 'Airspy LINEAR: the preset slider, clamped to the owner’s limit, no AGC key');
  const sens = gainRowFor({ driver: 'airspy', aspGainMode: 0, gainTenthDb: 50 });
  ok(sens.kind === 'manual' && sens.label === 'SENSITIVE' && sens.at === 5, 'Airspy SENSITIVE: its own curve');
  const free = gainRowFor({ driver: 'airspy', aspGainMode: 2 });
  ok(free.kind === 'none' && free.more, 'Airspy FREE: no single gain → no row, its stages under ⌄');
  ok(gainRowFor({ driver: 'airspy', aspGainMode: 1, aspLimit: { max: 5, locked: true } }).kind === 'locked', 'Airspy locked on this band by the owner: 🔒');
  ok(gainRowFor({ driver: 'rtl', gains: [0, 100], noHwGain: true, more: false }).kind === 'none', 'a relayed rtl_tcp stream (no tuner): no row');
  ok(gainRowFor({ driver: 'icom', more: true }).kind === 'none' && gainRowFor({ driver: 'icom', more: true }).more, 'an Icom: no row, ⌄ for its own controls');
  ok(gainRowFor({ driver: 'mystery' }).kind === 'none' && !gainRowFor({ driver: 'mystery' }).more, 'an unknown driver with no controls: nothing at all (not dead)');
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
// ── ★★★ beta97 the pill ATTACHED (the web's) and the short layout's bottom row ──
{
  const t = chromeGeom({ w: 402, h: 874, insets: { top: 62, bottom: 34, left: 0, right: 0 }, chips: ['servers', 'tune', 'layers', 'stats', 'map', 'chat', 'rate'] as ChipId[] });
  ok(t.pill.at === 'top' && t.pill.y === 0 && t.pill.pad === 62 && t.pill.h === 30 + 62, 'tall: the pill flush to the TOP edge, padded below the notch');
  const s2 = chromeGeom({ w: 956, h: 376, insets: { top: 0, bottom: 20, left: 62, right: 62 }, chips: ['servers', 'tune', 'layers', 'stats', 'map', 'chat', 'rate'] as ChipId[] });
  ok(s2.pill.at === 'bottom' && s2.pill.y + s2.pill.h === 376 && s2.pill.pad === 20, 'short: the pill flush to the BOTTOM edge, padded above the home bar');
  ok(s2.pill.x + s2.pill.w <= s2.safe.r - BR_ROW_W, 'short: the pill\'s band ends before the bottom-right row (scale · zoom · ?) — never over the scale bar');
  ok(s2.list.side && s2.list.top >= TAB_H && s2.list.top + s2.list.maxH >= s2.safe.b - 2 * GAP, 'short: LIST is the left side panel, the full height under the top row');
  const mac = chromeGeom({ w: 993, h: 621, insets: { top: 28, bottom: 0, left: 0, right: 0 }, chips: ['servers', 'tune', 'layers', 'stats', 'map', 'chat', 'rate'] as ChipId[] });
  const line = 'Shared VFO: Ask before changing mode or exiting 2/5';
  ok(Math.floor((mac.pill.w - 24) / 7.2) >= line.length, 'a Mac window: the whole shared-tuner line fits its band');
}

// ── ★★ a long card RISES beside its column (cardRise) rather than opening a short window at the foot ──
{
  ok(cardRise({ y: 400, maxH: 300, yMin: 80 }, 200).top === 400, 'fits below its chip: stays beside it');
  const r1 = cardRise({ y: 400, maxH: 300, yMin: 80 }, 500);
  ok(r1.top === 200 && r1.maxH === 500, `taller: rises until it fits (top ${r1.top}), its foot where it was`);
  const r2 = cardRise({ y: 400, maxH: 300, yMin: 80 }, 2000);
  ok(r2.top === 80 && r2.maxH === 620, 'very tall: rises no higher than under the corner, then scrolls');
  const g = chromeGeom({ w: 993, h: 621, insets: { top: 28, bottom: 0, left: 0, right: 0 }, chips: ['servers', 'tune', 'layers', 'stats', 'map', 'chat', 'rate'] as ChipId[] });
  const cb = cardBox(g, 'map', 993, 621);
  ok(cb.yMin <= cb.y && cb.yMin >= g.boxes.exit!.y + g.boxes.exit!.h, 'a side card may rise to just under the ✕ corner, never over it');
}

// ── ★★★ beta97 the LIST's own controls are their OWN stops (Stuart, build 587) ──
{
  const tall = chromeGeom({ w: 402, h: 874, insets: { top: 62, bottom: 34, left: 0, right: 0 }, chips: ['servers', 'tune', 'layers', 'stats', 'map', 'chat', 'rate'] as ChipId[] });
  const ls = listStops({ side: false, swap: true, tabs: ['aircraft', 'messages'], pause: true, sort: true });
  const o = padOrder(tall, { br: true, pill: true, listTop: ls.top, listBar: ls.bar });
  const i = (x: string) => o.indexOf(x);
  ok(i('pill') < i('swap') && i('swap') < i('max') && i('max') < i('list'), `tall: … pill → ≋ swap → ⌃ maximise → LIST (${o.slice(i('pill')).join(' ')})`);
  ok(i('list') < i('hide') && i('hide') < i('tab:aircraft') && i('tab:aircraft') < i('tab:messages') && i('tab:messages') < i('pause') && i('pause') < i('sort') && i('sort') < i('help'),
     'tall: LIST → ☰ hide → the mode tabs → ‖ +N → sort → the bottom-right group');
  ok(!o.some((x) => x.startsWith('row:')), 'the rows are never in the walk outside the list (Enter in, as now)');
  const inside = padOrder(tall, { br: true, listInside: true, listRows: 3, listTop: ls.top, listBar: ls.bar });
  ok(inside.join() === 'swap,max,row:0,row:1,row:2,hide,tab:aircraft,tab:messages,pause,sort', `inside: Up from the first row reaches ⌃, Down from the last the bar (${inside.join(' ')})`);
  const sd = listStops({ side: true, swap: true, tabs: ['aircraft', 'messages'], pause: false, sort: true, filter: true });
  ok(sd.top.join() === 'swap,tab:aircraft,tab:messages,all' && sd.bar.join() === 'hide,sort', `short landscape's side panel: no maximise; its tabs head it (${sd.top.join(' ')} | ${sd.bar.join(' ')})`);
  ok(listStops({ side: false, swap: true, tabs: ['a', 'b'], sort: true, wf: true }).bar.join() === 'hide', 'the waterfall pane: the bar is ☰ only');
  const mc = readFileSync(new URL('../src/components/MapChrome.tsx', import.meta.url), 'utf8');
  ok(/<NavCtx\.Provider value=\{inside \? scope\.navCtx : outerNav\}>/.test(mc) && /\{listStop\}\n\s*<NavCtx\.Provider value=\{inside \? scope\.navCtx : null\}>\n\s*<ScrollView/.test(mc),
     'ListDrawer: its controls in the chrome walk (outerNav) when outside, the rows only inside; the LIST stop just above the rows');
}

// ── ★★★ beta97 build 587: both counts where this radio AND others hold a layer; the chip's TOTAL; the pill centred; the notice ──
{
  const ac = [{ age: 5, src: ['ADS-B'], radios: ['feed'] }, { age: 5, src: ['ADS-B'], radios: ['feed', 'r1'] }, { age: 5, src: ['ADS-B', 'VDL2'], radios: ['r1'] }, { age: 90, src: ['ADS-B'], radios: ['feed'] }];
  const oc = layerOtherCounts(ac, { mine: 'r1', onRadio: true });
  ok(oc.adsb === 2 && !oc.vdl2, `other sources' counts: this radio's own and stale (>60 s) aircraft left out (${JSON.stringify(oc)})`);
  const row = { id: 'adsb', n: 0, nOther: 33, live: true, others: ['PiAware'] };
  const sp = layerSplit(row);
  ok(sp.split && sp.mine === 0 && sp.other === 33 && sp.total === 33, 'ADS-B LIVE 0 (this radio) + LIVE 33 (PiAware): both badges, both counts; total 33');
  ok(!layerSplit({ n: 12, live: true }).split && layerSplit({ n: 12, live: true }).total === 12, 'one source: one badge / count as before');
  const fam = layerFamiliesX([row, { id: 'navst', n: 46 }], {});
  ok(fam.length === 2 && fam[0].glyph === 'aircraft' && fam[0].n === 33 && fam[1].n === 46, `the LAYERS chip: ✈ 33 over NAVTEX 46 — a family never vanishes because this radio has 0 (${JSON.stringify(fam)})`);
  ok(pillLeft({ x: 80, w: 1000 }, 300, 1200) === 450, 'the pill centred on the WINDOW (not its band)');
  ok(pillLeft({ x: 80, w: 300 }, 280, 1200) === 100, 'slid only as far as it must to stay inside its band');
  ok(feedNotice('UberSDR HFDL lost — reconnecting · PiAware still live')?.title === 'AIRCRAFT FEED' && feedNotice('')=== null && feedNotice('Map time: 4 min on the map')?.title === 'TIME LIMIT', 'the notice\'s heading from its words; nothing to say → no strip');
  const mo = readFileSync(new URL('../src/components/MapOverlay.tsx', import.meta.url), 'utf8');
  const mc = readFileSync(new URL('../src/components/MapChrome.tsx', import.meta.url), 'utf8');
  ok(!/feedwarn/.test(mo) && !/__adsbFeedWarn/.test(mo), 'the page\'s own top banner (#feedwarn) is deleted');
  ok(/notice=\{feedNotice\(p\.feedWarn\)\}/.test(mo) && /feedWarn=\{kind === 'airsea' \? adsbFeedWarn \?\? '' : ''\}/.test(mo) && /\{!!p\.notice && noticeX !== p\.notice\.text && /.test(mc),
     'the warning is the cards\' notice strip (radio and viewer paths), × hides it until its words change');
  ok(/top: \(p\.shared \? g\.pill\.y \+ g\.pill\.h : p\.insets\.top\) \+ GAP \* k/.test(mc), 'tall: directly below the pill, never under it');
}

// ── ★★★ beta97 the LIST's dB chip reads (Stuart, build 587: "the dB reading in the list is tiny") ──
{
  const mc = readFileSync(new URL('../src/components/MapChrome.tsx', import.meta.url), 'utf8');
  const al = readFileSync(new URL('../src/components/AdsbLiveList.tsx', import.meta.url), 'utf8');
  const pt = Number(/export const LIST_SNR_PT = ([0-9.]+);/.exec(mc)?.[1]);
  const sub = Number(/lsecond: \{ fontFamily: F, fontSize: ([0-9.]+),/.exec(mc)?.[1]);
  const facts = Number(/kv: \{ fontFamily: F, fontSize: ([0-9.]+),/.exec(mc)?.[1]);
  ok(pt >= sub && pt >= facts, `LIST rows (aircraft, messages, ships, spots — every TrackRow): the chip ${pt} pt ≥ the subtitle ${sub} and the facts ${facts}`);
  ok(/<SnrChip mode=\{r\.snrMode as SnrMode\} db=\{r\.snr\} size=\{LIST_SNR_PT \* k\} \/>/.test(mc), '…at the chrome\'s scale (k)');
  const info = Number(/info: +\{ fontFamily: F, fontSize: ([0-9.]+),/.exec(al)?.[1]);
  ok(/<SnrChip mode="adsb" db=\{r\.snr\} size=\{11\.5 \* snrK\} \/>/.test(al) && 11.5 >= info && /const snrK = chromeScale\(win\.width, win\.height\);/.test(al), `the ADS-B LIVE rows too (≥ their info line ${info} pt, at the same scale)`);
}

// ── ★★★ beta97 the TUNE gain row (Stuart, build 587: "gain controls bring up the main menu instead") ──
{
  const rsp = gainRowFor({ driver: 'sdrplay', rspIfGr: 42, rspIfAgc: true, rspLna: 3, agcName: 'VibeAGC', more: true });
  ok(rsp.kind === 'auto' && rsp.name === 'VibeAGC' && rsp.value === 'RF LNA 3 \u00b7 IF 42 dB', `RSP auto: the server's loop name and RF + IF on one row (${rsp.kind !== 'none' ? rsp.name + ' ' + rsp.value : ''})`);
  const sdrOnly = gainRowFor({ driver: 'sdrplay', rspIfGr: 42, rspIfAgc: true, more: true });
  ok(sdrOnly.kind === 'auto' && sdrOnly.name === 'IF AGC', 'no loop reported (an older server): the generic name, never a made-up one');
  const lockedRsp = gainRowFor({ driver: 'sdrplay', rspIfGr: 42, rspIfAgc: true, rspLna: 3, agcName: 'VibeAGC', locked: true, more: true });
  ok(lockedRsp.kind === 'locked', 'locked by the owner: the 🔒 row');
  ok(gainRowFor({ driver: 'airspyhf', ahfAtt: 2, ahfAgc: true, more: true }).kind === 'auto' && gainRowFor({ driver: 'rtl', gains: [0, 100], gainTenthDb: 100, autoGain: false, more: true }).kind === 'manual',
     'other drivers keep their own name / status');
  const mc = readFileSync(new URL('../src/components/MapChrome.tsx', import.meta.url), 'utf8');
  const sdr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
  const lhp = readFileSync(new URL('../src/components/LocalHardwarePanel.tsx', import.meta.url), 'utf8');
  ok(/row\.kind === 'locked' && <Key label=\{'\\u\{1F512\}'\} words="Gain locked by the owner — enter the admin password" onPress=\{\(\) => setPw\(\(v\) => !v\)\} \/>/.test(mc)
     && /secureTextEntry autoComplete="current-password"/.test(mc) && /const ok = await h\.onUnlock\(pwText\)/.test(mc), 'locked: the 🔒 opens the INLINE admin password box (the sheet\'s own unlock call), no sheet');
  ok(/const canMore = row\.more && !!h\.extras && row\.kind !== 'locked';/.test(mc) && /const extras = canMore && open \? <View style=\{styles\.gainExtras\}>\{h\.extras\}<\/View> : null;/.test(mc),
     '⌄ expands the other controls IN the card; none while locked, none where there are none');
  ok(/get extras\(\) \{ return hwPanelEl \? React\.cloneElement\(hwPanelEl, \{ inline: true, visible: false \}\) : undefined; \}/.test(sdr) && !/onMore: \(\) => \{ markInteract\(\); setHwOpen\(true\); \}/.test(sdr),
     'the extras are the hardware panel\'s own body (per radio.driver, hwinfo-driven) — the map never opens the controls sheet');
  ok(/if \(p\.inline\) return <View style=\{styles\.inlineBody\}>\{body\}<\/View>;/.test(lhp), 'LocalHardwarePanel draws its body alone, inline (no Modal, no keys of its own)');
  ok(/agcName: drv === 'sdrplay' && rspIfAgc !== false && \(rspVagcRunning !== undefined/.test(sdr) && /\? vagcIfOwner\(!!rspVagcRunning\) : undefined,/.test(sdr), 'the name from the server\'s report (rspstat vagc → VibeAGC / SDRplay)');
}

// ── ★★★ beta97 hotfix: a map item's own card (mesh node, ship, station…) never under the chips (Stuart 11-28) ──
{
  const chips = ['servers', 'tune', 'layers', 'stats', 'map', 'chat', 'rate'] as ChipId[];
  const ins = { top: 62, bottom: 34, left: 0, right: 0 };
  const g = chromeGeom({ w: 402, h: 874, insets: ins, chips });
  const a = itemCardArea(g, { winW: 402, winH: 874, insets: ins, k: 1, listH: 160, listW: 402, listShown: true, brTop: 150 });
  const boxes = Object.values(g.boxes) as Array<{ x: number; y: number; w: number; h: number }>;
  const cardL = 402 - a.right - Math.min(340, 402 - a.right - a.left), cardR = 402 - a.right;
  const under = boxes.filter((b) => b.x < cardR && b.x + b.w > cardL && b.y < 874 - a.bottom && b.y + b.h > a.top);
  ok(under.length === 0, `iPhone: the card's room touches no chip (top ${a.top}, right ${a.right}, left ${a.left}, bottom ${a.bottom})`);
  ok(a.top >= g.boxes.exit!.h && a.top >= g.boxes.rate!.h && a.top >= g.pill.y + g.pill.h, 'below the ✕ / RATE corners and the pill (the × reachable)');
  ok(a.bottom >= 160 && a.bottom >= 150, 'above a full-width LIST and the bottom-right group');
  const s2 = chromeGeom({ w: 956, h: 376, insets: { top: 0, bottom: 20, left: 62, right: 62 }, chips });
  const b2 = itemCardArea(s2, { winW: 956, winH: 376, insets: { top: 0, bottom: 20, left: 62, right: 62 }, k: 1, listH: 300, listW: s2.list.w, listShown: true, brTop: 60 });
  ok(b2.top >= 44 && b2.left >= s2.list.x + s2.list.w && b2.right >= 62, 'short landscape: under the top row, right of the side panel, inside the notch');
  const mo = readFileSync(new URL('../src/components/MapOverlay.tsx', import.meta.url), 'utf8');
  ok(/\.vtl-card\{top:var\(--caT,10px\)!important;right:var\(--caR,10px\)!important;/.test(mo) && /max-height:calc\(100% - var\(--caT,10px\) - var\(--caB,10px\)\)!important;/.test(mo),
     'the page places the track layer\'s card in that room; its body scrolls inside it');
  ok(/\.vtl-card \.vtl-x\{position:sticky!important;top:-10px;float:right;margin:-8px -10px 0 6px;min-width:40px;min-height:40px;/.test(mo), 'its × stays at the top while the body scrolls, a full-size target');
  ok(/onItemCardArea=\{\(a\) => p\.pageCall\('__cardsArea'/.test(mo) && /window\.__cardsArea=function\(t,r,b,l\)/.test(mo), 'React Native tells the page (on every change of the chrome)');
}

// ── ★★★ beta97 hotfix the LIST and its folder tabs never on a chip, at any list height (Stuart, TestFlight 589) ──
{
  const chips = ['servers', 'tune', 'layers', 'stats', 'map', 'chat', 'rate', 'health'] as ChipId[];
  const wins = [{ n: 'iPhone 17', w: 402, h: 874, ins: { top: 62, bottom: 34, left: 0, right: 0 }, k: 1 },
                { n: 'iPhone SE', w: 375, h: 667, ins: { top: 20, bottom: 0, left: 0, right: 0 }, k: 1 },
                { n: 'iPad', w: 820, h: 1180, ins: { top: 24, bottom: 20, left: 0, right: 0 }, k: chromeScale(820, 1180) },
                { n: 'Mac window', w: 1440, h: 900, ins: { top: 28, bottom: 0, left: 0, right: 0 }, k: chromeScale(1440, 900) }];
  for (const W of wins) {
    const { g } = chromeGeomAt({ w: W.w, h: W.h, insets: W.ins, chips, layersTallH: 110 }, W.k);
    let worst = '';
    for (let listH = LIST_CHIP_H * W.k; listH <= g.list.maxH + 0.5; listH += 8) {
      const top = W.h - listH - FTAB_H * W.k;   // the tabs' top edge (the drawer sits on the window's foot, tall layout)
      for (const [id, b] of Object.entries(g.boxes) as Array<[string, { x: number; y: number; w: number; h: number }]>) {
        const xOverlap = b.x < g.list.w && b.x + b.w > 0;
        if (xOverlap && b.y + b.h > top + 0.5) { worst = `${id} at list ${Math.round(listH)} (chip foot ${Math.round(b.y + b.h)} > tabs ${Math.round(top)})`; break; }
      }
      if (worst) break;
    }
    ok(!worst, `${W.n}: the drawer and its ≋ / ⌃ tabs stay below every chip they run under, up to the maximised list${worst ? ' — ' + worst : ''}`);
  }
  const mc = readFileSync(new URL('../src/components/MapChrome.tsx', import.meta.url), 'utf8');
  ok((mc.match(/top: -FTAB_H \* k/g) ?? []).length === 2 && /ftab: \{ position: 'absolute', width: 34, height: FTAB_H,/.test(mc), 'the tabs drawn at the height the layout allows for (FTAB_H)');
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
  const o = padOrder(g, { listRows: 100, br: true, pill: true });
  ok(o[0] === 'chip:exit' && o.indexOf('chip:rate') < o.indexOf('pill') && o.indexOf('pill') < o.indexOf('list') && o[o.length - 1] === 'help',
     `short: the top row → the pill → the LIST (one stop) → bottom-right (${o.join(' ')})`);
  ok(!o.some((x) => x.startsWith('row:')) && o.indexOf('help') === o.indexOf('list') + 1, 'the LIST is ONE stop: Down from it goes PAST the rows to the bottom-right group (100 rows, none visited)');
  const inside = padOrder(g, { listInside: true, listRows: 3, br: true });
  ok(inside.join() === 'row:0,row:1,row:2,bar', `ENTER on the LIST steps INTO it: its rows, then its bar (${inside.join(' ')}); BACK steps out`);
  ok(padOrder(g, { cardOpen: true, listRows: 9, br: true }).join() === 'card', 'an open card is its own scope (ENTER opens it, BACK shuts it)');
  const t = chromeGeom({ w: 375, h: 560, insets: { top: 20, bottom: 0, left: 0, right: 0 }, chips: ALL });
  const ot = padOrder(t, { br: false });
  ok(ot[0] === 'chip:exit' && ot.indexOf('chip:stats') < ot.indexOf('chip:rate'), `tall: the left family, then the right column (${ot.join(' ')})`);
}
// ── the highlighted row is scrolled into view (index 40 of 100, from the top and from mid-list) ──
{
  const rowH = 44, viewH = 5 * rowH;
  const at = (i: number, y: number) => i * rowH - y;   // the row's distance from the viewport's top
  const y1 = revealScrollTarget(at(40, 0), rowH, viewH, 0);
  ok(y1 != null && at(40, y1) >= rowH && at(40, y1) + rowH <= viewH - rowH, `row 40 of 100 highlighted from the top → scrolled to ${y1}: fully visible with a row's margin`);
  const y2 = revealScrollTarget(at(41, 1680), rowH, viewH, 1680);
  ok(y2 === null, 'the next row still on screen: no jump');
  const y3 = revealScrollTarget(at(20, 1680), rowH, viewH, 1680);
  ok(y3 != null && at(20, y3) >= 0 && at(20, y3) + rowH <= viewH, 'from mid-list, back up to row 20: scrolled into view');
  ok(revealScrollTarget(at(0, 100), rowH, viewH, 100) === 0, 'never above the top');
}
// ── the chrome's size follows the window (Stuart: "on the iPad/Mac build they are super tiny") ──
{
  ok(chromeScale(390, 844) === 1 && chromeScale(956, 376) === 1, 'a phone (either way up): the reference size');
  ok(Math.abs(chromeScale(820, 1180) - 1180 / 926) < 1e-9 && chromeScale(1440, 900) === 1.45 && chromeScale(2560, 1440) === 1.45, `iPad / Mac windows: by the long side, ≤ 1.45 (${chromeScale(820, 1180).toFixed(2)}, ${chromeScale(1440, 900)})`);
  ok(Math.round(44 * chromeScale(1440, 900) * 0.77) === 49, 'a 1440-pt Mac window at the Mac’s 77 %: the chip ≈ 49 px (the web’s 48)');
  const at = chromeGeomAt({ w: 1440, h: 900, insets: { top: 24, bottom: 0, left: 0, right: 0 }, chips: ALL }, 1.45);
  ok(at.g.boxes.servers!.h === 44 * 1.45 && !at.g.short && at.g.boxes.exit!.x === 0, 'the layout at scale: the same rules, every distance × k');
  const c = cardBoxAt(at, 'tune', 1440, 900);
  ok(c.w <= 340 * 1.45 + 0.01 && c.x >= at.g.boxes.tune!.x + at.g.boxes.tune!.w && c.x + c.w <= 1440, 'a card at scale opens beside its chip, inside the window');
}

// ── SERVERS: LEAVE always, then every other radio with its listeners ──
{
  const radios = [{ id: 'a', label: 'RTL HF', users: 4 }, { id: 'b', label: 'RSP VHF', users: 2, driver: 'sdrplay' }, { id: 'c', label: 'Club radio', pinLocked: true }];
  const r = serversRows({ radios, mine: 'a', viaDoor: true, host: 'pi.local', live: { b: { listeners: 1, max: 2 } } });
  ok(r[0].id === 'back' && /this server’s radios/.test(r[0].label) && r[0].sub === 'pi.local', 'LEAVE first: back to this server’s radios');
  ok(r.length === 3 && !r.some((x) => x.id.endsWith(':a')), 'every OTHER radio, never this one');
  ok(r[1].id === 'r:b' && r[1].label === 'Switch to RSP VHF' && /1\/2 listeners/.test(r[1].sub) && /sdrplay/.test(r[1].sub), `a switch row with its listeners (${r[1].sub})`);
  ok(r[2].id === 'pin:c' && /PIN/.test(r[2].sub), 'a radio behind a PIN goes to the radio list (its PIN is typed there)');
  ok(serversRows({ radios, mine: 'a', viaDoor: true, host: 'h', unlocked: { c: true } })[2].id === 'r:c', 'a PIN radio unlocked this visit switches straight away');
  ok(/\?\/4 listeners/.test(serversRows({ radios: [{ id: 'x', label: 'X' }, { id: 'a', label: 'A', users: 4 }], mine: 'x', viaDoor: true, host: 'h' })[1].sub), 'not polled yet: the limit alone (?/4)');
  const lone = serversRows({ radios: [], mine: null, viaDoor: false, host: 'h' });
  ok(lone.length === 1 && /server list/.test(lone[0].label), 'no door / one radio: LEAVE alone (back to the server list)');
}
// ── SOURCES + LIVE elsewhere ──
{
  const ac = [{ radios: ['a'], src: ['ADS-B'], age: 5 }, { radios: ['b'], src: ['VDL2'], age: 10 }, { radios: ['feed'], src: ['ADS-B'], age: 3 }, { radios: ['b'], src: ['ACARS'], age: 300 }];
  const rows = sourceRows({ aircraft: ac, vessels: [{ radios: ['b'] }], feeds: [{ title: 'PiAware', label: 'beast', state: 'connected', aircraft: 40 }],
                            mine: 'a', onRadio: true, names: { a: 'RTL HF', b: 'RSP VHF' }, hidden: new Set(['b']), liveText: 'ADS-B 12/s' })!;
  ok(rows.length === 3, `a row per radio and per feed (${rows.map((x) => x.name).join(' · ')})`);
  ok(rows[0].name === 'RTL HF' && /this radio · live: ADS-B 12\/s · 1 aircraft/.test(rows[0].what), 'this radio: live, what it holds');
  ok(rows[1].name === 'RSP VHF' && !rows[1].on && /2 aircraft · 1 ships/.test(rows[1].what), 'a hidden radio is off, with its counts');
  ok(rows[2].id === 'feed' && /BEAST feed · connected · 40 aircraft/.test(rows[2].what) && !rows[2].down, 'a feed row, live');
  ok(sourceRows({ aircraft: [{ radios: ['a'] }], vessels: [], feeds: [], mine: 'a', onRadio: true, names: {}, hidden: new Set() }) === null, 'one source: no SOURCES at all');
  const oth = layerOthers(ac, { mine: 'a', onRadio: true, names: { b: 'RSP' }, feedTitle: 'PiAware' });
  ok(JSON.stringify(oth.vdl2) === '["RSP"]' && JSON.stringify(oth.adsb) === '["PiAware"]', `outlined LIVE: another radio / the feed, by name (${JSON.stringify(oth)})`);
  ok(!oth.acars, 'heard more than a minute ago: not live elsewhere');
  ok(!layerOthers(ac, { mine: 'a', onRadio: true, names: {} }).adsb?.includes('SDR'), 'this radio’s own decodes are never “elsewhere”');
  // the scope hides what ONLY hidden sources heard
  const sc = { mine: 'a', others: true, t0: 0, hidden: ['b'] };
  ok(!scopeRadioOk(['b'], sc) && scopeRadioOk(['a', 'b'], sc) && scopeRadioOk([], sc), 'scope: heard only by a hidden source → out; also heard by a shown one → in; unknown → in');
  ok(scopeList([{ r: ['b'] }, { r: ['a'] }], sc, (x) => x.r, () => null).length === 1, 'scopeList applies the hidden sources');
}

// ── FIT TO ONE LAYER ──
{
  const items = [{ key: 'adsb:abc', lat: 1, lon: 1 }, { key: 'a:x', lat: 1, lon: 1, _adsb: { sparse: true } }, { key: 'a:y', lat: 1, lon: 1, _adsb: {} }, { key: 's:123', lat: 1, lon: 1 },
                 { key: 'd:1:2', lat: 1, lon: 1 }, { key: 'w:5:9', lat: 1, lon: 1 }, { key: 'm:3', lat: 1, lon: 1 }, { key: 'vor:1', lat: 1, lon: 1 }, { key: 'sds:R', lat: 1, lon: 1 },
                 { key: 'GB|Niton', lat: 1, lon: 1 }, { key: 'g:5', lat: 1, lon: 1 }, { key: 'a:area', lat: 1, lon: 1, _nofit: true }, { key: 's:no', lat: null, lon: null }];
  const gs = items.map((it) => fitGroupOf(it as never));
  ok(gs.join() === 'adsb,datalink,adsb,ships,dsc,warnings,weather,vor,sondes,navst,other,,', `each item's layer from its own fields (${gs.join()})`);
  ok(fitGroupOf({ key: 's:1', lat: 1, lon: 1, _fg: 'datalink' }) === 'datalink', 'a tagged _fg wins');
  const f = fitGroups(items as never, '', fitMainOf('ais'), fitDefault);
  ok(f.use === 'adsb' && f.groups[0].id === 'adsb' && f.groups[0].n === 2 && !f.groups.some((g) => g.id === 'other'), 'default: ADS-B where there is ADS-B; OTHER never a FIT TO row');
  ok(fitGroups(items as never, 'ships', 'ais', fitDefault).use === 'ships', 'the viewer\u2019s pick wins');
  ok(fitGroups(items as never, 'sondes', 'ais', fitDefault).use === 'sondes' && fitGroups([{ key: 's:1', lat: 1, lon: 1 }] as never, 'sondes', 'ais', fitDefault).use === 'ships', 'a pick with nothing left → the default (the live mode\u2019s: ships)');
  ok(fitGroups([{ key: 'w:1', lat: 1, lon: 1 }] as never, '', '', fitDefault).use === '', 'only NAVTEX warnings: nothing framed (the map keeps where it is)');
  ok(fitMainOf('datalink') === 'datalink' && fitMainOf('vormap') === 'vor' && fitMainOf('') === '', 'the live mode → its layer');
}

// ── NDB in the drawer: the app's own report (ndbSanitise) straight into the shared model ──
{
  const rep = ndbSanitise({ type: 'ndb', centre: 400000, lo: 255000, hi: 526000, carriers: 2, identified: 1, beacons: [
    { id: 1, hz: 338000, ident: 'BIA', active: true, lastId: 50, snr: 12 }, { id: 2, hz: 400500, ident: '', active: false, lastId: 0 }], log: [], copy: [] })!;
  const seen = new Map<string, string>();
  const m0 = wfModelNdb(rep, seen, true);
  ok(m0.lo === 255000 && m0.hi === 526000 && m0.key === 'ndb400000', `the band the radio holds is the span (${m0.lo}–${m0.hi})`);
  ok(m0.marks.length === 2 && m0.marks[0].label === 'BIA 338.0' && m0.marks[1].label === '', 'a marker per beacon; only a decoded ident is labelled');
  ok(m0.stamps.length === 0, 'opening (primed): what was read before is not stamped');
  const rep2 = ndbSanitise({ type: 'ndb', centre: 400000, lo: 255000, hi: 526000, carriers: 2, identified: 2, beacons: [
    { id: 1, hz: 338000, ident: 'BIA', active: true, lastId: 50 }, { id: 2, hz: 400500, ident: 'LA', active: true, lastId: 60 }], log: [], copy: [] })!;
  const m1 = wfModelNdb(rep2, seen, false);
  ok(m1.stamps.length === 1 && m1.stamps[0].text === 'LA' && m1.stamps[0].hz === 400500, `a NEW decoded ident is stamped at its frequency (${JSON.stringify(m1.stamps)})`);
  ok(wfModelNdb(rep2, seen, false).stamps.length === 0, 'the same reading again: not stamped twice');
}

// ── Stuart's LIST flow, step by step (the R36S handheld's): stop → ENTER rows → Down ×3 → ENTER its card → BACK same row → BACK stop ──
{
  const o = { rows: 100, firstPlaced: 2 };
  let st: ReturnType<typeof listFlowStep> = { at: 'stop', row: 0 };
  const step = (k: 'enter' | 'back' | 'up' | 'down') => { if (st !== 'out') st = listFlowStep(st, k, o); return st; };
  step('down');
  ok(st !== 'out' && st.at === 'stop', '1. Down on the LIST stop does not enter it (the chrome’s walk moves on past it)');
  step('enter');
  ok(st !== 'out' && st.at === 'rows' && st.row === 2, '2. ENTER → into the list: the first row with a place on the map');
  step('down'); step('down'); step('down');
  ok(st !== 'out' && st.at === 'rows' && st.row === 5, '3. Down × 3 → row by row (the list scrolls to keep it in view)');
  step('enter');
  ok(st !== 'out' && st.at === 'item' && st.row === 5, '4. ENTER on a row → its card, the highlight in it');
  step('down');
  ok(st !== 'out' && st.at === 'item', '   (Up / Down in the card are the card’s own keys)');
  step('back');
  ok(st !== 'out' && st.at === 'rows' && st.row === 5, '5. BACK in the card → the card shut, the highlight on the SAME row');
  step('back');
  ok(st !== 'out' && st.at === 'stop', '6. BACK again → out of the list, the highlight on the LIST stop');
  step('back');
  ok(st === 'out', '   BACK on the stop passes on (the chrome’s BACK: a card, then the map’s exit)');
  ok(listFlowStep({ at: 'rows', row: 99 }, 'down', o) !== 'out' && (listFlowStep({ at: 'rows', row: 99 }, 'down', o) as { row: number }).row === 99, 'the last row stays the last');
  ok((listFlowStep({ at: 'stop', row: 0 }, 'enter', { rows: 0, firstPlaced: 0 }) as { at: string }).at === 'stop', 'an empty list: ENTER stays on the stop');
}

// ── the LAYERS chip grows to hold its families (Stuart, build 586: "the chip doesn't grow to accommodate it") ──
{
  const g = chromeGeom({ w: 440, h: 956, insets: { top: 62, bottom: 34, left: 0, right: 0 }, chips: ALL, layersTallH: 2 * 31 + 10, layersTallW: 16 + 3 * 7.2 });
  const ly = g.boxes.layers!;
  ok(ly.h === 72 && ly.w >= 44 && g.boxes.stats!.y >= ly.y + ly.h, `tall: LAYERS as tall as its two families (${ly.h} pt) and the next chip below it`);
  const g1 = chromeGeom({ w: 440, h: 956, insets: { top: 62, bottom: 34, left: 0, right: 0 }, chips: ALL, layersTallH: 10, layersTallW: 10 });
  ok(g1.boxes.layers!.h === 44 && g1.boxes.layers!.w === 44, 'never smaller than a chip');
  const wide = chromeGeom({ w: 440, h: 956, insets: { top: 62, bottom: 34, left: 0, right: 0 }, chips: ALL, layersTallW: 16 + 5 * 7.2 });
  ok(wide.boxes.layers!.w > 44, 'a five-figure count widens it');
}

console.log(`\n${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
