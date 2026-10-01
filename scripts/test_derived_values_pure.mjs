/**
 * No useDerivedValue may WRITE a shared value (src/**).
 *
 * ★★★ WHY (B8 power audit): DrumWheel's four tick builders each did `f.value ^= 1` on a shared value
 * they also read. A derived value subscribes to every shared value it reads, so its own write marked
 * it dirty again: it re-ran and asked Skia to redraw EVERY FRAME, forever — ~80 redraws a second per
 * drum, in the background and with the screen off (it was the whole of the app's background CPU on
 * the emulator). A derived value must be a pure function of what it reads; anything that needs to
 * remember state between runs belongs in useAnimatedReaction's body, which is not subscribed.
 *
 * Static check: find each `useDerivedValue(` call, take its callback body (balanced braces), and fail
 * on any `<ident>.value` assignment (=, op=, ++/--) or `.set(` inside it.
 *
 * Run: node scripts/test_derived_values_pure.mjs   (run-tests.sh does)
 */
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = new URL('../src/', import.meta.url).pathname;
const files = [];
(function walk(d) {
  for (const f of readdirSync(d)) {
    const p = join(d, f);
    if (statSync(p).isDirectory()) walk(p);
    else if (/\.(ts|tsx)$/.test(f)) files.push(p);
  }
})(ROOT);

/** The text of the callback passed to the call that starts at `open` (index of its '('). */
function callbackBody(src, open) {
  const brace = src.indexOf('{', open);
  const arrowEnd = src.indexOf('=>', open);
  if (arrowEnd < 0) return '';
  // expression-bodied arrow: `() => expr` — take up to the matching ')' of the call
  if (brace < 0 || brace > src.indexOf('\n', arrowEnd) + 200 && src.slice(arrowEnd + 2).trimStart()[0] !== '{') {
    let depth = 0;
    for (let i = open; i < src.length; i++) {
      if (src[i] === '(') depth++;
      else if (src[i] === ')') { depth--; if (depth === 0) return src.slice(arrowEnd + 2, i); }
    }
    return '';
  }
  let depth = 0;
  for (let i = brace; i < src.length; i++) {
    if (src[i] === '{') depth++;
    else if (src[i] === '}') { depth--; if (depth === 0) return src.slice(brace, i + 1); }
  }
  return '';
}

const WRITE = /\b[\w$]+\.value\s*(?:[-+*/%&|^]|<<|>>>?|\*\*|\?\?|&&|\|\|)?=(?!=)|\b[\w$]+\.value\s*(?:\+\+|--)|(?:\+\+|--)\s*[\w$]+\.value\b|\b[\w$]+\.set\(/;

let checked = 0, fails = 0;
for (const file of files) {
  const src = readFileSync(file, 'utf8');
  let at = 0;
  for (;;) {
    const i = src.indexOf('useDerivedValue(', at);
    if (i < 0) break;
    at = i + 1;
    // skip the import line itself
    const lineStart = src.lastIndexOf('\n', i) + 1;
    if (/^\s*import\b/.test(src.slice(lineStart, i)) || /[{,]\s*$/.test(src.slice(lineStart, i)) && /import/.test(src.slice(Math.max(0, i - 200), i))) {
      if (src.slice(i + 'useDerivedValue('.length).trimStart()[0] !== '(') continue;
    }
    const body = callbackBody(src, i + 'useDerivedValue'.length);
    if (!body) continue;
    checked++;
    const m = body.match(WRITE);
    if (m) {
      fails++;
      const line = src.slice(0, i).split('\n').length;
      console.log(`  FAIL ${file.replace(ROOT, 'src/')}:${line} — useDerivedValue writes a shared value: \`${m[0]}\``);
    }
  }
}

// The check must be able to see the bug it exists for.
const BAD = `const p = useDerivedValue(() => { 'worklet';\n  f.value ^= 1;\n  return g(f.value);\n}, []);`;
const sawBad = WRITE.test(callbackBody(BAD, BAD.indexOf('(')));
if (!sawBad) { fails++; console.log('  FAIL the check cannot see `f.value ^= 1` inside a derived value'); }
const GOOD = `const p = useDerivedValue(() => { 'worklet';\n  return a.value === b.value ? x.value : 0;\n}, []);`;
if (WRITE.test(callbackBody(GOOD, GOOD.indexOf('(')))) { fails++; console.log('  FAIL the check flags a pure comparison'); }

if (checked < 5) { fails++; console.log(`  FAIL only ${checked} derived values found — the scan is broken`); }
console.log(`${fails ? 'FAIL' : 'ok'}  derived values pure: ${checked} checked, ${fails} failed`);
process.exit(fails ? 1 : 0);
