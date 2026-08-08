// ★★★ THE SETUP PAGE IS A C++ RAW STRING, so nothing checks its JavaScript. A stray brace or a
//     name that does not exist compiles perfectly, ships, and then the page does NOTHING — no
//     error the owner can see, no line in any log, just a form that will not save. That is the
//     worst failure mode this file has, because the page is what a new owner meets first.
//
// ★★ Parsing is not linting. This does not judge the code; it answers one question — would the
//    browser refuse to run it — which is exactly the failure that reaches a user silently.
//    Deliberately narrow, so it can never become a gate people learn to skip.
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const m = src.match(/kVibeSetupPage = R"HTML\(([\s\S]*?)\)HTML"/);
if (!m) {
  console.error('✗ could not find the setup page string — has the raw-string delimiter changed?');
  process.exit(2);
}
const html = m[1];

// 1. The JavaScript must parse.
const js = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((x) => x[1]).join('\n');
if (!js.trim()) { console.error('✗ the setup page has no script at all'); process.exit(1); }
try {
  new Function(js);
} catch (e) {
  console.error(`✗ setup page JavaScript does not parse: ${e.message}`);
  process.exit(1);
}

// 2. Tags must balance. An unclosed <div> silently swallows everything after it — the cards are
//    still "there" and the owner simply cannot see them.
let bad = 0;
for (const tag of ['div', 'label', 'select', 'button']) {
  const open = (html.match(new RegExp(`<${tag}\\b`, 'g')) || []).length;
  const close = (html.match(new RegExp(`</${tag}>`, 'g')) || []).length;
  // <select> and <button> appear inside JS template strings and comments too, so only flag a
  // tag with MORE closes than opens, which is unambiguous, plus div/label which are structural.
  const structural = tag === 'div' || tag === 'label';
  if ((structural && open !== close) || close > open) {
    console.error(`✗ <${tag}>: ${open} opened, ${close} closed`);
    bad++;
  }
}

// 3. Every element the script reaches for by id must exist in the markup. This is the mistake
//    that produced a blank receiver once already: code moved, the id did not, and the failure was
//    a null dereference at run time that killed everything after it.
// ★ Comment lines are dropped before the scan. This was a character-by-character stripper that
//   tracked strings, and it was WRONG in a way worth recording: JavaScript regex literals contain
//   quotes (/[&<>"']/g is in this very page), so a lexer that does not also understand regexes
//   reads one as an unterminated string and treats every comment after it as code. Lexing JS
//   properly to decide whether to READ A COMMENT is far more machinery than the job deserves —
//   a line that begins with // or * is a comment, and that covers every real case here.
const codeLines = js.split('\n').filter((l) => !/^\s*(\/\/|\*|\/\*)/.test(l)).join('\n');

const ids = new Set([...html.matchAll(/\bid="([^"]+)"/g)].map((x) => x[1]));
const missing = new Set();
for (const [, name] of codeLines.matchAll(/\$\("([A-Za-z0-9_]+)"\)/g))
  if (!ids.has(name)) missing.add(name);
if (missing.size) {
  console.error(`✗ the script asks for ids that are not in the page: ${[...missing].join(', ')}`);
  bad++;
}

if (bad) process.exit(1);
console.log(`ok   setup page: JS parses, tags balance, ${ids.size} ids all present`);
