// The owner's landing message: **bold** and *italic* only (src/utils/landingMarkup.ts) — Stuart, 2026-11-12.
// ★★★ Runs EVERY case through the shared tokenizer (web + app) AND the directory page's hand port (ownerMarkup in
//     directory/public/index.html, extracted between its ownerMarkup:start/end markers), so the two cannot drift.
//   npx tsx scripts/test_landing_markup.ts
import fs from 'node:fs';
import path from 'node:path';
import { landingMarkupHtml, landingMarkupSpans } from '../src/utils/landingMarkup';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
console.log('test_landing_markup');

// ── The directory's port, run as the page runs it (its own esc()).
const page = fs.readFileSync(path.join(__dirname, '..', 'directory/public/index.html'), 'utf8');
const a = page.indexOf('/* ── ownerMarkup:start'), b = page.indexOf('/* ── ownerMarkup:end');
ok(a > 0 && b > a, 'the directory page carries the port');
const dirSrc = page.slice(a, b);
// eslint-disable-next-line @typescript-eslint/no-implied-eval
const dirMarkup = new Function(`const esc = (s) => String(s ?? '').replace(/[&<>"']/g,
  (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));\n${dirSrc}\nreturn ownerMarkup;`)() as (t: unknown) => string;

/** Every tag in the output must be exactly <b>, </b>, <i> or </i>, and they must balance. */
function onlyBI(html: string): boolean {
  const tags = html.match(/<[^>]*>/g) ?? [];
  const st: string[] = [];
  for (const t of tags) {
    const m = /^<(\/?)([bi])>$/.exec(t);
    if (!m) return false;
    if (!m[1]) st.push(m[2]); else if (st.pop() !== m[2]) return false;
  }
  return st.length === 0;
}

const cases: Array<[string, string, string]> = [
  ['plain text', 'Hello listeners', 'Hello listeners'],
  ['bold', 'Please **retune** before you leave', 'Please <b>retune</b> before you leave'],
  ['italic', 'The waterfall *slows* at night', 'The waterfall <i>slows</i> at night'],
  ['bold and italic together', '***Both*** at once', '<b><i>Both</i></b> at once'],
  ['italic inside bold', '**a *b* c**', '<b>a </b><b><i>b</i></b><b> c</b>'],
  ['bold inside italic', '*a **b** c*', '<i>a </i><b><i>b</i></b><i> c</i>'],
  ['bold closing an italic run ("*a **b***")', '*a **b***', '<i>a </i><b><i>b</i></b>'],
  ['line breaks kept', 'Line one\n**Line two**', 'Line one\n<b>Line two</b>'],
  ['a pair never crosses a line break', '**open\nclose**', '**open\nclose**'],
  ['unclosed bold shown as typed', '**oops and the rest', '**oops and the rest'],
  ['unclosed italic shown as typed', 'one *two three', 'one *two three'],
  ['a lone star is text', '5 * 3 = 15', '5 * 3 = 15'],
  ['spaced stars are not a pair', 'a * b * c', 'a * b * c'],
  ['four stars is a row of stars', 'Rating ****', 'Rating ****'],
  ['empty pair is not markup', 'x****y', 'x****y'],
  ['underscores are NOT italic (call signs)', 'M0ABC_portable and _this_', 'M0ABC_portable and _this_'],
  ['crossed markers never unbalance', '**a *b** c*', '<b>a *b</b> c*'],
  ['<script> is text', '<script>alert(1)</script>', '&lt;script&gt;alert(1)&lt;/script&gt;'],
  ['<img onerror> inside bold is text', '**<img src=x onerror=alert(1)>**', '<b>&lt;img src=x onerror=alert(1)&gt;</b>'],
  ['quotes and ampersands escaped', `Tom & "Jerry's" <b>`, 'Tom &amp; &quot;Jerry&#39;s&quot; &lt;b&gt;'],
  ['a typed </b> cannot close our tag', '**x</b><script>y**', '<b>x&lt;/b&gt;&lt;script&gt;y</b>'],
  ['entities are not decoded', '&lt;script&gt;', '&amp;lt;script&amp;gt;'],
  ['no links are made', 'See https://example.com/**x** or [a](javascript:alert(1))',
   'See https://example.com/<b>x</b> or [a](javascript:alert(1))'],
  ['empty', '', ''],
];
for (const [what, input, want] of cases) {
  const got = landingMarkupHtml(input), dir = dirMarkup(input);
  ok(got === want, `${what}${got === want ? '' : ` — got ${JSON.stringify(got)}`}`);
  ok(dir === got, `  …the directory agrees${dir === got ? '' : ` — got ${JSON.stringify(dir)}`}`);
}
ok(landingMarkupHtml(undefined) === '' && landingMarkupHtml(42) === '' && dirMarkup(null) === '', 'non-strings draw nothing');

// ── Fuzz: random strings from a hostile alphabet. Every output must be only balanced <b>/<i>, the text with its
//    markers removed must equal the input with its paired markers removed (nothing invented, nothing lost), and the
//    directory must agree.
const alpha = ['*', '*', '**', '***', '<', '>', '&', '"', "'", ' ', '\n', 'a', 'b', 'x', 'script', 'onerror=', '_', '/'];
let seed = 12345;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
let fuzzBad = 0, fuzzDiff = 0, fuzzLost = 0;
const unesc = (h: string) => h.replace(/<\/?[bi]>/g, '').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"')
  .replace(/&#39;/g, "'").replace(/&amp;/g, '&');
for (let n = 0; n < 5000; n++) {
  let s = '';
  const len = 1 + Math.floor(rnd() * 14);
  for (let k = 0; k < len; k++) s += alpha[Math.floor(rnd() * alpha.length)];
  const h = landingMarkupHtml(s);
  if (!onlyBI(h) || /<(?![\/]?[bi]>)/.test(h)) fuzzBad++;
  if (dirMarkup(s) !== h) fuzzDiff++;
  // The visible text is the input minus some '*' characters — never anything else.
  const vis = unesc(h);
  if (vis.replace(/\*/g, '') !== s.replace(/\*/g, '')) fuzzLost++;
  const spans = landingMarkupSpans(s);
  if (spans.map((x) => x.text).join('') !== vis) fuzzLost++;
}
ok(fuzzBad === 0, `fuzz: 5000 hostile strings produce only balanced <b>/<i> (${fuzzBad} bad)`);
ok(fuzzLost === 0, `fuzz: only paired stars are ever removed (${fuzzLost} bad)`);
ok(fuzzDiff === 0, `fuzz: the directory port agrees on all 5000 (${fuzzDiff} differ)`);

process.exit(fails ? 1 : 0);
