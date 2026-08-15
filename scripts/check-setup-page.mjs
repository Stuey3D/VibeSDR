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

// 2. Tags must NEST, not merely balance. This started as a count of opens against closes and it
//    let a real fault straight through: a card lost its closing tag while the totals still
//    matched, so #serverPane swallowed #radioPane and every radio setting appeared under the
//    server tab. Equal counts say nothing about STRUCTURE — a stack does.
let bad = 0;
{
  const stack = [];
  const tagRe = /<(\/?)(div|label|select)\b[^>]*?(\/?)>/g;
  // ★ Comments are skipped: the file explains itself heavily, and a <div> mentioned in prose is
  //   not markup. Same reasoning as the id scan below.
  // ★★ SCRIPT IS NOT MARKUP. The page builds rows and chips as HTML strings in JavaScript, so a
  //    scan of the whole file sees <div> that never belonged to the document at all. Stripping the
  //    scripts first is what makes a nesting check possible; without it the counts are noise —
  //    which is precisely why the old count-based check could not have caught this.
  const markup = html.replace(/<script>[\s\S]*?<\/script>/g, '')
                     .replace(/<!--[\s\S]*?-->/g, '');
  let m;
  while ((m = tagRe.exec(markup))) {
    const [, closing, tag, selfClose] = m;
    if (selfClose) continue;
    if (!closing) { stack.push({ tag, at: m.index }); continue; }
    const top = stack.pop();
    if (!top) { console.error(`✗ </${tag}> with nothing open`); bad++; break; }
    if (top.tag !== tag) {
      const near = markup.slice(Math.max(0, top.at), top.at + 70).replace(/\s+/g, ' ');
      console.error(`✗ <${top.tag}> is closed by </${tag}> — near: ${near}`);
      bad++; break;
    }
  }
  if (!bad && stack.length) {
    // ★ Report the INNERMOST unclosed tag, not the outermost. A missing close cascades, so the
    //   first thing left on the stack is usually <div class="wrap"> — the outermost casualty, and
    //   the least useful place to start looking. The last one is where the tag actually went.
    const f = stack[stack.length - 1];
    console.error(`✗ ${stack.length} unclosed tag(s); the innermost <${f.tag}> is near: `
                  + markup.slice(f.at, f.at + 100).replace(/\s+/g, ' '));
    bad++;
  }
}

// ★★ AND THE PANES MUST HOLD THE CARDS THEY ARE NAMED FOR. Valid structure is not the same as
//    RIGHT structure: the fault above produced perfectly well-formed HTML in which the radio pane
//    was simply empty and every radio setting sat under the server tab.
{
  const paneCards = (id) => {
    const i = html.indexOf(`id="${id}"`);
    if (i < 0) return null;
    const start = html.lastIndexOf('<div', i);
    let d = 0;
    const re = /<div\b|<\/div>/g;
    re.lastIndex = start;
    let m;
    while ((m = re.exec(html))) {
      d += m[0] === '</div>' ? -1 : 1;
      if (d === 0) return (html.slice(start, m.index).match(/<h2>/g) || []).length;
    }
    return null;
  };
  const sp = paneCards('serverPane'), rp = paneCards('radioPane');
  if (sp === null || rp === null) { console.error('✗ a pane is missing entirely'); bad++; }
  else if (rp < 4) { console.error(`✗ the radio pane holds only ${rp} card(s) — the per-radio settings are not inside it`); bad++; }
  // ★ The ceiling is a "have the two panes merged?" guard, not a design limit — it catches an
  //   unclosed <div> pulling the radio settings inside the server pane. Raise it when a card is
  //   legitimately added, and say which one: 9 since the machine-wide Waterfall rate joined
  //   (2026-08-11), which Simple mode had and Full mode was missing entirely.
  else if (sp > 9) { console.error(`✗ the server pane holds ${sp} cards — it has swallowed the radio pane`); bad++; }
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


// ★★ AND THE hidden ATTRIBUTE MUST ACTUALLY HIDE. [hidden] is only display:none in the user-agent
//    stylesheet, so any author rule setting display beats it — which has now put a modal on screen
//    over the wrong tab, left an ADMIN MODE banner up for strangers, and needed two per-element
//    patches before that. The rule is cheap; forgetting it is not.
if (!/\[hidden\]\s*\{[^}]*display:\s*none\s*!important/.test(html)) {
  console.error('✗ no global [hidden]{display:none !important} rule — an element with the hidden'
              + ' attribute can be shown by any author display rule');
  bad++;
}

// ★★★ NO INPUT BELOW 16px, ON EITHER PAGE THAT A PHONE OPENS. iOS Safari zooms the whole page in
//     when a focused input's font-size is under 16px, and it never zooms back out — so one
//     under-sized field leaves the radio permanently magnified for the rest of the session. The
//     web client's host/PIN boxes were 15px, one pixel under, and the symptom ("the SDR page
//     starts very slightly zoomed in") pointed at the scrolling landing page instead, because
//     scrolling is what made a stuck viewport visible rather than what caused it.
// ★ maximum-scale/user-scalable is NOT the alternative: it takes pinch-zoom from people who need
//   it, and iOS has ignored it since iOS 10. Meeting the threshold is the only thing that works.
for (const page of ['web/client/index.html', 'android/app/src/main/cpp/vibe_setup_page.h']) {
  // ★★★ NARROW CATCH, AND IT IS THE POINT. This was `catch { continue }` around a call to a bare
  //     `readFileSync` that this module never imported — so every run threw a ReferenceError, the
  //     catch swallowed it, both files were skipped, and the check printed OK while testing
  //     NOTHING. It was only found by deliberately breaking the thing it guards and watching it
  //     pass anyway. A catch that cannot tell "file absent" from "code wrong" turns a test into a
  //     decoration.
  let pageSrc;
  try {
    pageSrc = fs.readFileSync(new URL(`../${page}`, import.meta.url), 'utf8');
  } catch (e) {
    if (e.code === 'ENOENT') continue;         // the only tolerable failure
    throw e;
  }
  for (const m of pageSrc.matchAll(/([^{}\n][^{}]*)\{([^}]*)\}/g)) {
    if (!/\binput\b|\btextarea\b/.test(m[1])) continue;
    const size = m[2].match(/font-size:\s*([0-9.]+)px/);
    if (size && parseFloat(size[1]) < 16) {
      console.error(`✗ ${page}: "${m[1].trim().replace(/\s+/g, ' ').slice(-40)}" sets font-size `
                  + `${size[1]}px — iOS zooms the page in on focus below 16px and never back out`);
      bad++;
    }
  }
}

if (bad) process.exit(1);
console.log(`ok   setup page: JS parses, tags balance, ${ids.size} ids all present`);
console.log('ok   no input under the 16px iOS auto-zoom threshold on either phone-facing page');
