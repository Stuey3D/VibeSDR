/**
 * ★★★ TEXT FROM ANY SERVER IS UNTRUSTED (Stuart, 2026-10-03: "make sure someone doesn't corrupt their app or
 *     web client with the broken bookmark text too, so sandbox it all"). A station or bookmark name can come from
 *     an admin's typing, from RDS/DAB decoded off the air — garbled, or sent by anyone with a transmitter — from
 *     a server older than our own fixes, or from a server that is not ours at all. Shared by the app and the web
 *     client, so both clean it the same way, on the way IN, before anything stores or draws it.
 *
 *  - Not a string (a number, an object, null) → '' — the caller drops the entry. `name.trim()` on a number threw.
 *  - C0/C1 control characters and DEL → a space: a newline or NUL in a label breaks layouts and storage.
 *  - Bidi overrides and isolates (U+202A–202E, U+2066–2069) → removed: they reverse the text drawn AFTER them
 *    on the same line ("Trojan Source" style). Ordinary right-to-left scripts are untouched.
 *  - Zero-width space, word joiner and BOM (U+200B, U+2060, U+FEFF) → removed: invisible padding. ★ NOT the
 *    joiners U+200C/U+200D — Indic scripts and multi-part emoji need them (and Tibetan, Thai, Arabic… all pass).
 *  - Runs of spaces collapsed, trimmed, and capped (default 64 characters, never splitting a surrogate pair).
 */
export function cleanText(v: unknown, max = 64): string {
  if (typeof v !== 'string') return '';
  let s = v
    .replace(/[\u0000-\u001f\u007f-\u009f]/g, ' ')
    .replace(/[\u202a-\u202e\u2066-\u2069]/g, '')
    .replace(/[\u200b\u2060\ufeff]/g, '')
    .replace(/\s+/g, ' ')
    .trim();
  if (s.length > max) {
    let cut = max;
    const c = s.charCodeAt(cut - 1);
    if (c >= 0xd800 && c <= 0xdbff) cut -= 1;   // never leave half a surrogate pair
    s = s.slice(0, cut).trim();
  }
  return s;
}

/** A demodulator name as a server sends it: lower-case letters and digits only, short. '' if not one. */
export function cleanMode(v: unknown): string {
  return typeof v === 'string' ? v.toLowerCase().replace(/[^a-z0-9]/g, '').slice(0, 12) : '';
}
