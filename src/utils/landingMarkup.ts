/**
 * ★★★ BOLD AND ITALIC IN THE OWNER'S LANDING MESSAGE — and nothing else (Stuart, 2026-11-12: "HTML I really dont
 * want on the SDR itself. Italics etc are fine but I don't want scripts that could potentially harm users being
 * hosted on VibeServers. We already have a separate box for a hyperlink").
 *
 *   **bold**    *italic*    ***both***       line breaks as typed
 *
 * ★★★ WHY THIS IS STILL SAFE. The message is owner-authored text shown to strangers by every client we ship, so
 *     it is never parsed as markup. This tokenizer turns the text into SPANS — plain strings with two flags — and
 *     each renderer draws a span as text: the web escapes every character of it and wraps it in nothing but <b>
 *     and <i> (landingMarkupHtml below); the app nests <Text> with a weight or a style. No tag, attribute, URL or
 *     entity the owner types can ever reach the page as anything but characters.
 *  ★★ NO LINKS, EVER. The link has its own box, whose scheme is checked at both ends (vibeserver_config.h); a
 *     link inside the prose would mean parsing URLs out of owner text, which is the hole that box exists to avoid.
 *  ★★ ONE SYNTAX. `_underscores_` are NOT italic: call signs, file names and handles (M0ABC_portable, my_server)
 *     carry them, and turning half a name italic is a worse surprise than a missing feature.
 *  ★ A marker that does not pair is shown AS TYPED: "**oops" stays "**oops", never the rest of the message bold.
 *    Pairs never cross a line break, so an unclosed marker cannot reach beyond its own line either.
 *  ★ The same rules as Markdown where they matter to a person typing: an opener must touch the word after it
 *    and a closer the word before it, so "5 * 3 * 2" and a lone "*" are left alone.
 *
 * ✗ The directory page carries a hand port of this (directory/public/index.html ownerMarkup) — it is a static page
 *   with no build step. scripts/test_landing_markup.ts runs the SAME cases through both; change both or neither.
 */

export interface MarkupSpan { text: string; b: boolean; i: boolean }

type Tok =
  | { t: 'txt'; s: string }
  | { t: 'del'; kind: 'b' | 'i'; raw: string; open: boolean; close: boolean; matched: boolean; on?: boolean;
      /** The first half of a "***" run (its second half is the next token). */
      tri?: boolean };
type Del = Extract<Tok, { t: 'del' }>;

const isSpace = (c: string | undefined) => c === undefined || /\s/.test(c);

/** One line → tokens with their delimiters paired. */
function pairLine(line: string): Tok[] {
  const toks: Tok[] = [];
  let i = 0;
  while (i < line.length) {
    if (line[i] !== '*') {
      let j = i;
      while (j < line.length && line[j] !== '*') j++;
      toks.push({ t: 'txt', s: line.slice(i, j) });
      i = j;
      continue;
    }
    let j = i;
    while (j < line.length && line[j] === '*') j++;
    const n = j - i;
    const prev = line[i - 1], next = line[j];
    const open = !isSpace(next), close = !isSpace(prev);
    // ★ A run of four or more is a row of stars someone typed (a divider, a rating) — never markup.
    if (n > 3 || (!open && !close)) toks.push({ t: 'txt', s: line.slice(i, j) });
    else if (n === 3) {
      toks.push({ t: 'del', kind: 'b', raw: '**', open, close, matched: false, tri: true });
      toks.push({ t: 'del', kind: 'i', raw: '*', open, close, matched: false });
    } else toks.push({ t: 'del', kind: n === 2 ? 'b' : 'i', raw: '*'.repeat(n), open, close, matched: false });
    i = j;
  }

  // Pair them: a closer takes the NEAREST open opener of its kind; anything opened inside that pair and still open
  // is abandoned (shown as typed) — so crossed markers like "**a *b** c*" can never leave a tag unbalanced.
  const stack: number[] = [];
  for (let k = 0; k < toks.length; k++) {
    const d = toks[k];
    if (d.t !== 'del') continue;
    // ★ "***" closing: close whichever of its two halves is innermost first, so "*a **b***" and "***x***" pair.
    if (d.tri && d.close && stack.length && (toks[stack[stack.length - 1]] as Del).kind === 'i') {
      const half = toks[k + 1] as Del;
      d.tri = false;
      toks[k] = half; toks[k + 1] = d;
    }
    const cur = toks[k] as Del;
    if (cur.close) {
      let at = -1;
      for (let s = stack.length - 1; s >= 0; s--) {
        if ((toks[stack[s]] as Del).kind === cur.kind) { at = s; break; }
      }
      // ★ An empty pair ("****" is already text; "** **" has spaces) is not markup either.
      if (at >= 0 && stack[at] !== k - 1) {
        const opener = toks[stack[at]] as Del;
        opener.matched = true; opener.on = true;
        cur.matched = true; cur.on = false;
        stack.length = at;
        continue;
      }
    }
    if (cur.open) stack.push(k);
  }
  return toks;
}

/** The message as spans. Text is returned exactly as typed apart from the markers that paired. */
export function landingMarkupSpans(text: unknown): MarkupSpan[] {
  if (typeof text !== 'string' || !text) return [];
  const out: MarkupSpan[] = [];
  const push = (s: string, b: boolean, i: boolean) => {
    if (!s) return;
    const last = out[out.length - 1];
    if (last && last.b === b && last.i === i) last.text += s;
    else out.push({ text: s, b, i });
  };
  const lines = text.split(/\r\n|\r|\n/);
  lines.forEach((line, li) => {
    if (li > 0) push('\n', false, false);
    let b = 0, it = 0;
    for (const tk of pairLine(line)) {
      if (tk.t === 'txt') { push(tk.s, b > 0, it > 0); continue; }
      if (!tk.matched) { push(tk.raw, b > 0, it > 0); continue; }
      const d = tk.on ? 1 : -1;
      if (tk.kind === 'b') b += d; else it += d;
    }
  });
  return out;
}

const ESC: Record<string, string> = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
/** Every character that could start markup, escaped. */
export function escapeMarkupText(s: string): string {
  return s.replace(/[&<>"']/g, (c) => ESC[c]);
}

/** ★★★ The web's rendering: escaped text, and the only tags it can ever produce are <b> and <i>. Line breaks stay
 *  as '\n' — the host element is `white-space: pre-wrap`. */
export function landingMarkupHtml(text: unknown): string {
  return landingMarkupSpans(text).map((sp) => {
    let h = escapeMarkupText(sp.text);
    if (sp.i) h = `<i>${h}</i>`;
    if (sp.b) h = `<b>${h}</b>`;
    return h;
  }).join('');
}

/** The hint the setup screens show under the field — one sentence, the same everywhere. */
export const LANDING_MARKUP_HINT = '**bold** and *italic* are allowed. No HTML; put a link in the link box below.';
