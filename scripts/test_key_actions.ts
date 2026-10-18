// The keyboard's one scheme (src/utils/keyActions.ts) — Stuart, 2026-10-18.
import { keyAction, keyDir, keyRepeats } from '../src/utils/keyActions';

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

if (fails) { console.log(`${fails} FAILED`); process.exit(1); }
console.log('all ok');
