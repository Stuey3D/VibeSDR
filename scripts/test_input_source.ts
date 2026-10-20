// The last input source (src/utils/inputSource.ts) — controller keyboard brief, decided 2026-10-20.
import { readFileSync } from 'node:fs';
import { InputSourceTracker } from '../src/utils/inputSource';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
const src = (p: string) => readFileSync(new URL('../' + p, import.meta.url), 'utf8');

console.log('test_input_source');
const t = new InputSourceTracker();
const heard: string[] = [];
const un = t.subscribe((s) => heard.push(s));
ok(t.get() === 'touch', 'starts as touch (a fresh app is a hand on glass)');
t.set('pad'); t.set('pad'); t.set('keyboard'); t.set('touch');
ok(heard.join(',') === 'pad,keyboard,touch', 'subscribers hear each CHANGE once (' + heard.join(',') + ')');
un(); t.set('pad');
ok(heard.length === 3, 'unsubscribed: silent');

// Wired where the brief says, and nowhere guessing:
const hk = src('src/utils/hardwareKeys.ts');
ok(/VibeKeyDown[^\n]*inputSource\.set\('keyboard'\)/.test(hk), 'a physical key sets keyboard before any listener hears it');
ok(/function runPad[\s\S]{0,400}inputSource\.set\('pad'\)/.test(hk), 'a pad press sets pad — even while a field has the cursor');
const app = src('App.tsx');
ok(/onStartShouldSetResponderCapture=\{\(\) => \{ noteTouchInteraction\(\); return false; \}\}/.test(app), 'the root touch capture notes a touch and never takes it');
ok(/onTouchStart=\{noteTouchInteraction\}/.test(app), 'and the raw touch, for native gesture views');
const pn = src('src/components/PanelNav.tsx');
ok(/inputSource\.subscribe/.test(pn) && /export function useInputSource/.test(pn), 'PanelNav derives keyboardMode from the tracker and exposes useInputSource');
process.exit(fails ? 1 : 0);
