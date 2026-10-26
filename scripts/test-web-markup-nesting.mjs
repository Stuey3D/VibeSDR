#!/usr/bin/env node
// test-web-markup-nesting.mjs — the served page's panels are where the page expects them (2026-10-26).
//
// ★★★ WHY: two merges each dropped one `</div>` (the PSK and the APRS settings sub-panels). HTML forgives that silently:
//     everything after the first one — DIGITAL SPOTS, the MAP key and the WHOLE MAIN MENU — became a child of a hidden
//     sub-panel. On Stuart's Pi 2 MENU did nothing and the decoders sheet had no spots or map (Safari, 2026-10-26). A
//     headless check that only asked "does #menu have class open" passed throughout, because the class WAS set.
//     So this reads the markup itself: the tag stack must balance, and each panel must have the parent it is meant to.
import fs from 'node:fs';
import path from 'node:path';

const ROOT = path.resolve(new URL('..', import.meta.url).pathname);
const html = fs.readFileSync(path.join(ROOT, 'web/client/index.html'), 'utf8');
const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'param', 'source', 'track', 'wbr']);
let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? '\x1b[32mok\x1b[0m  ' : '\x1b[31mFAIL\x1b[0m'} ${what}`); if (!c) fails++; };

// ★ A tag scanner, not a parser: comments, scripts and styles are cut out first (they may hold "<div" in text).
const src = html.replace(/<!--[\s\S]*?-->/g, '').replace(/<(script|style)\b[\s\S]*?<\/\1>/gi, '');
const stack = [];            // [{ tag, id }]
const parent = new Map();    // id -> nearest ancestor id ('' at the top)
let mismatches = 0;
for (const m of src.matchAll(/<(\/?)([a-zA-Z][a-zA-Z0-9-]*)\b([^>]*)>/g)) {
  const [, close, rawTag, attrs] = m;
  const tag = rawTag.toLowerCase();
  if (close) {
    if (VOID.has(tag)) continue;
    const top = stack.pop();
    if (!top || top.tag !== tag) mismatches++;
    continue;
  }
  const id = (/\bid\s*=\s*"([^"]*)"/.exec(attrs) || [])[1] || '';
  if (id) parent.set(id, [...stack].reverse().find((e) => e.id)?.id || '');
  if (!VOID.has(tag) && !/\/\s*$/.test(attrs)) stack.push({ tag, id });
}
ok(mismatches === 0, `every closing tag closes the element it should (${mismatches} mismatched)`);
ok(stack.length === 0, `nothing is left open at the end of the page (${stack.length} open: ${stack.slice(-3).map((e) => e.tag + (e.id ? '#' + e.id : '')).join(' ')})`);

// ★ The panels the PANELS list toggles are top-level: none may live inside another.
for (const id of ['menu', 'audioPanel', 'decodersPanel', 'recordingsPanel']) {
  if (parent.has(id)) ok(!['menu', 'audioPanel', 'decodersPanel', 'recordingsPanel'].includes(parent.get(id)), `#${id} is not inside another panel (parent: ${parent.get(id) || 'top'})`);
}
// ★ Every decoder's settings, and the spots section, sit directly in the decoders panel.
for (const [id, p] of parent) {
  if (/Settings$/.test(id) && parent.get(id) && html.includes(`class="subPanel" id="${id}"`)) ok(p === 'decodersPanel', `#${id} is in the decoders panel (parent: ${p || 'top'})`);
}
ok(parent.get('spotsSection') === 'decodersPanel', `DIGITAL SPOTS / MAP sit in the decoders panel (parent: ${parent.get('spotsSection') || 'top'})`);

console.log(fails ? `\n${fails} failed` : '\nall passed');
process.exit(fails ? 1 : 0);
