// test_map_cards.ts — ★★★ beta96 THE MAP CONTROL CARDS' RULES (src/utils/mapCards.ts; docs/v12/MAP-CONTROL-CARDS.md): the
// window decides how many cards (one on a phone, never the whole width), the oldest gives way, LIST alone in its column,
// the defaults, FIT's outlier rule and its default layer.   npx tsx scripts/test_map_cards.ts
import { CARD_IDS, cardColumns, cardDefaults, cardLimit, cardToggle, fitDefault, rateWords, trimOutliers, type CardId } from '../src/utils/mapCards';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };

// ── how many: the window, not the device ──
ok(cardLimit(390, 844) === 1, 'a 390 px phone: one card');
ok(cardLimit(599, 1200) === 1, '599 px (still phone-width): one card');
ok(cardLimit(820, 1180) === 2, `820 × 1180 (an iPad, or a phone unfolded): two — one column, two stacked (${cardLimit(820, 1180)})`);
ok(cardLimit(1440, 900) === 4, `1440 × 900: four — two columns of two (${cardLimit(1440, 900)})`);
ok(cardLimit(1440, 600) === 2, `1440 × 600 (short): one card per column (${cardLimit(1440, 600)})`);
ok(cardLimit(2560, 1400) === 6, `2560 × 1400: every card (${cardLimit(2560, 1400)})`);
for (const [w, h] of [[820, 1180], [1024, 768], [1440, 900], [1920, 1080], [2560, 1400]]) {
  const cols = Math.ceil(cardLimit(w, h) / (h >= 700 ? 2 : 1));
  ok(cols * 348 <= w * 0.72 + 1 || cols === 1, `${w} × ${h}: the columns never pass 72 % of the width (${cols} × 348 of ${w})`);
}
// ── opening: the oldest gives way ──
let open: CardId[] = [];
for (const id of CARD_IDS) open = cardToggle(open, id, true, 390, 844);
ok(open.length === 1 && open[0] === 'sources', `phone: six opened in turn leave the last (${open})`);
open = [];
for (const id of ['list', 'view', 'map', 'key', 'sources'] as CardId[]) open = cardToggle(open, id, true, 1440, 900);
ok(open.join() === 'view,map,key,sources', `1440 × 900: five opened leave the last four (${open})`);
open = cardToggle(open, 'map', false, 1440, 900);
ok(open.join() === 'view,key,sources', `shut one: ${open}`);
open = cardToggle(open, 'view', true, 1440, 900);
ok(open.join() === 'key,sources,view', `opening an open one brings it to the top: ${open}`);
// ── columns: LIST alone ──
ok(JSON.stringify(cardColumns(['view', 'list', 'map'], 1440, 900)) === '[["list"],["view","map"]]', `LIST has its own column: ${JSON.stringify(cardColumns(['view', 'list', 'map'], 1440, 900))}`);
ok(JSON.stringify(cardColumns(['view', 'map'], 390, 844)) === '[["map"]]', 'a phone shows the newest only');
ok(JSON.stringify(cardColumns(['view', 'map', 'key'], 1440, 600)) === '[["map"],["key"]]', `short and wide: one per column, the newest two (${JSON.stringify(cardColumns(['view', 'map', 'key'], 1440, 600))})`);
// ── defaults ──
ok(cardDefaults(390).length === 0, 'a phone opens with nothing (the map is the point)');
ok(cardDefaults(1024).join() === 'list', 'a wide window opens with LIST');
// ── FIT: outliers ──
const rx = { lat: 52.3125, lon: -0.875 };
const uk = Array.from({ length: 40 }, (_, i) => ({ lat: 51 + (i % 8) * 0.3, lon: -2 + Math.floor(i / 8) * 0.5, id: i }));
const tallinn = { lat: 59.437, lon: 24.7536, id: 99 };
const t = trimOutliers([...uk, tallinn], rx);
ok(!t.includes(tallinn) && t.length === 41 - 3, `Tallinn (949 NM) among 41 UK aircraft is left out — the furthest 5 % (${41 - t.length} dropped)`);
ok(trimOutliers(uk.slice(0, 7), rx).length === 7, 'fewer than 8: nothing is an outlier');
ok(trimOutliers([...uk.slice(0, 7), tallinn], rx).length === 7, '8 points: the furthest one goes');
const noRx = trimOutliers([...uk, tallinn], null);
ok(!noRx.includes(tallinn), 'with no receiver, measured from the median point: Tallinn still goes');
// ── FIT: the default layer ──
ok(fitDefault({ adsb: 140, datalink: 5, ships: 3 }, 'datalink') === 'adsb', 'ADS-B where there is ADS-B (even in the DATALINK mode)');
ok(fitDefault({ datalink: 5, ships: 3 }, 'datalink') === 'datalink', 'no ADS-B: the live mode\'s own layer');
ok(fitDefault({ ships: 3, navst: 25, warnings: 19 }, '') === 'navst', 'no ADS-B, nothing live: the fullest layer');
ok(fitDefault({ other: 9 }, '') === '', 'only unnamed items: none');
// ── the rate's words ──
ok(rateWords(1425.4) === '1,425' && rateWords(98.6) === '99' && rateWords(4.25) === '4.3', `the rate: ${rateWords(1425.4)} · ${rateWords(98.6)} · ${rateWords(4.25)}`);
console.log(`test_map_cards: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
