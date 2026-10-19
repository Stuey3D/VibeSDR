// The controller guide's shared data and layout rules (src/utils/padGuide.ts) — Stuart approved the prototype 2026-10-19.
import { PAD_GUIDES, PAD_LAYOUTS, guideColumns, guideTabStep, leaderPath, viewToBox } from '../src/utils/padGuide';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };

console.log('test_pad_guide');
for (const l of PAD_LAYOUTS) {
  const g = PAD_GUIDES[l];
  const [vx, vy, vw, vh] = g.view;
  ok(g.items.every((it) => it.at[0] >= vx && it.at[0] <= vx + vw && it.at[1] >= vy && it.at[1] <= vy + vh),
     `${l}: every marker sits inside the drawing`);
  ok(g.items.some((it) => it.t === 'SELECT' && it.s === 'this guide' && it.kind === 'meta'), `${l}: SELECT — this guide (ink)`);
  ok(g.items.some((it) => /START/.test(it.t) && it.kind === 'meta'), `${l}: HOLD START — switch layout (ink)`);
  ok(g.items.filter((it) => it.kind === 'back').length === 1 && g.items.find((it) => it.kind === 'back')!.t.includes('BACK'),
     `${l}: one BACK, coloured as back (amber)`);
  ok(g.items.some((it) => it.side === 'left') && g.items.some((it) => it.side === 'right'), `${l}: labels on both sides`);
  ok(g.art.length > 5, `${l}: a drawing`);
}
// The words match the scheme gamepad.ts / padActions.ts implement.
const st = PAD_GUIDES.sticks.items.map((i) => i.t).join(' | ');
ok(/BOTTOM BUTTON — ENTER/.test(st) && /RIGHT BUTTON — BACK/.test(st), 'sticks: bottom = enter, right = back');
ok(/LEFT STICK/.test(st) && /RIGHT STICK/.test(st), 'sticks: left stick tunes, right stick zooms');
const bt = PAD_GUIDES.buttons.items.map((i) => i.t).join(' | ');
ok(/R — ENTER/.test(bt) && /L — BACK/.test(bt), 'buttons: R = enter, L = back');
ok(/TOP — ZOOM IN/.test(bt) && /BOTTOM — ZOOM OUT/.test(bt) && /LEFT — TUNE DOWN/.test(bt) && /RIGHT — TUNE UP/.test(bt),
   'buttons: face buttons by position tune and zoom');
// Arrowheads (shapes, not SVG markers): both ends of each turn arrow, drawn as closed paths in the action colour.
ok(PAD_GUIDES.sticks.art.filter((a) => a.k === 'path' && a.fill === 'action').length === 4, 'sticks: two turn arrows, a head at each end');

ok(guideColumns(1200) === 3 && guideColumns(901) === 3, 'wide: three columns with lines');
ok(guideColumns(900) === 2 && guideColumns(700) === 2, 'narrow: the pad on top, the list in two columns');
ok(guideColumns(520) === 1 && guideColumns(360) === 1, 'phone: one column');

ok(guideTabStep('sticks', 1) === 'buttons' && guideTabStep('buttons', -1) === 'sticks', 'D-pad left / right moves between the tabs');
ok(guideTabStep('buttons', 1) === 'buttons' && guideTabStep('sticks', -1) === 'sticks', '…and stops at the ends');

ok(leaderPath(100, 50, 20, 80, 'left') === 'M100.0,50.0 L44.0,80.0 L20.0,80.0 ', 'leader line: diagonal, then level into the number (left)');
ok(leaderPath(10, 50, 200, 80, 'right') === 'M10.0,50.0 L176.0,80.0 L200.0,80.0 ', '…and on the right');
const [x, y] = viewToBox([0, 0, 200, 100], 400, 400, 100, 50);
ok(x === 200 && y === 200, 'viewBox → box: "meet" scales to fit and centres');

console.log(fails ? `${fails} FAILED` : 'all passed');
process.exit(fails ? 1 : 0);
