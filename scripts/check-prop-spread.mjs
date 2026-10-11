#!/usr/bin/env node
// ★★★ THREE HAND-WRITTEN COPIES OF ONE PROP LIST, AND NOTHING CHECKS THEM (2026-09-20).
//
// ControlsBar receives props, builds a `shared` object, and spreads it into PortraitBar and LandscapeBar.
// Each of those three keeps its own hand-written list of names, and the parameter lists are typed `: any`, so
// a name missing from any one of them fails in a different way:
//
//   • missing from a BAR's destructure, used in its body  → ReferenceError, the app bounces to the server list
//   • missing from `shared`                               → the prop is undefined: a DEAD FEATURE, in silence
//   • missing from ControlsBar's own destructure          → ReferenceError while building `shared`
//
// All three happened in one afternoon with the shared-tuner banner: it threw in landscape, it was dead in
// portrait, and the first fix for that threw while assembling the object. Hence this check, which reads both
// directions at every layer.
//
//   node scripts/check-prop-spread.mjs
import { readFileSync } from 'node:fs';

const FILE = 'src/components/ControlsBar.tsx';
const strip = (t) => t.replace(/\/\*[\s\S]*?\*\//g, ' ').replace(/\/\/[^\n]*/g, ' ');
/** Names in a destructuring list. ★ The closing brace is INCLUDED by the caller: the last name has nothing
 *  after it otherwise, and a check that silently ignores the last entry is worse than none. */
const namesIn = (txt) => new Set([...strip(txt).matchAll(/([a-zA-Z_]\w*)(?=\s*[,=}])/g)].map((m) => m[1]));
const usedIn = (body, n) => new RegExp('[^\\w."\']' + n + '\\b').test(strip(body));

const src = readFileSync(FILE, 'utf8');
let bad = 0;
const fail = (msg) => { console.error('  ✗ ' + msg); bad = 1; };

// ── the object that carries props from ControlsBar to its bars ───────────────────────────────
const spreadTxt = src.match(/const shared = \{([\s\S]*?)\n  \};/)[1];
/* Each entry is either shorthand (`snrText,` — the name is both key and value) or renamed
 * (`bus: meterBus,` — the KEY is what the bar destructures, the VALUE is what must be in scope here). */
/* ★★ SPLIT THE OBJECT INTO ENTRIES AT DEPTH ZERO, rather than pattern-matching names. Values are arbitrary
 *  expressions (`modeLabel: dabOn ? 'DAB' : modeDisplay(mode) + ...`), and three regex attempts at this each
 *  mis-read those and reported a key as missing when it was right there — a checker that cries wolf is one
 *  nobody runs. Depth counting is dull and correct. */
function entriesOf(objText) {
  const out = [];
  let depth = 0, start = 0;
  const txt = strip(objText);
  for (let i = 0; i < txt.length; i++) {
    const c = txt[i];
    if ('([{'.includes(c)) depth++;
    else if (')]}'.includes(c)) depth--;
    else if (c === ',' && depth === 0) { out.push(txt.slice(start, i)); start = i + 1; }
  }
  out.push(txt.slice(start));
  return out.map((e) => e.trim()).filter(Boolean).map((e) => {
    const colon = e.indexOf(':');
    if (colon < 0) return { key: e.trim(), value: e.trim() };        // shorthand
    const key = e.slice(0, colon).trim();
    const value = e.slice(colon + 1).trim();
    return { key, value: /^[a-zA-Z_]\w*$/.test(value) ? value : null };   // null = an expression, tsc's job
  }).filter((e) => /^[a-zA-Z_]\w*$/.test(e.key));
}
const entries = entriesOf(spreadTxt);
const spreadKeys = entries.map((e) => e.key);

// ── 1. everything `shared` names must be in ControlsBar's scope (a prop, or a local it defines) ──
const cbStart = src.indexOf('function ControlsBar({');
const cbEnd = src.indexOf('}: ControlsBarProps)', cbStart);
const cbProps = namesIn(src.slice(cbStart, cbEnd + 1));
const cbBody = src.slice(cbEnd, src.indexOf('const shared = {'));
const cbLocals = new Set([...strip(cbBody).matchAll(/(?:const|let|function)\s+([a-zA-Z_]\w*)/g)].map((m) => m[1]));
for (const { key, value } of entries) {
  if (value === null) continue;            // an expression — tsc checks those
  if (!cbProps.has(value) && !cbLocals.has(value))
    fail(`shared passes \`${key}\` from \`${value}\`, which ControlsBar neither takes as a prop nor defines`);
}

// ── 2. each bar: used-but-not-declared (throws), and declared-but-not-passed (dead) ──────────
for (const bar of ['PortraitBar', 'LandscapeBar']) {
  const i = src.indexOf('function ' + bar + '(');
  if (i < 0) { fail(`no component ${bar} — has it been renamed?`); continue; }
  const he = src.indexOf('}: any)', i);
  const declared = namesIn(src.slice(i, he + 1));
  const next = src.indexOf('\nfunction ', he);
  const body = src.slice(he, next > 0 ? next : src.length);
  for (const k of spreadKeys) if (!declared.has(k) && usedIn(body, k)) fail(`${bar} uses \`${k}\` without destructuring it — this THROWS when it renders`);
  for (const d of declared) {
    if (!/^[a-z]/.test(d) || /^(any|false|true|null|undefined)$/.test(d)) continue;
    if (!spreadKeys.includes(d) && usedIn(body, d)) fail(`${bar} uses \`${d}\`, which \`shared\` never passes — a dead feature, silently`);
  }
}

console.log(bad ? `${FILE}: prop lists disagree — see above` : `${FILE}: prop lists agree across ControlsBar, PortraitBar and LandscapeBar`);
process.exit(bad);
