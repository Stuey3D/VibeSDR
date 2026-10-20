// The controller keyboard on the WEB page (web/client/src/padKeyboard.ts + gamepad.ts) — the promises that can be read
// from the source, and the shared rules it leans on. The behaviour itself is driven end to end, with a simulated pad in a
// real browser, by scripts/test-web-pad-keyboard.mjs. Decided by Stuart 2026-10-20 (docs/v12/BRIEF-controller-keyboard.md).
import { readFileSync } from 'node:fs';
import { kbInit, kbReduce, kbDisplay } from '../src/utils/controllerKeyboard';
import { kbKeys } from '../src/utils/kbLayouts';
import { editorFor, handover } from '../src/utils/kbHandover';
import { InputSourceTracker } from '../src/utils/inputSource';
import { PAD_GUIDES } from '../src/utils/padGuide';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
const src = (p: string) => readFileSync(new URL('../' + p, import.meta.url), 'utf8');
const code = (p: string) => src(p).split('\n').filter((l) => !/^\s*(\*|\/\/|\/\*)/.test(l)).join('\n');

console.log('test_web_pad_keyboard');
const kb = code('web/client/src/padKeyboard.ts');
const gp = code('web/client/src/gamepad.ts');

// ── SHARED, NOT FORKED: the app's own files, imported ──
for (const m of ['kbLayouts', 'controllerKeyboard', 'kbHandover', 'inputSource'])
  ok(new RegExp(`from '\\.\\./\\.\\./\\.\\./src/utils/${m}'`).test(kb), `padKeyboard.ts imports src/utils/${m}.ts (no copy of its own)`);
ok(!/function kbReduce|function kbKeys|const LETTERS/.test(kb), '…and re-implements none of it');

// ── ★★ Password mode: NEVER logged ──
ok(!/console\.|crumb\(|diagnostics\(|sendBeacon|localStorage/.test(kb), 'padKeyboard.ts logs and stores nothing');
ok(/\.secure \.kbKey\.kbOn \{ outline-color: #9a9a9a/.test(src('web/client/index.html')), 'password mode: a neutral grey ring');
ok(/if \(!s \|\| s\.secure \|\| !root\) return;\s+\/\/ ★ password mode: no press animation/.test(src('web/client/src/padKeyboard.ts')), 'password mode: no press flash');
const pw = ['h', 'u', 'n'].reduce((x, c) => kbReduce(x, { t: 'ch', ch: c }), kbInit('password'));
ok(kbDisplay(pw) === '•••' && pw.value === 'hun', 'password: the shared display draws dots');

// ── ★★ Never a focused <input> while it shows ──
ok(/if \(document\.activeElement === el\) el\.blur\(\);/.test(kb), 'opening lets go of the field first (no phone keyboard underneath)');
ok(!/createElement\('input'\)|createElement\('textarea'\)|<input|contenteditable/.test(kb), 'the keyboard draws its value as text — it has no input of its own');
ok(/if \(isTextField\(el\)\) \{ openPadKeyboard\(el\); suppressHeld\(\); return; \}/.test(gp) && !/el\.focus\(\); return;/.test(gp),
   'pad enter on a text box opens the controller keyboard, never el.focus()');

// ── ★★ Only REAL pad use counts (Stuart, 2026-10-20) ──
ok(/if \(rising\.some\(Boolean\) \|\| sticks\.some\(\(v, i\) => v && !sWas\[i\]\)\) notePadInput\(\);/.test(gp),
   'the pad becomes the input only on a button press or a stick crossing STICK_USE');
ok(/const STICK_USE = 0\.5;/.test(gp) && (gp.match(/notePadInput\(\)/g) ?? []).length === 1, '…in exactly one place, with a dead zone of 0.5');
ok(!/gamepadconnected[^\n]*inputSource|gamepadconnected[^\n]*notePadInput/.test(gp), '…never on connecting');

// ── The hand-over, as the shared rule says ──
ok(editorFor('pad') === 'controller' && editorFor('touch') === 'system' && editorFor('keyboard') === 'inplace', 'shared rule: who opens what');
ok(handover('controller', 'touch') === 'system' && handover('controller', 'keyboard') === 'inplace' && handover('system', 'pad') === 'controller', 'shared rule: the live hand-over');
ok(/handOver\('touch'\)/.test(kb) && /handOver\('keyboard'\)/.test(kb), 'padKeyboard.ts hands over on a touch / click and on a physical key');
ok(/carry\(\{ value: s\.st\.value, caret: s\.st\.caret, secure: s\.secure \}\)/.test(kb), '…carrying the value, the caret and password mode');
ok(/if \(!e\.isTrusted \|\| MODIFIERS\.includes\(e\.key\)\) return;/.test(kb), 'its own Enter (the commit) and bare modifiers are not a hand-over');
const t = new InputSourceTracker();
ok(t.get() === 'touch', 'a fresh page is a hand on glass, not a pad');

// ── Buttons per profile; START; Back ──
const kp = gp.slice(gp.indexOf('function kbPoll'));
ok(/B_R1\), padKbPress/.test(kp) && /B_L1\), padKbBack/.test(kp) && /B_WEST\), \(\) => padKbEdit\('left'\)/.test(kp) && /B_EAST\), \(\) => padKbEdit\('right'\)/.test(kp),
   'buttons profile: R presses, L backs, the tune buttons move the caret');
ok(/B_SOUTH\), padKbPress/.test(kp) && /B_EAST\), padKbBack/.test(kp) && /B_WEST\), \(\) => padKbEdit\('bs'\)/.test(kp) && /B_NORTH\), \(\) => padKbEdit\('space'\)/.test(kp)
   && /numeric \? 'unit' : 'shift'/.test(kp) && /numeric \? 'unit' : 'symbols'/.test(kp) && /tuneJog\.feed[\s\S]{0,120}padKbEdit\(t > 0 \? 'right' : 'left'\)/.test(kp),
   'sticks profile: ✕ press, ○ back, □ ⌫, △ space, L1/R1 shift/symbols (kHz/MHz on the number pad), left stick = caret');
ok(/SWITCH_HOLD_MS\) \{ padKbSubmit\(\); return; \}/.test(kp) && !/toggleProfile/.test(kp.slice(0, kp.indexOf('export function initGamepad'))),
   'START: a tap submits; a hold does not switch the controls while it is open');
ok(/if \(s\.st\.layer !== 'lower'\) \{ apply\(\{ t: 'abc' \}\); return; \}/.test(kb), 'back peels one layer, then cancels');
ok(/if \(isBack\) \{ el\.blur\(\); highlight\(el\); return true; \}/.test(gp), "pad back lets go of a text box (the audit's trap)");

// ── Fields ──
const main = code('web/client/src/main.ts');
ok(/kbField\(\$\('freqInput'\)/.test(main) && /kbField\(freq, \{/.test(code('web/client/src/chat.ts')), 'the frequency box and the chat share frequency carry their units');
ok(/id="host" data-kb="url"/.test(src('web/client/index.html')), 'the server address box types as an address');
const f = kbKeys('frequency', 'lower', { unitLabel: 'kHz' });
ok(f.some((k) => k.id === 'unit') && !kbKeys('frequency', 'lower', { unitLocked: true }).some((k) => k.id === 'unit'), 'a field with no unit switch gets no dead unit key');

// ── The copy that says how (AGENTS.md) ──
ok(/\$\('pgKb'\)!\.innerHTML = \[\.\.\.g\.keyboard/.test(code('web/client/src/padGuide.ts')) && PAD_GUIDES.sticks.keyboard.length > 2 && PAD_GUIDES.buttons.keyboard.length > 2,
   'the web controller guide shows the typing lines (the app\'s, from src/utils/padGuide.ts)');
ok(/Enter on a text box opens an on-screen\s+keyboard for the controller/.test(src('web/client/index.html')), 'the menu says how to type with a controller');
ok(/#mFreq, #padKb \.kbDigit \{/.test(src('web/client/index.html')), 'the number pad digits share the frequency readout\'s rule');

process.exit(fails ? 1 : 0);
