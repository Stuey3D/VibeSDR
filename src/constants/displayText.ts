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
 * ★★ NON-LATIN SCRIPTS (Cyrillic, Greek, Arabic, CJK…): the platform ICU transliterator
 *   (`Any-Latin; Latin-ASCII` — NSString applyingTransform on iOS, android.icu.text.Transliterator on
 *   Android 10+) runs FIRST, then the fold below, then — if still nothing usable — the frequency +
 *   Latin callsign fallback. Hermes has no transliterator, so the native call is installed from
 *   outside with `setTransliterator()` (src/services/transliterator.ts does it at app start). Only
 *   the NON-LATIN RUNS of a string are sent to it, so Latin accents survive for the dot display
 *   ("Rádio Россия" keeps its á); every answer is memoised (bounded LRU), so a long EiBi list costs
 *   each distinct name one native call. With nothing installed (web, tests, an old binary) non-Latin
 *   names fall back to the frequency, never empty cells or tofu.
 *
 * Pure: no React, no imports — scripts/test_faceplate_text.ts runs it under plain Node.
 */

// ── Transliteration (native ICU, installed from outside) ─────────────────────

type Transliterate = (s: string) => string;
let transliterate: Transliterate | null = null;
/** Bounded LRU of native answers, keyed on the non-Latin run sent. A Map iterates in insertion
 *  order, so the first key is the least recently used. */
export const TRANSLIT_CACHE_MAX = 500;
const translitCache = new Map<string, string>();

/** ★ Install the synchronous `Any-Latin; Latin-ASCII` transform (null = none). Clears the memo. */
export function setTransliterator(f: Transliterate | null): void {
  transliterate = f;
  translitCache.clear();
}

/** Characters that are Latin (or script-neutral) and never need the transliterator: ASCII, Latin-1,
 *  Latin Extended-A/B, IPA, spacing modifiers, combining diacritics, Latin Extended Additional,
 *  general punctuation, currency, letterlike symbols, Latin ligatures. Everything else is a run
 *  worth offering to ICU (it hands back anything it cannot transliterate unchanged). */
const LATIN_CH = '\\u0000-\\u036F\\u1E00-\\u1EFF\\u2000-\\u206F\\u20A0-\\u20CF\\u2100-\\u214F\\uFB00-\\uFB06';
/** A run of non-Latin characters, spaces inside it kept so ICU sees whole phrases. */
const NON_LATIN_RUN = new RegExp(`[^${LATIN_CH}]+(?:\\s+[^${LATIN_CH}]+)*`, 'g');
const HAS_NON_LATIN = new RegExp(`[^${LATIN_CH}]`);

function translitRun(run: string): string {
  const hit = translitCache.get(run);
  if (hit !== undefined) {
    translitCache.delete(run); translitCache.set(run, hit);           // most recently used
    return hit;
  }
  let out: string;
  try {
    const r = transliterate!(run);
    out = typeof r === 'string' ? r : run;
  } catch {
    out = run;                                                          // a native failure = no answer
  }
  translitCache.set(run, out);
  if (translitCache.size > TRANSLIT_CACHE_MAX) {
    const oldest = translitCache.keys().next().value;
    if (oldest !== undefined) translitCache.delete(oldest);
  }
  return out;
}

/** Step 1 of the fold: the non-Latin runs through the installed transliterator; Latin untouched. */
export function transliterateNonLatin(text: string): string {
  if (!transliterate || !HAS_NON_LATIN.test(text)) return text;
  return text.normalize('NFC').replace(NON_LATIN_RUN, translitRun);
}

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
  let t = transliterateNonLatin(text);
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
  const src = transliterateNonLatin(text).normalize('NFC');
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
    if (!w || significant(w) === 0 || HAS_NON_LATIN.test(w)) return false;
    return significant(foldToAscii(w)) === significant(w) && /^[\x20-\x7e]+$/.test(foldToAscii(w));
  }).join(' ');
  return callsign ? `${freqLabel} ${callsign}` : freqLabel;
}

// ── The status display (§8.1) ────────────────────────────────────────────────

export interface GainParts { label: string; dir: 'up' | 'down' | null; value: string; tail: string }

/**
 * The bus's gain reading ("GAIN ↓ 25.4 dB", "GAIN · 3.0 dB (held)") split so the status display can
 * DRAW the arrow — Doto has no arrow glyph (§8.1) — and write the value the mockup's way ("25.4dB").
 * null when the text is not a gain reading ("AGC"), which is then shown as it is.
 */
export function statusGainParts(agcText: string): GainParts | null {
  const m = /^GAIN\s*([↑↓·])?\s*(-?[\d.]+)\s*dB(.*)$/.exec(agcText);
  if (!m) return null;
  return { label: 'GAIN', dir: m[1] === '↑' ? 'up' : m[1] === '↓' ? 'down' : null, value: `${m[2]}dB`, tail: m[3] };
}

/**
 * ★★★ IN DIRECT SAMPLING THE STATUS ROW SAYS "Direct Sample", NOT A GAIN (Stuart, B10, 2026-10-01:
 *  "it should say Direct Sample"). The tuner is bypassed — on the Q or I branch the ADC is fed
 *  straight from the aerial — so there is no gain to report, and VibeAGC has stood down (the server
 *  stops stepping in direct sampling). The last "GAIN ↓25.4dB" the loop wrote before the crossover
 *  just stood there, reading as a gain the radio was still applying.
 *  ★ ONLY the gain item. The IF figure beside it STAYS: the owner's receiver uses the IF filter in
 *    direct sampling and it "actually responds great" — it is the one control still working there.
 *  ★ `dsLive` is hwinfo's `ds` (the mode the radio is in NOW: 0 tuner, 1 I, 2 Q; −1 unknown), never
 *    the owner's setting — an AUTO receiver above the crossover is on its tuner and has a real gain.
 */
export const STATUS_DIRECT_SAMPLE = 'Direct Sample';
export function statusGainText(agcText: string, dsLive: number | undefined): string {
  return (dsLive ?? 0) > 0 ? STATUS_DIRECT_SAMPLE : agcText;
}

/**
 * ★ Row 9's hook: the landscape status row's DROP ORDER (§8.2), first to go first. The connection
 * meter is never in it — it is never dropped. Measured with onLayout, never by device model.
 */
export const STATUS_DROP_ORDER = ['if', 'gain', 'linkIcons', 'localTime', 'dsp', 'rate', 'shared', 'utc', 'rec'] as const;
export type StatusItem = typeof STATUS_DROP_ORDER[number];

/**
 * One thing in the landscape status row, as the row lays it out: its natural width (measured, with
 * any margin of its own) and the `lead` gap the row puts before it when something precedes it in
 * its section. `item: null` is the CONNECTION METER — never dropped (§8.2). Width 0 = not present
 * (no DSP on, not a shared server…): it costs nothing and dropping it frees nothing.
 */
export interface StatusUnit { item: StatusItem | null; width: number; lead: number }

/** The row: three sections (times left, SHARED TUNER + DSP centre, link right), `sectionGap` apart. */
export interface StatusRowSpec {
  left: StatusUnit[]; centre: StatusUnit[]; right: StatusUnit[];
  sectionGap: number;
  /** Width of SHARED TUNER shortened to `SHARED` (§8.2: it shortens before it is dropped). */
  sharedShort?: number;
}

/**
 * The row's state after `step` steps:
 *   0          today's layout — every item, the side sections EQUAL (flex 1) so the centre is centred;
 *   1          PACKED — every item, the sides content-sized (space-between). Same items, more room;
 *   2 …        one step per STATUS_DROP_ORDER item, in order, except that `shared` takes TWO:
 *              shorten to `SHARED`, then drop.
 */
export interface StatusFit { step: number; packed: boolean; sharedShort: boolean; hidden: Set<StatusItem> }

export function statusStepCount(order: readonly StatusItem[] = STATUS_DROP_ORDER): number {
  return 2 + order.length + (order.includes('shared') ? 1 : 0);
}

export function statusState(step: number, order: readonly StatusItem[] = STATUS_DROP_ORDER): StatusFit {
  const hidden = new Set<StatusItem>();
  let sharedShort = false;
  let k = 2;
  for (const it of order) {
    if (it === 'shared') {
      if (step >= k) sharedShort = true;
      k++;
    }
    if (step >= k) hidden.add(it);
    k++;
  }
  return { step, packed: step >= 1, sharedShort: sharedShort && !hidden.has('shared'), hidden };
}

/** The width a section needs with `st`'s items shown. */
function sectionWidth(units: StatusUnit[], st: StatusFit, sharedShort: number | undefined): number {
  let w = 0, any = false;
  for (const u of units) {
    if (u.item && st.hidden.has(u.item)) continue;
    const width = u.item === 'shared' && st.sharedShort && sharedShort !== undefined ? sharedShort : u.width;
    if (width <= 0) continue;
    w += width + (any ? u.lead : 0);
    any = true;
  }
  return w;
}

/** Does the row fit `available` pt in state `st`? (Half a point of rounding slack.) */
export function statusFits(available: number, spec: StatusRowSpec, st: StatusFit): boolean {
  const L = sectionWidth(spec.left, st, spec.sharedShort);
  const C = sectionWidth(spec.centre, st, spec.sharedShort);
  const R = sectionWidth(spec.right, st, spec.sharedShort);
  // The side sections are always laid out; the centre only when it has something in it.
  const gaps = spec.sectionGap * (C > 0 ? 2 : 1);
  const need = st.packed ? L + C + R + gaps : 2 * Math.max(L, R) + C + gaps;
  return need <= available + 0.5;
}

/**
 * ★★★ §8.2 — WHAT THE LANDSCAPE STATUS ROW DROPS TO FIT, from MEASURED widths (never the device model).
 *   Items go strictly in STATUS_DROP_ORDER (IF first … the recording timer last); SHARED TUNER
 *   shortens to `SHARED` before it goes; the connection meter is never dropped. Before anything is
 *   dropped the row PACKS (sides content-sized), so nothing drops where the items fit at all.
 * ★★ PORTRAIT NEVER DROPS: it is the full readout (two lines), so it is step 0 whatever the width.
 * ★ HYSTERESIS: dropping more happens at once (nothing may overflow), but bringing an item BACK needs
 *   `hysteresis` pt to spare — so a rate readout ticking from 9k/s to 10k/s cannot make IF flap.
 *   Pass the previous result's `step` as `prevStep`.
 */
export function statusFit(available: number, spec: StatusRowSpec, opts: {
  portrait?: boolean; prevStep?: number; hysteresis?: number; order?: readonly StatusItem[];
  /** ★★ CENTRE FIRST (Stuart, 2026-10-03: "I dont like the shared tuner message being off centre"; "gain and
   *  filter are less important if the signals are clean"): the first `centreFirst` items of the order may DROP
   *  WHILE THE SIDES STAY EQUAL — the centre stays centred — before the row packs. 0 / absent = §8.2 as built. */
  centreFirst?: number;
} = {}): StatusFit {
  const order = opts.order ?? STATUS_DROP_ORDER;
  if (opts.portrait) return statusState(0, order);
  const seq = statusSequence(order, opts.centreFirst ?? 0);
  const first = (avail: number) => {
    for (let i = 0; i < seq.length; i++) if (statusFits(avail, spec, seq[i])) return i;
    return seq.length - 1;
  };
  const k = first(available);
  const prev = opts.prevStep;
  const at = (i: number) => ({ ...seq[i], step: i });
  if (prev === undefined || k >= prev) return at(k);
  // Room to bring something back — only with the hysteresis to spare, and never past where we were.
  return at(Math.min(prev, first(available - (opts.hysteresis ?? 0))));
}

/** The states a row tries, in order: with `centreFirst` = n, the first n drops UNPACKED (centred), then §8.2's
 *  packed steps from the one that already has those n dropped. n = 0 is exactly §8.2's sequence. */
export function statusSequence(order: readonly StatusItem[], centreFirst: number): StatusFit[] {
  const n = Math.max(0, Math.min(centreFirst, order.filter(it => it !== 'shared').length));
  if (n === 0) return Array.from({ length: statusStepCount(order) }, (_, k) => statusState(k, order));
  const seq: StatusFit[] = [];
  for (let j = 0; j <= n; j++) {
    // State 2 + (j - 1) of §8.2 hides the first j items; take its items, unpacked.
    const st = j === 0 ? statusState(0, order) : statusState(1 + j, order);
    seq.push({ ...st, packed: false });
  }
  for (let k = 1 + n; k < statusStepCount(order); k++) seq.push(statusState(k, order));
  return seq;
}

// ── The VTS strip on a VFD ───────────────────────────────────────────────────

/** A unit riding in a 14-segment run: drawn in the sans over its blank cells, never through DSEG. */
export interface SegUnit { at: number; len: number; text: string }
export interface SegRun { cells: string[]; units: SegUnit[] }

/**
 * ★★ Text for the 14-segment strip, UNITS KEPT OUT of the segments (§7 TRAP: "The 14-segment VTS
 * can't [do lower case], so units never go through it"). Every whitelisted unit becomes that many
 * BLANK cells plus a SegUnit saying where to draw it, in its own case, in the sans. Everything else
 * is toSegCells(), one cell per character. Give it CASE-KEPT text (vfdStripText does).
 */
export function toSegRun(text: string): SegRun {
  const parts = text.split(UNIT_RE);
  const cells: string[] = [];
  const units: SegUnit[] = [];
  let carry = '';
  for (let i = 0; i < parts.length; i++) {
    const p = parts[i];
    const prev = parts[i - 1], next = parts[i + 1];
    const unit = i % 2 === 1 && !isLetter(prev ? prev[prev.length - 1] : undefined) && !isLetter(next ? next[0] : undefined);
    if (!unit) { carry += p; continue; }
    if (carry) { cells.push(...segCellList(toSegCells(carry))); carry = ''; }
    /* ★★★ UNITS GO THROUGH THE SEGMENTS, IN CAPITALS — "MHZ", "KHZ", "DB" (Stuart, 2026-10-02, choosing
     *  between this and a printed legend). They were drawn in the app's sans over blank cells, then as a
     *  meshed printed legend; both were a different thing sitting on the display, and both broke the VFD.
     *  A real 14-segment display has no lower case and shows MHZ in its own segments, so this does too.
     *  `units` stays in the type (always empty here) so a caller that still reads it draws nothing. */
    cells.push(...segCellList(toSegCells(p.toUpperCase())));
  }
  if (carry) cells.push(...segCellList(toSegCells(carry)));
  return { cells, units };
}

/**
 * The strip's text for a VFD display: the name (and any secondary line) folded for the display's
 * ROM — the frequency (+ Latin callsign) when a name folds to nothing — upper-cased with the units
 * kept on dot; left in its case on seg, where toSegRun() upper-cases everything, units included.
 * ★ Display only; the notif itself keeps the original text.
 */
export function vfdStripText(name: string, secondary: string | undefined, display: 'dot' | 'seg', freqLabel: string): string {
  const one = (t: string) => {
    const r = displayOrFallback(t, display, freqLabel);
    // seg: displayOrFallback upper-cased it; hand back the case-kept fold so the units survive.
    return display === 'seg' && foldIsUsable(t, foldForSeg(t), 'seg') ? foldToAscii(t) : r;
  };
  const body = secondary ? `${one(name)}  /  ${one(secondary)}` : one(name);
  return display === 'dot' ? toUpperDisplay(body) : body;
}

/** A fixed window of `n` cells over `cells`, starting at `offset`; a short run is centred in WHOLE
 *  cells (a VFD cannot place a character between cells). */
export function cellWindow<T>(cells: T[], n: number, offset: number, blank: T): T[] {
  if (n <= 0) return [];
  if (cells.length <= n) {
    const left = cellWindowLeft(cells.length, n);
    const out = new Array<T>(n).fill(blank);
    for (let i = 0; i < cells.length; i++) out[left + i] = cells[i];
    return out;
  }
  const o = Math.max(0, Math.min(offset, cells.length - n));
  return cells.slice(o, o + n);
}

/** Cells of left padding when a short run is centred (unit overlays follow it). */
export function cellWindowLeft(count: number, n: number): number {
  return count <= n ? Math.floor((n - count) / 2) : 0;
}

/**
 * ★★ STEPPED, never smooth (§7, ref vfd-scroll.gif): the scroll offset after `ms` of scrolling a run
 * of `count` cells through a window of `n`. A pause at the start, then one WHOLE cell per step — no
 * easing, no sub-cell offset. `loop`: pause at the end too, then jump back to the start.
 */
export const VFD_STEP_MS = 300;
export const VFD_PAUSE_MS = 1500;
export function steppedOffset(ms: number, count: number, n: number, loop: boolean): number {
  const travel = count - n;
  if (travel <= 0) return 0;
  const run = VFD_PAUSE_MS + travel * VFD_STEP_MS;
  let t = ms;
  if (loop) t = ms % (run + VFD_PAUSE_MS);
  if (t < VFD_PAUSE_MS) return 0;
  return Math.min(travel, Math.floor((t - VFD_PAUSE_MS) / VFD_STEP_MS) + 1);
}

/**
 * ★★★ THE VCR FREQUENCY READOUT'S CELLS (Stuart, 2026-10-02: "about 3 extra 0's that are a bit distracting
 * … centre the display as 104.200 and have the faded VFD numbers on either side that light up when in
 * use"). Returns the text SegDigits draws, where a SPACE is a cell left unlit (its ghost segments show).
 *   MHz  104.200000 → "   104.200   "     the Hz digits stay dark until they are in use;
 *        104.200500 → "   104.2005  "     …and light from the left as they are;
 *   kHz  1250.000   → "   1250   "        Hz digits dark, and the point with them;
 *        7074.500   → "   7074.5  "
 * ★ FIXED CELLS. Lighting a digit never moves one: the Hz digits sit in cells that were always there.
 *   The equal run of dark cells on the LEFT is what keeps the always-lit core (MHz to the kHz digit, or
 *   the whole kHz figure) CENTRED in the window — the readout only re-centres when the count of MHz/kHz
 *   digits changes (99.9 → 100.0), as it always has.
 * ★ Anything else (Hz unit, no decimal point, letters) is returned untouched.
 */
export function vfdFreqCells(text: string, unit: string): string {
  return vfdFreqLayout(text, unit).text;
}

/** vfdFreqCells plus how many CELLS at each end are drawn small: the Hz digits (lit or dark) and the
 *  matching dark cells on the left. 0/0 when the text was returned untouched. */
export function vfdFreqLayout(text: string, unit: string): { text: string; smallLead: number; smallTail: number } {
  const u = unit.toLowerCase();
  const m = /^(\d+)\.(\d+)$/.exec(text.replace(/,/g, '').trim());
  if (!m || (u !== 'mhz' && u !== 'khz')) return { text, smallLead: 0, smallTail: 0 };
  const [, int, frac] = m;
  const always = u === 'mhz' ? Math.min(3, frac.length) : 0;   // MHz keeps its kHz digits lit
  let used = always;
  for (let i = frac.length - 1; i >= always; i--) if (frac[i] !== '0') { used = i + 1; break; }
  const dark = frac.length - always;                             // the Hz cells, lit or not
  const lead = ' '.repeat(dark);
  const tail = ' '.repeat(frac.length - used);
  const out = used > 0 ? `${lead}${int}.${frac.slice(0, used)}${tail}` : `${lead}${int}${tail}`;
  return { text: out, smallLead: dark, smallTail: dark };
}
