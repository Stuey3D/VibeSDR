// The keyboard's one scheme (src/utils/keyActions.ts) — Stuart, 2026-10-18.
import { keyAction, keyDir, keyRepeats, toHwKey, KeyBus } from '../src/utils/keyActions';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
const is = (k: string, want: string | null, mods?: any) => {
  const got = keyAction(k, mods);
  ok(got === want, `${JSON.stringify(k)}${mods ? ' ' + JSON.stringify(mods) : ''} -> ${want} (got ${got})`);
};

console.log('test_key_actions');
for (const [k, a] of [['ArrowUp', 'up'], ['ArrowDown', 'down'], ['ArrowLeft', 'left'], ['ArrowRight', 'right'],
                      ['w', 'up'], ['a', 'left'], ['s', 'down'], ['d', 'right'],
                      ['W', 'up'], ['A', 'left'], ['S', 'down'], ['D', 'right']] as const) is(k, a);
is('Enter', 'enter');
is('Backspace', 'back'); is('Escape', 'back'); is('Delete', 'back');
is('<', 'tuneDown'); is(',', 'tuneDown'); is('>', 'tuneUp'); is('.', 'tuneUp');
is('-', 'zoomOut'); is('_', 'zoomOut'); is('+', 'zoomIn'); is('=', 'zoomIn');
// The old shortcuts: nothing.
for (const k of ['m', 'M', 't', 'T', '[', ']', 'c', 'C', 'f', 'F', 'e', 'E', 'h', 'k', 'b', 'p', 'r', 'u', 'l', 'n',
                 'Tab', ' ', 'Space', '1', '0', 'Home', 'PageUp', 'F1', '', 'Shift']) is(k, null);
// Ctrl / Cmd / Alt: never ours (Ctrl-+ page zoom, Cmd-W, Alt-arrow).
is('+', null, { ctrl: true }); is('=', null, { meta: true }); is('ArrowLeft', null, { alt: true }); is('w', null, { meta: true });
is('-', 'zoomOut', { ctrl: false, meta: false, alt: false });
ok(keyAction(undefined) === null && keyAction(null) === null, 'no key -> null');
ok(keyDir('up') === 'up' && keyDir('tuneUp') === null && keyDir(null) === null, 'keyDir picks out movement');
ok(keyRepeats('left') && keyRepeats('tuneUp') && keyRepeats('zoomIn') && !keyRepeats('enter') && !keyRepeats('back')
   && !keyRepeats(null), 'movement, tune and zoom repeat; enter and back never do');


// ── The app: canonical names, typing, and the bus ──
const hk = (key: string, extra: any = {}) => toHwKey({ key, ...extra });
ok(hk('w')?.key === 'ArrowUp' && hk('w')?.plain === false, 'app: W arrives as ArrowUp, not plain (FKA detector)');
ok(hk('ArrowUp')?.key === 'ArrowUp' && hk('ArrowUp')?.plain === true, 'app: the real arrow is plain');
ok(hk('ArrowUp', { plain: false })?.plain === false, 'app: an arrow the native side marked synthesised stays not plain');
ok(hk('Escape')?.key === 'Back' && hk('Backspace')?.key === 'Back' && hk('Delete')?.key === 'Back', 'app: Escape, Backspace, Delete = Back');
ok(hk('<')?.key === 'TuneDown' && hk('.')?.key === 'TuneUp' && hk('_')?.key === 'ZoomOut' && hk('=')?.key === 'ZoomIn', 'app: tune and zoom names');
ok(hk('M') === null && hk('Tab') === null && hk('Space') === null && hk('P') === null, 'app: old shortcut keys never reach a listener');
ok(hk('Enter', { typing: true })?.typing === true && hk('Escape', { typing: true })?.key === 'Back', 'app: typing — Enter and Escape still reported, marked typing');
ok(hk('Backspace', { typing: true }) === null && hk('w', { typing: true }) === null && hk('ArrowDown', { typing: true }) === null
   && hk('-', { typing: true }) === null && hk(',', { typing: true }) === null, 'app: typing — Backspace, letters, arrows, punctuation are the field\'s');
{
  const bus = new KeyBus(); const got: string[] = [];
  const a = bus.add('down', (e) => { got.push('screen:' + e.key); });
  const b = bus.add('down', (e) => { got.push('panel:' + e.key); return e.key === 'Back'; });
  bus.dispatch('down', { key: 'Backspace' });
  ok(got.join() === 'panel:Back', `bus: newest first, and a consumed Back stops there (${got.join()})`);
  got.length = 0; bus.dispatch('down', { key: 'd' });
  ok(got.join() === 'panel:ArrowRight,screen:ArrowRight', `bus: an unconsumed key reaches every listener (${got.join()})`);
  got.length = 0; b.remove(); bus.dispatch('down', { key: 'Escape' });
  ok(got.join() === 'screen:Back', 'bus: a removed listener hears nothing');
  got.length = 0; bus.dispatch('down', { key: 'm' });
  ok(got.length === 0, 'bus: a key that is not ours is never dispatched');
  const ups: string[] = []; bus.add('up', (e) => { ups.push(e.key); return true; }); bus.add('up', (e) => { ups.push('2' + e.key); return true; });
  bus.dispatch('up', { key: '>' });
  ok(ups.join() === '2TuneUp,TuneUp', 'bus: releases are never consumed (a sweep must always hear its key let go)');
  a.remove();
  // Priority: a screen registered LATER (low priority) still hears a key after the panel over it.
  const bus2 = new KeyBus(); const got2: string[] = [];
  bus2.add('down', (e) => { got2.push('panel'); return e.key === 'Back'; });
  bus2.add('down', () => { got2.push('screen'); }, -1);
  bus2.dispatch('down', { key: 'Escape' });
  ok(got2.join() === 'panel', `bus: a low-priority screen listener added later still comes after the panel (${got2.join()})`);
}

if (fails) { console.log(`${fails} FAILED`); process.exit(1); }
console.log('all ok');
