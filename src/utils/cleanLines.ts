import { cleanText } from './safeText';

/**
 * cleanText for text whose LINE BREAKS are part of it — an operator's landing message ("house rules",
 * a donation line). Each line is cleaned on its own (controls, bidi overrides, zero-width padding),
 * blank runs collapse to one empty line, and the whole is capped at `max` characters and 12 lines.
 */
export function cleanLines(v: unknown, max = 500): string {
  if (typeof v !== 'string') return '';
  const lines = v.split(/\r\n|\r|\n/).map((l) => cleanText(l, max));
  const out: string[] = [];
  for (const l of lines) {
    if (!l && (out.length === 0 || out[out.length - 1] === '')) continue;
    out.push(l);
    if (out.length >= 12) break;
  }
  while (out.length && out[out.length - 1] === '') out.pop();
  let s = out.join('\n');
  if (s.length > max) {
    let cut = max;
    const c = s.charCodeAt(cut - 1);
    if (c >= 0xd800 && c <= 0xdbff) cut -= 1;   // never leave half a surrogate pair
    s = s.slice(0, cut).trimEnd();
  }
  return s;
}
