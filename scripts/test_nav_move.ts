// navMove (src/utils/navMove.ts — PanelNav's movement rule) and the frequency card's tune pane (src/utils/freqCardNav.ts).
// ★ Stuart, V12 485: "no way of selecting the frequency box" — the field sat after the units in a flat list.
import { navMove, type NavMoveKey } from '../src/utils/navMove';
import { tuneCardItems } from '../src/utils/freqCardNav';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };

console.log('test_nav_move');

// ── The flat list: every item its own row, left/right act as up/down (StepPicker, ModeSelector, menus) ──
const flat = [0, 1, 2, 3];
ok(navMove(flat, -1, 'ArrowDown', true) === 0, 'flat: first movement takes focus at the top');
ok(navMove(flat, 0, 'ArrowDown', true) === 1 && navMove(flat, 1, 'ArrowRight', true) === 2, 'flat: down / right step on');
ok(navMove(flat, 2, 'ArrowUp', true) === 1 && navMove(flat, 2, 'ArrowLeft', true) === 1, 'flat: up / left step back');
ok(navMove(flat, 3, 'ArrowDown', true) === 3 && navMove(flat, 0, 'ArrowUp', true) === 0, 'flat: clamps at both ends');
ok(navMove([], -1, 'ArrowDown', true) === -1, 'empty: nowhere to go');

// ── A grid ──
const grid = [0, 0, 1, 2, 2, 2];
ok(navMove(grid, 0, 'ArrowRight', false) === 1 && navMove(grid, 1, 'ArrowRight', false) === -1, 'grid: right walks the row, stops at its end');
ok(navMove(grid, 1, 'ArrowLeft', false) === 0 && navMove(grid, 0, 'ArrowLeft', false) === -1, 'grid: left walks back, stops at its start');
ok(navMove(grid, 1, 'ArrowDown', false) === 2, 'grid: down goes to the next row');
ok(navMove(grid, 2, 'ArrowDown', false) === 3, 'grid: down lands on the first of the next row');
ok(navMove(grid, 5, 'ArrowUp', false) === 2 && navMove(grid, 3, 'ArrowUp', false) === 2, 'grid: up from any of a row lands on the row above');
ok(navMove(grid, 2, 'ArrowUp', false) === 1, 'grid: up lands on the LAST of the row above');

// ── The frequency card's tune pane, as drawn ──
const walk = (rows: number[], from: number, keys: NavMoveKey[]) => {
  let i = from;
  for (const k of keys) { const t = navMove(rows, i, k, false); if (t >= 0) i = t; }
  return i;
};
for (const tabs of [true, false]) for (const vts of [true, false]) for (const lockUnit of [true, false]) for (const profiles of [true, false]) {
  const { items, start } = tuneCardItems({ tabs, vts, lockUnit, profiles });
  const rows = items.map((x) => x.row);
  const at = (k: string) => items.findIndex((x) => x.k === k);
  const tag = `[tabs ${tabs} vts ${vts} lock ${lockUnit} profiles ${profiles}]`;
  ok(start >= 0 && items[start].k === 'field', `${tag} the highlight starts on the frequency box`);
  ok(rows.every((r, i) => i === 0 || rows[i - 1] <= r), `${tag} rows run down the screen in order`);
  // Every unit: one Up is the field (the bug: Up went to the tabs).
  for (const u of ['unit:hz', 'unit:khz', 'unit:mhz']) {
    if (at(u) >= 0) ok(walk(rows, at(u), ['ArrowUp']) === start, `${tag} Up from ${u} is the frequency box`);
  }
  // From the field, Down is the first unit present.
  ok(items[walk(rows, start, ['ArrowDown'])].k.startsWith('unit:'), `${tag} Down from the frequency box is a unit`);
  // From either tab, Down reaches the field (through the station skip when it is there).
  if (tabs) for (const t of ['tab:tune', 'tab:bookmarks']) {
    ok(walk(rows, at(t), vts ? ['ArrowDown', 'ArrowDown'] : ['ArrowDown']) === start, `${tag} Down from ${t} reaches the frequency box`);
  }
  if (vts) ok(walk(rows, at('vts:prev'), ['ArrowDown']) === start && walk(rows, at('vts:next'), ['ArrowDown']) === start,
              `${tag} Down from either station arrow is the frequency box`);
  if (profiles) ok(walk(rows, at('profile'), ['ArrowUp', 'ArrowUp']) === start, `${tag} the profile is two Ups from the frequency box`);
  ok(walk(rows, -1, ['ArrowDown']) === 0, `${tag} with nothing highlighted, the first arrow takes the top`);
}

console.log(fails ? `test_nav_move: ${fails} FAILED` : 'test_nav_move: all ok');
// ★ The action row (controller audit group C, 2026-10-20): CANCEL / SHARE / TUNE on one row below everything, SHARE only
//   when it can be pressed; the highlight still starts on the frequency box.
{
  const a = tuneCardItems({ tabs: true, vts: false, lockUnit: false, profiles: true, actions: { share: true } });
  const last = a.items.slice(-3).map((x) => x.k).join(',');
  ok(last === 'act:cancel,act:share,act:tune', `action row in order (${last})`);
  ok(new Set(a.items.slice(-3).map((x) => x.row)).size === 1 && a.items.slice(-3)[0].row > a.items[a.items.length - 4].row, 'one row, below the rest');
  ok(a.items[a.start].k === 'field', 'still starts on the frequency box');
  const b = tuneCardItems({ tabs: false, vts: false, lockUnit: false, profiles: false, actions: { share: false } });
  ok(!b.items.some((x) => x.k === 'act:share') && b.items.some((x) => x.k === 'act:tune'), 'no SHARE stop when sharing is off');
}
process.exit(fails ? 1 : 0);
