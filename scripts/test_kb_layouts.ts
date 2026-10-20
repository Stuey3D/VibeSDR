// The controller keyboard's layouts (src/utils/kbLayouts.ts) — brief, decided 2026-10-20.
import { kbKeys, kbMove, kbStartKey, layoutFor, type KbMode, type KbLayer, type KbKey } from '../src/utils/kbLayouts';
import type { Dir } from '../src/utils/gamepadNav';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
const ids = (k: KbKey[]) => k.map((x) => x.id);

console.log('test_kb_layouts');
const modes: KbMode[] = ['frequency', 'pin', 'search', 'text', 'password', 'url'];
const layers: KbLayer[] = ['lower', 'upper', 'caps', 'symbols', 'accents', 'accentsUp'];
// ★ Decided 10-20: a visible, focusable ↵ and Back on every layout, as well as ⌫.
for (const m of modes) for (const l of layoutFor(m) === 'numeric' ? ['lower' as KbLayer] : layers) {
  const k = kbKeys(m, l);
  const has = (t: string) => k.some((x) => x.act.t === t);
  ok(has('enter') && has('back') && has('bs'), `${m}/${l}: ↵, Back and ⌫ are keys`);
  // No two keys overlap, and every key has a real size.
  let overlap = false;
  for (let i = 0; i < k.length; i++) for (let j = i + 1; j < k.length; j++) {
    const a = k[i], b = k[j];
    if (a.x < b.x + b.w - 1e-6 && b.x < a.x + a.w - 1e-6 && a.y < b.y + b.h - 1e-6 && b.y < a.y + a.h - 1e-6) overlap = true;
  }
  ok(!overlap && k.every((x) => x.w > 0 && x.h > 0), `${m}/${l}: keys never overlap`);
  ok(new Set(ids(k)).size === k.length, `${m}/${l}: ids unique`);
  // Every key reachable from the start by arrows alone (BFS over the four directions).
  const start = Math.max(0, k.findIndex((x) => x.id === kbStartKey(m)));
  const seen = new Set([start]); const q = [start];
  while (q.length) { const c = q.shift()!; for (const d of ['up', 'down', 'left', 'right'] as Dir[]) { const n = kbMove(k, c, d); if (n >= 0 && !seen.has(n)) { seen.add(n); q.push(n); } } }
  ok(seen.size === k.length, `${m}/${l}: every key reachable by the D-pad (${seen.size}/${k.length})`);
}
// QWERTY rows are ten units wide.
for (const m of ['text', 'password', 'url', 'search'] as KbMode[]) {
  const k = kbKeys(m, 'lower');
  for (let r = 0; r <= 4; r++) {
    const row = k.filter((x) => x.y === r);
    const w = row.reduce((a, x) => a + x.w, 0);
    ok(Math.abs(w - (r === 2 ? 10 : 10)) < 1e-6, `${m} row ${r} is ten units (${w})`);
  }
}
// Modes.
const num = kbKeys('frequency', 'lower', { unitLabel: 'kHz' });
ok(num.some((x) => x.id === '.') && num.find((x) => x.id === 'unit')?.label === 'kHz', 'frequency: a point and the unit key');
ok(!kbKeys('frequency', 'lower', { unitLocked: true }).some((x) => x.id === 'unit'), 'a locked unit: no unit key');
const pin = kbKeys('pin');
ok(!pin.some((x) => x.id === '.' || x.id === 'unit'), 'PIN: digits only — no point, no unit');
const url = kbKeys('url', 'upper');
ok(url.every((x) => x.label !== 'Q') && url.some((x) => x.label === '/') && url.some((x) => x.label === ':') && !url.some((x) => x.id === 'space'), 'url: no capitals, / and : on the base layer, no space');
ok(kbKeys('text', 'upper').some((x) => x.label === 'Q') && kbKeys('text', 'lower').some((x) => x.label === 'q'), 'text: ⇧ gives capitals');
ok(kbKeys('password').some((x) => x.act.t === 'reveal') && !kbKeys('text').some((x) => x.act.t === 'reveal'), 'password alone has SHOW / HIDE');
ok(kbKeys('text', 'accents').some((x) => x.label === 'é') && kbKeys('text', 'accentsUp').some((x) => x.label === 'É'), 'accents, lower and upper');
ok(!kbKeys('url').some((x) => x.act.t === 'accents'), 'url: no accents');
// Wide keys and wrapping.
const t = kbKeys('text');
const at = (id: string) => t.findIndex((x) => x.id === id);
ok(t[kbMove(t, at('space'), 'up')].y === 3, 'up from space lands on the row above');
ok(t[kbMove(t, at('lb'), 'down')].id === 'space', 'down from b lands on space (the wide key below)');
ok(kbMove(t, at('lq'), 'left') === at('lp'), 'left from q wraps to p');
ok(kbMove(t, at('lp'), 'right') === at('lq'), 'right from p wraps to q');
ok(t[kbMove(t, at('d1'), 'up')].y === 4, 'up from the top row wraps to the bottom of that column');
const n = num;
const nat = (id: string) => n.findIndex((x) => x.id === id);
ok(kbMove(n, nat('9'), 'right') === nat('bs'), 'numeric: right from 9 reaches ⌫');
ok(kbMove(n, nat('enter'), 'right') === nat('back'), 'numeric: right from ↵ reaches Back');
ok(kbStartKey('frequency') !== 'enter' && kbStartKey('text') !== 'enter', '↵ is never where the highlight starts');
process.exit(fails ? 1 : 0);
