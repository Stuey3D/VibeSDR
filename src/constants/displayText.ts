/**
 * displayText.ts — what a faceplate DISPLAY can physically show (faceplates brief §7, §7.1).
 *
 * A real VFD or 14-segment tube could only draw what its character ROM held. These are the pure
 * text rules for the dot-matrix (`dot`, Doto) and 14-segment (`seg`, DSEG14) displays:
 *
 *   toSegCells()      text → one DSEG14 cell per character (the font has traps; see below)
 *   foldForSeg()      accents → plain A–Z, like a segment ROM
 *   foldForDot()      keep what Doto can draw (it has Latin-1 and most of Latin Extended-A)
 *   toUpperDisplay()  upper case EXCEPT the units — "the radio nerds will have our heads"
 *   flagToIso()       🇬🇧 → 'GB' (a colour emoji cannot exist on a VFD)
 *   displayOrFallback()  a string that folds to nothing usable → the frequency (+ any Latin callsign)
 *
 * ★★ FOLDING IS FOR THE DISPLAY ONLY. Search, bookmarks and the Hyperlegible / Nixie displays keep
 *   the original text. Nothing here may be written back anywhere.
 * ★★ NON-LATIN SCRIPTS: the brief asks for the platform ICU transliterator (`Any-Latin; Latin-ASCII`
 *   — CFStringTransform on iOS, android.icu.text.Transliterator on Android 10+). Neither is
 *   reachable from JS: Hermes has `String.prototype.normalize` but no transliterator, and Intl has
 *   nothing that does it. So this is NOT WIRED YET — it needs a small native module, a separate
 *   step. Until then Cyrillic / Greek / Arabic / CJK fold to nothing and `displayOrFallback()` shows
 *   the frequency and whatever Latin callsign survived, never empty cells or tofu. The hook for the
 *   native step is `setTransliterator()` below: install a sync `Any-Latin` function and it runs
 *   before the fold.
 *
 * Pure: no React, no imports — scripts/test_faceplate_text.ts runs it under plain Node.
 */

// ── Transliteration hook (native, not yet wired) ─────────────────────────────

type Transliterate = (s: string) => string;
let transliterate: Transliterate | null = null;
/** ★ For the native ICU step: install a synchronous `Any-Latin` transform. null = not available. */
export function setTransliterator(f: Transliterate | null): void { transliterate = f; }

// ── Folding ──────────────────────────────────────────────────────────────────

/** The letters NFD does not decompose (brief §7), plus their lower-case twins and Ð (which looks
 *  exactly like Đ). ★ Å and the rest DO decompose; they are listed where the brief lists them only
 *  because NFD handles them anyway. */
const SPECIAL: Record<string, string> = {
  'ß': 'ss', 'ẞ': 'SS',
  'Æ': 'AE', 'æ': 'ae', 'Œ': 'OE', 'œ': 'oe',
  'Ø': 'O',  'ø': 'o',  'Ł': 'L',  'ł': 'l',
  'Đ': 'D',  'đ': 'd',  'Ð': 'D',  'ð': 'd',
  'Þ': 'TH', 'þ': 'th', 'ı': 'i',
};
const SPECIAL_RE = /[ßẞÆæŒœØøŁłĐđÐðÞþı]/g;
const COMBINING_RE = /[̀-ͯ]/g;
/** Typographic punctuation EiBi and RDS carry, to its ASCII cell. */
const PUNCT: Record<string, string> = {
  '‘': "'", '’': "'", '‚': ',', '‛': "'",
  '“': '"', '”': '"', '„': '"',
  '–': '-', '—': '-', '−': '-', '…': '...',
  ' ': ' ', ' ': ' ', ' ': ' ', '\t': ' ', '\n': ' ', '\r': ' ',
};

function preFold(text: string): string {
  let t = transliterate ? transliterate(text) : text;
  t = t.replace(/[‘-„–—−…   \t\n\r]/g, (c) => PUNCT[c] ?? c);
  return t;
}

/** Plain ASCII: the brief's special cases, then NFD with the combining marks stripped. Case kept. */
export function foldToAscii(text: string): string {
  return preFold(text).replace(SPECIAL_RE, (c) => SPECIAL[c] ?? c)
    .normalize('NFD').replace(COMBINING_RE, '');
}

/** 14-segment: everything to plain upper-case A–Z / 0–9 and a few symbols. */
export function foldForSeg(text: string): string {
  return foldToAscii(text).toUpperCase();
}

/**
 * The non-ASCII characters Doto 900 (assets/fonts/Doto-Black.ttf) actually has a glyph for, read
 * from its cmap (combining marks left out — they are never drawn alone). A real dot-matrix VFD
 * often had Latin-1 in ROM, so these are KEPT on the dot display; everything else folds.
 * ★ If the font file is replaced, regenerate this from its cmap.
 */
export const DOTO_EXTRA =
  '¡¢£¥§¨©ª«®¯°´¶·¸º»¿ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖ×ØÙÚÛÜÝÞßàáâãäåæçèéêëìíîïðñòóôõö÷øùúûüýþÿ'
  + 'ĀāĂăĄąĆćĊċČčĎďĐđĒēĖėĘęĚěĞğĠġĢģĦħĪīĮįİıĶķĹĺĻļĽľŁłŃńŅņŇňŐőŒœŔŕŘřŚśŞşŠšŤťŪūŮůŰűŲųŴŵŶŷŸŹźŻżŽž'
  + 'ȘșȚțȷˆˇ˘˙˚˛˜˝ẀẁẂẃẄẅẞỲỳ–—‘’‚“”„•…‹›€™−';
const DOTO_SET = new Set<string>(DOTO_EXTRA);

/** Can Doto draw `ch` as it stands? (Printable ASCII: all of it.) */
export function dotoHas(ch: string): boolean {
  const c = ch.codePointAt(0) ?? 0;
  return (c >= 0x20 && c <= 0x7e) || DOTO_SET.has(ch);
}

/**
 * Dot matrix: keep the accent where Doto has the glyph; fold only what it cannot draw; anything
 * still undrawable becomes a space (a blank cell), never a tofu box. Case kept — Doto has lower
 * case; the VTS upper-cases with toUpperDisplay() afterwards.
 * ★ Works on the precomposed (NFC) form, so "e + ◌́" from a decomposed source is drawn as é.
 */
export function foldForDot(text: string): string {
  const src = (transliterate ? transliterate(text) : text).normalize('NFC');
  let out = '';
  for (const ch of src) {
    if (dotoHas(ch)) { out += ch; continue; }
    const f = foldToAscii(ch);
    for (const c of f) out += dotoHas(c) ? c : ' ';
  }
  return out;
}

// ── 14-segment cells ─────────────────────────────────────────────────────────

/** DSEG14's blank cell. ★ NOT an exclamation mark — DSEG draws `!` as an empty 816-unit cell. */
export const SEG_BLANK = '!';
/** DSEG14's all-segments-on glyph: the ghost layer. */
export const SEG_GHOST = '~';

/** Characters DSEG14 Classic draws a FULL cell for, used as-is (measured from the TTF, §7). */
const SEG_OK = /^[A-Z0-9?@&$%'"()*+,\-/<=>^_\\|`]$/;
/** The traps and the missing glyphs (§7). */
const SEG_MAP: Record<string, string> = {
  '!': '|', '~': '-', ':': '-',
  '#': 'H', ';': ',', '[': '(', ']': ')',
  ' ': SEG_BLANK,
};

/**
 * ★★★ One character → exactly one 14-segment cell (brief §7). The returned string is fed to
 * DSEG14 verbatim.
 *  - every space is sent as `!` (DSEG's space is only 200 wide and would push the grid off);
 *  - a real `!` → `|`, a real `~` → `-` (DSEG's `~` is the all-on ghost);
 *  - `.` becomes the PREVIOUS cell's decimal point (zero width in DSEG — exactly how real
 *    14-segment displays did it, one DP per cell); a `.` with no cell before it, or after a cell
 *    that already has its point, gets a blank cell of its own;
 *  - `:` → `-` in its own cell;  `# ; [ ]` → `H , ( )`;
 *  - anything left after folding and mapping → a blank cell, never a tofu box.
 * The ghost layer is `segGhost(cells)`.
 */
export function toSegCells(text: string): string {
  const t = foldForSeg(text);
  const cells: string[] = [];
  for (const ch of t) {
    if (ch === '.') {
      const last = cells.length - 1;
      if (last >= 0 && !cells[last].endsWith('.')) cells[last] += '.';
      else cells.push(SEG_BLANK + '.');
      continue;
    }
    const m = SEG_MAP[ch] ?? ch;
    cells.push(m === SEG_BLANK || SEG_OK.test(m) ? m : SEG_BLANK);
  }
  return cells.join('');
}

/** How many cells a toSegCells() string occupies (its points ride on their cells). */
export function segCellCount(cells: string): number {
  let n = 0;
  for (const ch of cells) if (ch !== '.') n++;
  return n;
}

/** The ghost layer for a run of cells: every segment of every cell, dim. */
export function segGhost(n: number): string {
  return SEG_GHOST.repeat(Math.max(0, n));
}

/** Split a toSegCells() string into its cells ('A', 'B.', '!'). Used by the stepped scroll. */
export function segCellList(cells: string): string[] {
  const out: string[] = [];
  for (const ch of cells) {
    if (ch === '.' && out.length) out[out.length - 1] += '.';
    else out.push(ch);
  }
  return out;
}

// ── Case ─────────────────────────────────────────────────────────────────────

/** Units keep their case on every display (§7 TRAP). Longest first, so dBFS is not read as dB. */
export const UNIT_WHITELIST = ['dBFS', 'dBm', 'dB', 'kHz', 'MHz', 'Hz', 'k/s', 'fps'] as const;
const UNIT_RE = /(dBFS|dBm|dB|kHz|MHz|Hz|k\/s|fps)/;
const isLetter = (c: string | undefined) => !!c && c.toLowerCase() !== c.toUpperCase();

/**
 * ★★ Upper-case for a display, EXCEPT the units (`dB dBm dBFS Hz kHz MHz k/s fps`, exactly so).
 * Never `.toUpperCase()` a whole string. A unit counts only as a whole token — not glued to other
 * letters on either side ("25.4dB" and "6k/s" keep theirs; "HzFoo" does not).
 */
export function toUpperDisplay(text: string): string {
  const parts = text.split(UNIT_RE);
  let out = '';
  for (let i = 0; i < parts.length; i++) {
    const p = parts[i];
    if (i % 2 === 1) {
      const before = parts[i - 1], after = parts[i + 1];
      const standalone = !isLetter(before ? before[before.length - 1] : undefined)
                      && !isLetter(after ? after[0] : undefined);
      out += standalone ? p : p.toUpperCase();
    } else {
      out += p.toUpperCase();
    }
  }
  return out;
}

// ── Flags ────────────────────────────────────────────────────────────────────

/**
 * A flag emoji → its ISO code in plain letters (§7.1): the regional-indicator pair 🇬🇧 → 'GB'.
 * A subdivision flag (🏴 + tag letters, e.g. England) → its country's two letters ('GB').
 * Anything else → '' (the cells stay ghosted).
 */
export function flagToIso(flag: string | null | undefined): string {
  if (!flag) return '';
  let ri = '', tag = '';
  for (const ch of flag) {
    const cp = ch.codePointAt(0) ?? 0;
    if (cp >= 0x1f1e6 && cp <= 0x1f1ff) ri += String.fromCharCode(cp - 0x1f1e6 + 65);
    else if (cp >= 0xe0061 && cp <= 0xe007a) tag += String.fromCharCode(cp - 0xe0061 + 65);
  }
  if (ri.length >= 2) return ri.slice(0, 2);
  if (tag.length >= 2) return tag.slice(0, 2);
  return '';
}

// ── Usable or not ────────────────────────────────────────────────────────────

/** Letters and digits in a string — what a reader actually reads. */
function significant(s: string): number {
  let n = 0;
  for (const ch of s) {
    if (/[0-9]/.test(ch) || isLetter(ch)) n++;
  }
  return n;
}

/** Letters and digits the DISPLAY can actually draw: A–Z / 0–9 on a segment ROM, Doto's glyphs on
 *  the dot matrix. (A Cyrillic letter upper-cases happily and is still not a segment pattern.) */
function drawable(s: string, display: 'dot' | 'seg'): number {
  let n = 0;
  for (const ch of s) {
    if (display === 'seg' ? /[A-Z0-9]/.test(ch) : (dotoHas(ch) && (/[0-9]/.test(ch) || isLetter(ch)))) n++;
  }
  return n;
}

/**
 * Did the fold leave something worth showing? At least one drawable letter or digit, and at least
 * half as many as the original had — "Радио 1" folds to "1", which is not the station's name.
 */
export function foldIsUsable(original: string, folded: string, display: 'dot' | 'seg'): boolean {
  const o = significant(original);
  const f = drawable(folded, display);
  if (f === 0) return false;
  return o === 0 || f / o >= 0.5;
}

/**
 * ★ The whole display rule for one string on a VFD: fold for the display (`dot` keeps Doto's
 * accents, `seg` goes to plain upper case); if nothing usable comes back, show the frequency and
 * any Latin callsign that did survive (whole words that folded without loss) — never empty cells.
 * Returns plain text (not yet segment cells; toSegCells() does that last).
 */
export function displayOrFallback(text: string, display: 'dot' | 'seg', freqLabel: string): string {
  const folded = display === 'dot' ? foldForDot(text) : foldForSeg(text);
  if (foldIsUsable(text, folded, display)) return folded;
  const callsign = text.split(/\s+/).filter((w) => {
    if (!w || significant(w) === 0) return false;
    return significant(foldToAscii(w)) === significant(w) && /^[\x20-\x7e]+$/.test(foldToAscii(w));
  }).join(' ');
  return callsign ? `${freqLabel} ${callsign}` : freqLabel;
}
