import { cleanText } from './safeText';

/**
 * ★★ RDS PS / RadioText, where SPACING IS PART OF THE TEXT. PS is eight positional characters — stations
 * scroll and centre words with spaces — so cleanText's collapsing of space runs would change what is shown
 * and break the PS stabiliser's position matching. This only REPLACES control, bidi-override and zero-width
 * characters with a space, one for one (no collapsing, no trimming — callers trim as they always have), and
 * caps the length. Non-strings → ''.
 */
export function cleanKeepSpacing(v: unknown, max = 64): string {
  if (typeof v !== 'string') return '';
  let s = v.replace(/[\u0000-\u001f\u007f-\u009f‪-‮⁦-⁩​⁠﻿]/g, ' ');
  if (s.length > max) {
    let cut = max;
    const c = s.charCodeAt(cut - 1);
    if (c >= 0xd800 && c <= 0xdbff) cut -= 1;   // never leave half a surrogate pair
    s = s.slice(0, cut);
  }
  return s;
}

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
