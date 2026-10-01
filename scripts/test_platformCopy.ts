/**
 * platformCopy (src/services/platformCopy.ts) over EVERY string the About screen shows: on iOS none may
 * name Android (App Review 2.3.10), each rewrite still matches its source text (a reworded note would
 * silently stop being rewritten and fall through to being dropped), and Android sees the text unchanged.
 *
 * Run: node --no-warnings scripts/test_platformCopy.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import { platformCopy, IOS_REWRITES } from '../src/services/platformCopy.ts';

let fails = 0, passes = 0;
const ok = (what: string, c: boolean) => { if (c) passes++; else { fails++; console.log('  FAIL ' + what); } };

const src = readFileSync(new URL('../src/components/AboutOverlay.tsx', import.meta.url), 'utf8');
// Every single-quoted string literal in the file, with JS escapes decoded (— etc.).
const literals: string[] = [];
for (const m of src.matchAll(/'((?:[^'\\\n]|\\.)*)'/g)) {
  try { literals.push(JSON.parse('"' + m[1].replace(/"/g, '\\"').replace(/\\'/g, "'") + '"')); } catch { /* not text */ }
}
const withAndroid = literals.filter(s => /Android/.test(s));
ok(`the About screen still has Android mentions to test (${withAndroid.length})`, withAndroid.length >= 10);

for (const s of withAndroid) {
  const ios = platformCopy(s, 'ios');
  const tag = s.slice(0, 60).replace(/\s+/g, ' ');
  ok(`iOS never names Android: "${tag}…"`, !/Android/.test(ios));
  ok(`Android unchanged: "${tag}…"`, platformCopy(s, 'android') === s);
  ok(`iOS text has no dangling dash or double space: "${tag}…"`, !/^\s*—|—\s*$|\s{2,}/u.test(ios));
}
// Each rewrite must still find its source somewhere in the file — or it has rotted.
for (const [from] of IOS_REWRITES) ok(`rewrite still matches: "${from.slice(0, 50)}…"`, literals.some(s => s.includes(from)));

// The VibeServer pitch keeps its point on iOS (it is rewritten, not dropped).
const pitch = literals.find(s => s.startsWith('VibeServer — share your radio'))!;
ok('the VibeServer pitch survives on iOS', /Raspberry Pi/.test(platformCopy(pitch, 'ios')) && /web client/.test(platformCopy(pitch, 'ios')));
// A sentence purely about Android goes; its neighbours stay.
ok('drops only the Android sentence', platformCopy('One fix. Android got another. Third fix.', 'ios') === 'One fix. Third fix.');
ok('text without Android is untouched on iOS', platformCopy('No mention here.', 'ios') === 'No mention here.');

console.log(`${fails ? 'FAIL' : 'ok'}  platform copy: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
