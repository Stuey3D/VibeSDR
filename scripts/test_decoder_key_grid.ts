// test_decoder_key_grid.ts — V12 the Mode panel's uniform decoder keys and options that pop out of their key
// (src/utils/decoderKeyGrid.ts, Stuart 2026-10-29: "make the buttons for the decoders one uniform size … left justify them
// under their category title" — "the options look like they have popped out of the decoder button itself onto the row
// below"). Widths from an iPhone SE in Display Zoom up to an iPad / Mac sheet, and the row an option panel opens under.
// Also: the app and the web both use it (no second copy of the rule), and the app's grid follows the MEASURED panel,
// never the device.
// Run: npx tsx scripts/test_decoder_key_grid.ts
import { readFileSync } from 'node:fs';
import { colsFor, decoderKeyGrid, gridRows, keyNeed, optionsAfter, optionsInsertAt, tabCorners, type KeyLegend,
         type KeyMetrics } from '../src/utils/decoderKeyGrid';

let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };

// The app's metal metrics (ModeSelector): 7 pt gap, 8 pt padding + 1 pt border, a 13 pt glyph 5 pt from the legend.
const M: KeyMetrics = { gap: 7, padX: 9, iconW: 13, iconGap: 5, slack: 6, maxCols: 5 };
// Legends as measured at 12 pt bold Atkinson (round numbers near the real ones): DIGITAL SPOTS is the widest.
const L = (w: number, icon = true): KeyLegend => ({ w, icon });
const PANEL: KeyLegend[] = [L(42) /* ADS-B */, L(26) /* AIS */, L(30), L(26), L(30), L(84) /* SPOTS: BAND */,
  L(56) /* NAVTEX */, L(30) /* DSC */, L(50) /* WEFAX */, L(100) /* DIGITAL SPOTS */, L(20), L(28), L(36), L(36), L(40),
  L(42), L(36) /* TIME */, L(62, false) /* SCANNER */, L(56, false) /* GALLERY */, L(80, false) /* TRANSCRIBE */];

console.log('▶ the key size is the widest legend plus its glyph');
ok(keyNeed(PANEL, M, false) === 100 + 13 + 5 + 18 + 6, 'beside: 100 + 13 + 5 + 2×9 + 6 = 142');
ok(keyNeed(PANEL, M, true) === 100 + 18 + 6, 'above: the legend alone decides (the glyph is narrower) = 124');
ok(keyNeed([L(5)], M, true) === 13 + 18 + 6, 'above: a legend narrower than its glyph is the glyph\'s width');
ok(keyNeed([L(80, false)], M, false) === 80 + 18 + 6, 'a key with no glyph adds none');

console.log('▶ columns at each width (the panel\'s own, after the sheet\'s 16 pt padding)');
const at = (w: number) => decoderKeyGrid(PANEL, w, M);
const SE_ZOOM = 320 - 32, SE = 375 - 32, PRO = 393 - 32, MAX = 440 - 32, LAND = 640 - 32, WIDE = 2000;
for (const [w, name] of [[SE_ZOOM, 'SE in Display Zoom'], [SE, 'SE / mini'], [PRO, '6.1" phone'], [MAX, 'Pro Max'], [LAND, 'iPad / Mac sheet (640 cap)']] as const) {
  const g = at(w);
  ok(g.cols >= 2, `${name} (${w}): at least two keys a row (${g.cols}${g.stack ? ', glyph above' : ''})`);
  ok(g.keyW * g.cols + (g.cols - 1) * M.gap <= w + 0.01, `${name}: the row fits the panel exactly`);
  ok(g.keyW >= keyNeed(PANEL, M, g.stack) - 0.01, `${name}: every key holds the widest legend (${g.keyW.toFixed(1)} ≥ ${keyNeed(PANEL, M, g.stack)})`);
}
ok(!at(LAND).stack && at(LAND).cols === 4, 'the 640 pt sheet: four keys a row, glyphs beside');
ok(at(WIDE).cols === 5, 'a very wide panel stops at five (maxCols) — no row of tiny keys');
ok(at(SE_ZOOM).cols === 2, 'SE in Display Zoom: two a row');
// the stack fallback: beside would leave ONE key a row
const tight = decoderKeyGrid(PANEL, 260, M);
ok(tight.stack && tight.cols === 2, 'a 260 pt panel: the glyph goes above, which keeps two a row (beside would be one)');
ok(!decoderKeyGrid(PANEL, 600, M).stack, 'never above where beside already holds three or more');
ok(!decoderKeyGrid([L(100, false)], 250, M).stack, 'no glyphs at all: never "above"');
ok(decoderKeyGrid(PANEL, 0, M).cols === 1 && decoderKeyGrid(PANEL, 0, M).keyW === 0, 'an unmeasured (0) panel: one column, nothing drawn wide');
ok(colsFor(100, 200, 7) === 1, 'a key wider than the panel still gets one column');

console.log('▶ one MORE key (SITOR-A joining Maritime) changes nothing but the row count');
const plus = [...PANEL, L(60) /* SITOR-A */];
for (const w of [SE_ZOOM, PRO, LAND]) {
  ok(decoderKeyGrid(plus, w, M).cols === decoderKeyGrid(PANEL, w, M).cols, `${w}: same columns with a seventh-length legend added`);
}
const maritime = ['navtex', 'dsc', 'wefax', 'sitora'];
ok(gridRows(maritime, 3).map((r) => r.length).join() === '3,1', 'four Maritime keys at three a row: 3 + 1 (the short row ends early)');

console.log('▶ rows: left-justified, in order, the last one short');
ok(JSON.stringify(gridRows([1, 2, 3, 4, 5, 6, 7], 3)) === '[[1,2,3],[4,5,6],[7]]', '7 keys at 3: [3, 3, 1]');
ok(JSON.stringify(gridRows([1, 2], 4)) === '[[1,2]]', '2 keys at 4: one short row');
ok(gridRows([], 3).length === 0, 'no keys, no rows');
ok(gridRows([1, 2, 3], 0).length === 3, 'a nonsense column count is one a row');

console.log('▶ the options open on the row directly under THEIR key\'s row');
// Amateur at 4 a row: DIGITAL SPOTS CW PSK RTTY | JTTY SSTV APRS — RTTY (3) opens under row 0, APRS (6) under row 1.
ok(optionsAfter(3, 4) === 0 && optionsInsertAt(3, 4, 7) === 4, 'RTTY (4th of 7, 4 a row): under row 0, after key 4');
ok(optionsAfter(6, 4) === 1 && optionsInsertAt(6, 4, 7) === 7, 'APRS (7th of 7): under the last (short) row, after the last key');
ok(optionsAfter(0, 2) === 0 && optionsInsertAt(0, 2, 3) === 2, 'NAVTEX at 2 a row: after DSC, before WEFAX — the keys after it keep their order');
ok(optionsInsertAt(2, 2, 3) === 3, 'WEFAX at 2 a row: after itself (alone on row 1)');
ok(optionsAfter(-1, 3) === -1 && optionsInsertAt(-1, 3, 5) === -1, 'a key not drawn opens nothing');
for (const cols of [1, 2, 3, 4, 5]) for (let i = 0; i < 9; i++) {
  const ins = optionsInsertAt(i, cols, 9);
  if (!(ins > i && ins - i <= cols && (ins % cols === 0 || ins === 9))) ok(false, `insert after ${i} at ${cols} a row = ${ins}`);
}
ok(true, 'every key, every column count: the panel goes after the key\'s row ends, never before the key or past the next row');

console.log('▶ the tab: the panel\'s corner under the key is square');
ok(JSON.stringify(tabCorners(0, 3)) === '{"left":true,"right":false,"col":0}', 'first column: the left corner');
ok(JSON.stringify(tabCorners(2, 3)) === '{"left":false,"right":true,"col":2}', 'last column: the right corner');
ok(JSON.stringify(tabCorners(4, 3)) === '{"left":false,"right":false,"col":1}', 'a middle key: both rounded (the tab sits inside the edge)');
ok(JSON.stringify(tabCorners(0, 1)) === '{"left":true,"right":true,"col":0}', 'one a row: both');

console.log('▶ both clients use the one rule; the app measures the panel, never the device');
const app = readFileSync(new URL('../src/components/ModeSelector.tsx', import.meta.url), 'utf8');
const web = readFileSync(new URL('../web/client/src/decoderBandsDom.ts', import.meta.url), 'utf8');
ok(/from '\.\.\/utils\/decoderKeyGrid'/.test(app) && /decoderKeyGrid\(legends, panelW/.test(app), 'ModeSelector sizes its keys with decoderKeyGrid from the measured panel width');
ok(/from '\.\.\/\.\.\/\.\.\/src\/utils\/decoderKeyGrid'/.test(web) && /decoderKeyGrid\(legends, groups\.clientWidth/.test(web), 'the web sizes its keys with the same function, from the panel\'s own width');
ok(!/MODE_ICON_STACK_BELOW/.test(app), 'the app no longer decides "glyph above" from the window width');
ok(/onLayout=\{onPanelLayout\}/.test(app), 'the app measures the decoder section\'s own width (onLayout)');
ok(!/minWidth: '47%'/.test(app), 'DIGITAL SPOTS no longer spans two columns');
ok(/<KeyGrid items=\{keys\} grid=\{keyGrid\} openId=\{openDec\} options=\{openOpts\}/.test(app), 'the band groups open the selected decoder\'s options under its key');
ok(/VIEWS/.test(app) && /\{ id: 'gallery'/.test(app) && /\{ id: 'transcribe'/.test(app) && /\{ id: 'map'/.test(app), 'VIEWS holds MAP, GALLERY and TRANSCRIBE');
ok(/placeOptions\(activeDec\)/.test(readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8')), 'the web places the selected decoder\'s settings under its key');
const html = readFileSync(new URL('../web/client/index.html', import.meta.url), 'utf8');
ok(/<h2>VIEWS<\/h2>[\s\S]{0,200}id="mapBtn"[\s\S]{0,400}id="galleryBtn"/.test(html), 'the web\'s VIEWS holds MAP and GALLERY');
ok(!/<h2>DECODED SPOTS<\/h2>/.test(html), 'the web\'s DECODED SPOTS heading is gone');

console.log(`test_decoder_key_grid: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
