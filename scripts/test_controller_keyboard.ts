// The controller keyboard's editing rules (src/utils/controllerKeyboard.ts) — brief, decided 2026-10-20.
import { readFileSync } from 'node:fs';
import { kbInit, kbReduce, kbDisplay, kbParseNumber, kbFormatNumber, PIN_MAX, type KbAction, type KbState } from '../src/utils/controllerKeyboard';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
const run = (s: KbState, ...a: KbAction[]) => a.reduce(kbReduce, s);
const type = (s: KbState, str: string) => [...str].reduce((x, c) => kbReduce(x, { t: 'ch', ch: c }), s);

console.log('test_controller_keyboard');
// Numeric: one point, digits per unit.
let f = kbInit('frequency', '', { unit: 'mhz' });
f = type(f, '99.8.0');
ok(f.value === '99.80', `one point only (${f.value})`);
f = type(kbInit('frequency', '', { unit: 'mhz' }), '1234567');
ok(f.value === '12345', `MHz: five whole digits at most (${f.value})`);
f = type(kbInit('frequency', '', { unit: 'khz' }), '198.1234');
ok(f.value === '198.123', `kHz: three decimals at most (${f.value})`);
f = type(kbInit('frequency', '', { unit: 'mhz' }), 'a,-x5');
ok(f.value === '5', 'letters and commas are not digits');
ok(kbInit('frequency', '99,8').value === '99.8', 'a comma handed in becomes our point — `.` internally, whatever the locale');
ok(kbParseNumber('99.8') === 99.8 && Number.isNaN(kbParseNumber('9,8')) && Number.isNaN(kbParseNumber('.')), 'our own parse, never locale-aware');
// kHz ↔ MHz.
f = run(kbInit('frequency', '198', { unit: 'khz' }), { t: 'unit' });
ok(f.unit === 'mhz' && f.value === '0.198', `198 kHz → 0.198 MHz (${f.value})`);
f = run(f, { t: 'unit' });
ok(f.unit === 'khz' && f.value === '198', `and back (${f.value})`);
f = run(kbInit('frequency', '99.8', { unit: 'mhz' }), { t: 'unit' });
ok(f.value === '99800', `99.8 MHz → 99800 kHz (${f.value})`);
ok(run(kbInit('frequency', '', { unit: 'mhz' }), { t: 'unit' }).unit === 'khz', 'empty: the unit still switches');
ok(kbFormatNumber(0.1 + 0.2, 6) === '0.3', 'no float noise in a conversion');
// PIN.
let p = type(kbInit('pin'), '12a3.4');
ok(p.value === '1234', 'PIN: digits only');
p = type(kbInit('pin'), '1'.repeat(30));
ok(p.value.length === PIN_MAX, 'PIN: a limit');
ok(run(kbInit('pin', '12'), { t: 'space' }).value === '12', 'PIN: no space');
// Caret.
let t = kbInit('text', 'helo');
t = run(t, { t: 'left' }); t = type(t, 'l');
ok(t.value === 'hello' && t.caret === 4, 'insert at the caret');
t = run(t, { t: 'left' }, { t: 'left' }, { t: 'bs' });
ok(t.value === 'hllo' && t.caret === 1, '⌫ deletes BEFORE the caret');
ok(run(kbInit('text', 'ab'), { t: 'right' }, { t: 'right' }).caret === 2 && run(kbInit('text', 'ab', { caret: 0 }), { t: 'left' }).caret === 0, 'the caret clamps at both ends');
ok(run(kbInit('text', ''), { t: 'bs' }).value === '', '⌫ on nothing: nothing');
ok(kbInit('text', 'abc', { caret: 1 }).caret === 1, 'a hand-over keeps the caret');
// Shift / caps / layers.
t = run(kbInit('text'), { t: 'shift' });
ok(t.layer === 'upper', '⇧: one capital');
t = type(t, 'A');
ok(t.layer === 'lower', 'after one letter, back to lower case');
t = run(kbInit('text'), { t: 'shift' }, { t: 'shift' });
ok(t.layer === 'caps', 'double ⇧: caps lock');
t = type(t, 'AB');
ok(t.layer === 'caps', 'caps lock stays');
ok(run(t, { t: 'shift' }).layer === 'lower', 'a third ⇧: off');
ok(run(kbInit('text'), { t: 'symbols' }).layer === 'symbols' && run(kbInit('text'), { t: 'symbols' }, { t: 'abc' }).layer === 'lower', '#+= and ABC');
ok(run(kbInit('text'), { t: 'accents' }, { t: 'shift' }).layer === 'accentsUp', 'accents take ⇧ too');
ok(run(kbInit('url'), { t: 'shift' }).layer === 'lower', 'url: no capitals');
ok(run(kbInit('url', 'a'), { t: 'space' }).value === 'a' && type(kbInit('url'), 'a b').value === 'ab', 'url: no spaces');
ok(run(kbInit('search', ''), { t: 'space' }).value === ' ', 'search: a space');
// Password.
let pw = type(kbInit('password'), 'hunter2');
ok(kbDisplay(pw) === '•••••••' && pw.value === 'hunter2', 'password: dots on screen, the value intact');
ok(kbDisplay(run(pw, { t: 'reveal' })) === 'hunter2', 'SHOW reveals it');
ok(run(kbInit('text', 'x'), { t: 'reveal' }).reveal === false, 'reveal does nothing outside password mode');
// Clear.
ok(run(kbInit('text', 'abc'), { t: 'clear' }).value === '' , 'Clear');
// ★★ Never logged: no runtime import, no console, no crumb in the reducer or the layouts.
for (const f2 of ['src/utils/controllerKeyboard.ts', 'src/utils/kbLayouts.ts', 'src/utils/kbHandover.ts', 'src/components/ControllerKeyboard.tsx']) {
  const src = readFileSync(new URL('../' + f2, import.meta.url), 'utf8').split('\n').filter((l) => !/^\s*(\*|\/\/|\/\*)/.test(l)).join('\n');
  ok(!/console\.|crumb\(|diagnostics|logEvent|breadcrumb/.test(src), `${f2}: logs nothing`);
}
const red = readFileSync(new URL('../src/utils/controllerKeyboard.ts', import.meta.url), 'utf8');
ok(!/^import (?!type )/m.test(red), 'controllerKeyboard.ts: no runtime import at all');
// The overlay's own promises, read from its source.
const ck = readFileSync(new URL('../src/components/ControllerKeyboard.tsx', import.meta.url), 'utf8');
ok(/priority: 1000/.test(ck) && /return true;\s*\/\/ zoom: nothing here/.test(ck), 'the overlay hears keys above everything and consumes every one');
ok(/setPadKeyboardOpen\(true\)/.test(ck) && /holdIdleClose\(\)/.test(ck), 'START taps submit while open; the panel beneath cannot time out mid-typing');
ok(/password \? '#9a9a9a'/.test(ck) && /activeOpacity=\{password \? 1/.test(ck), 'password mode: a neutral ring and no press animation');
ok(/leaveTextField/.test(ck) && /Keyboard\.dismiss\(\)/.test(ck), 'opening lets go of any field cursor first (else the keyboard is deaf)');
ok(/onTouchStart=\{\(\) => \{ inputSource\.set\('touch'\); handover\(\); \}\}/.test(ck), 'any touch hands over to the system keyboard');
ok(/if \(!e\.pad\) \{ handover\(\); return true; \}/.test(ck), 'a physical key hands over to typing in place');
process.exit(fails ? 1 : 0);
