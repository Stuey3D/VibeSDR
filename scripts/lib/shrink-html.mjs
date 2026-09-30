/**
 * shrink-html.mjs — take the comments and the indentation OUT OF THE SHIPPED PAGE, and nothing else.
 *
 * ★★★ WHY. index.html is ~340 KB of source, and more than half of it is ★ comments and indentation:
 *     the reasons the page is the way it is. They belong in the SOURCE, where the next person reads
 *     them — not on the wire to a listener on a 2G link in a country where a megabyte costs real
 *     money (Stuart, 2026-09-30: "optimise the page first — that is our biggest issue for users on
 *     wank internet connections"). Measured on 7a5c4e69: CSS 195,757 → 74,066 bytes, markup outside
 *     script/style 216,173 → ~135,000.
 *
 * ★★ WHAT IT TOUCHES, EXACTLY — and each limit is deliberate:
 *    • `<!-- … -->` comments are dropped. (✗ Not `<!--[if …]>` conditional comments — there are
 *      none today, but one would be content, not a note.) Nothing in the client reads a comment
 *      node (checked: no COMMENT_NODE / nodeType 8 / TreeWalker in web/client/src).
 *    • <style> bodies go through esbuild's CSS minifier (comments + whitespace, and it never changes
 *      what a rule means — it is the same minifier the JS already goes through).
 *    • TEXT between tags: a run of whitespace becomes ONE character — a newline if the run had one,
 *      else a space. Under the default `white-space` a run renders as one space whatever it is made
 *      of, so this cannot move a pixel. ✗ It does NOT delete the run: a space between two inline
 *      buttons is a visible gap.
 *    • ✗ NEVER INSIDE A TAG. Attribute values are kept byte for byte: a multi-line `title=` is a
 *      tooltip the listener reads, `style=` is layout, and the server patches the page by finding
 *      `<meta charset="utf-8">` verbatim (the front door's data-frontdoor stamp).
 *    • ✗ NEVER inside <pre>, <textarea> or <script>: their whitespace is content. (The page's
 *      white-space:pre elements — #sigLabel, #decText — are filled by script, not by markup.)
 *
 * ★ Pure function, no I/O, so the build and a test can both call it.
 */
import { transform } from 'esbuild';

const RAW_TEXT = new Set(['script', 'pre', 'textarea']);

/** Where a tag starting at `i` (at '<') ends — the index just past its '>' — honouring quoted
 *  attribute values, which may contain '>' (an inline handler, a CSS selector in a title). */
function tagEnd(s, i) {
  let q = '';
  for (let j = i + 1; j < s.length; j++) {
    const c = s[j];
    if (q) { if (c === q) q = ''; continue; }
    if ((c === '"' || c === "'") && /=\s*$/.test(s.slice(Math.max(i, j - 8), j))) { q = c; continue; }
    if (c === '>') return j + 1;
  }
  throw new Error(`shrink-html: unterminated tag at ${i}: ${JSON.stringify(s.slice(i, i + 60))}`);
}

const collapse = (t) => t.replace(/\s+/g, (ws) => (ws.includes('\n') ? '\n' : ' '));

/**
 * @param {string} html
 * @param {{ target: string[] }} opts  esbuild CSS target (the JS bundle's browsers)
 * @returns {Promise<string>}
 */
export async function shrinkHtml(html, { target }) {
  let out = '';
  // ★ Text is held until the next TAG, not the next '<': a dropped comment must not leave the
  //   whitespace on either side of it as two separate runs (a blank line per comment).
  let text = '';
  const flush = () => { out += collapse(text); text = ''; };
  let i = 0;
  while (i < html.length) {
    const lt = html.indexOf('<', i);
    if (lt < 0) { text += html.slice(i); break; }
    text += html.slice(i, lt);
    i = lt;
    if (html.startsWith('<!--', i)) {
      const end = html.indexOf('-->', i + 4);
      if (end < 0) throw new Error(`shrink-html: unterminated comment at ${i}`);
      if (html.startsWith('<!--[if', i)) { flush(); out += html.slice(i, end + 3); }
      i = end + 3;
      continue;
    }
    const m = /^<([A-Za-z][A-Za-z0-9-]*)/.exec(html.slice(i, i + 32));
    if (!m && !/^<[/!?]/.test(html.slice(i, i + 2))) {   // a bare '<' in text ("a < b")
      text += '<';
      i++;
      continue;
    }
    flush();
    const e = tagEnd(html, i);
    const tag = html.slice(i, e);
    const name = m ? m[1].toLowerCase() : '';
    out += tag;
    i = e;
    if (name === 'style' || RAW_TEXT.has(name)) {
      const close = html.toLowerCase().indexOf(`</${name}`, i);
      if (close < 0) throw new Error(`shrink-html: <${name}> at ${i} never closes`);
      const body = html.slice(i, close);
      if (name === 'style') {
        const r = await transform(body, { loader: 'css', minify: true, target, legalComments: 'none' });
        out += r.code.trimEnd();
      } else {
        out += body;
      }
      i = close;
    }
  }
  flush();
  return out;
}
