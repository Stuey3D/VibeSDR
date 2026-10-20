// Follow the live input source while editing (src/utils/kbHandover.ts) — Stuart, decided 2026-10-20.
import { editorFor, handover, carry } from '../src/utils/kbHandover';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };

console.log('test_kb_handover');
// Opening: by the source at the moment Enter or a tap opens the field.
ok(editorFor('touch') === 'system', 'opened by touch: the system keyboard, always');
ok(editorFor('keyboard') === 'inplace', 'opened by a physical keyboard: type in place, no on-screen keyboard');
ok(editorFor('pad') === 'controller', 'opened by a pad or remote: ControllerKeyboard');
// The table.
ok(handover('controller', 'touch') === 'system', 'controller + touch → the system keyboard on the same field');
ok(handover('controller', 'keyboard') === 'inplace', 'controller + a physical key → type in place');
ok(handover('controller', 'pad') === null, 'controller + pad: stays');
ok(handover('system', 'keyboard') === null, 'system + a physical key: stays — the OS drops its keyboard; we do not fight it');
ok(handover('system', 'pad') === 'controller', 'system + pad → ControllerKeyboard');
ok(handover('system', 'touch') === null, 'system + touch: stays');
ok(handover('inplace', 'touch') === null, 'in place + touch: stays (the OS shows its own keyboard)');
ok(handover('inplace', 'pad') === 'controller', 'in place + pad → ControllerKeyboard');
// The value, caret and password mode never reset.
const c = carry({ value: 'hunter2', caret: 3, secure: true });
ok(c.value === 'hunter2' && c.caret === 3 && c.secure === true, 'value, caret and password mode carried unchanged');
ok(carry({ value: 'ab', caret: 9, secure: false }).caret === 2, 'a caret past the end is clamped, the value kept');
// A full round trip: pad opens, touch takes over, pad takes back — the text survives every step.
let ed = editorFor('pad'); let edit = { value: '', caret: 0, secure: true };
edit = { value: 'ab', caret: 2, secure: true };                       // typed on ours
ed = handover(ed, 'touch') ?? ed; edit = carry(edit);
ok(ed === 'system' && edit.value === 'ab', 'pad → touch: "ab" is in the field');
edit = { value: 'abc', caret: 3, secure: edit.secure };               // typed on the system keyboard
ed = handover(ed, 'pad') ?? ed; edit = carry(edit);
ok(ed === 'controller' && edit.value === 'abc' && edit.caret === 3 && edit.secure, 'touch → pad: "abc", caret 3, still a password');
process.exit(fails ? 1 : 0);
