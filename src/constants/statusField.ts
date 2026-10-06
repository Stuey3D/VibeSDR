/**
 * statusField.ts — the controls' STATUS ROW in the Display's own cells, on VCR (14-segment) and DOT (5 × 7 dots)
 * (2026-10-06, Stuart, on the Mac app's VCR skin: "on the VFD font it would be extremely difficult to get the bottom
 * bar in the VFD font with the logos?"). The row was a plain Doto run in every Display; now each of its runs —
 * `17:03 UTC`, `⛛ 18:03 BST`, `SHARED TUNER · FREE TO TUNE`, the bars, `23K/S 10FPS`, `· GAIN ↓44.5DB`, `· IF 1400K
 * AUTO` — is a FIXED-PITCH run of the Display's cells (components/StatusField.tsx draws them through SegField /
 * DotField), with every unlit electrode a ghost.
 *
 * What this file decides — the CELLS of a run (pure: no React, no runtime imports; scripts/test_faceplate_segfield.ts
 * and test_faceplate_dotfield.ts run it under plain Node, the caller hands in the cell converter so the folding rules
 * stay in displayText / dotField):
 *
 *   ★ ONE PITCH. Every character, space and symbol takes one cell (the server mark two), so a run is `n` cells wide
 *     and a reading that changes never moves a cell sideways. A run's cell COUNT can change ("9K/S" → "10K/S"); the
 *     row aligns each run left / centre / right exactly as it did, so it grows away from its anchor.
 *   ★ VCR: CAPITALS (a 14-segment display has no lower case — the KHZ compromise, memory vfd_units_in_segments) —
 *     except the d of the unit dB, the classic VFD lower-case d (SEG_LOWER_D, 2026-10-06: a capital D read as 3). A
 *     colon is the COLON ELECTRODE in the gap between two cells (a clock's, no cell of its own: `17:03` is four
 *     cells); a decimal point is the cell's own point. `/` is DSEG14's own `/` (the two diagonal segments, top-right
 *     to bottom-left). `·` has no segment, so it is a cell with no segments and ONE electrode: the point raised to
 *     the centre line (components/SegField `·`), meshed and ghosted like any other.
 *   ★ DOT: real lower case (`23k/s 10fps`, `auto`) — the raised descenders and the symbols are dotField's
 *     DOT_EXTRA_GLYPHS. `:` and `·` are ordinary cells, as on a 5 × 7 character display.
 *   ★ LOGOS are cells too. In the text they arrive as their symbols (⛛ ⚡ ⚿ 👤 ↑ ↓) or as StatusLogo parts (the bars,
 *     the ✕, the gain-arrow slot). On VCR each is drawn as an ELECTRODE in its cell(s) — split by breaks and meshed,
 *     the RDS mark's treatment (components/StatusField). On DOT most are glyphs in the cell; the server mark, the bars
 *     and the ✕ are marks of their own shape (DOT_NODE / DOT_BARS / DOT_CROSS), ghosted as that shape.
 *   ★ `·` swallows the spaces round it: its cell is mostly air already, and a space each side made it the widest gap
 *     in the row.
 */

/** A logo in a status run. `bars` / `cross` are the connection meter; `arrow` is the gain's slot (dir null = dark). */
export type StatusLogo =
  | { kind: 'node' }
  | { kind: 'bars'; q: 0 | 1 | 2 | 3 }
  | { kind: 'cross' }
  | { kind: 'arrow'; dir: 'up' | 'down' | null }
  | { kind: 'rec' }
  | { kind: 'bolt' }
  | { kind: 'key' }
  | { kind: 'person' };
export type StatusLogoKind = StatusLogo['kind'];
export type StatusPart = string | StatusLogo;

/** Cells each logo takes. The server mark is wider than tall (StatusIcon's three nodes), so two. */
export const STATUS_LOGO_CELLS: Readonly<Record<StatusLogoKind, number>> = {
  node: 2, bars: 1, cross: 1, arrow: 1, rec: 1, bolt: 1, key: 1, person: 1,
};

/** The symbols the status text carries that are drawn as logos. */
const SYMBOLS: Readonly<Record<string, StatusLogo>> = {
  '⛛': { kind: 'node' }, '⚡': { kind: 'bolt' }, '⚿': { kind: 'key' }, '👤': { kind: 'person' }, '✕': { kind: 'cross' },
  '●': { kind: 'rec' }, '↑': { kind: 'arrow', dir: 'up' }, '↓': { kind: 'arrow', dir: 'down' },
};

/** The middle-dot separator. */
export const STATUS_SEP = '·';

/**
 * Status text → parts: symbols become logos (an emoji's variation selector dropped), and the spaces round a `·` go.
 * Leading / trailing spaces go as well — the row's own gaps space the runs.
 */
export function statusParts(text: string): StatusPart[] {
  const out: StatusPart[] = [];
  let run = '';
  const flush = () => { if (run) out.push(run); run = ''; };
  const chars = [...text.trim()].filter(c => c !== '️');
  chars.forEach((c, i) => {
    if (c === ' ' && (chars[i - 1] === STATUS_SEP || chars[i + 1] === STATUS_SEP)) return;
    const logo = SYMBOLS[c];
    if (logo) { flush(); out.push(logo); return; }
    run += c;
  });
  flush();
  return out;
}

/** A run laid out in cells. */
export interface StatusSlots {
  /** What each cell lights ('' = dark). VCR: DSEG14 characters, a trailing '.' = the cell's point, '·' = the raised
   *  point. DOT: a dotField glyph. Logo cells are ''. */
  cells: string[];
  /** Each cell's ghost. VCR: '~' (every segment), '~.' (and its point, where one is lit), '·' (the raised point only),
   *  '' (none: a logo's cell). DOT: '#' (all 35 dots) or '' (a mark's cell). */
  ghost: string[];
  /** VCR: a lit colon electrode in the gap after each of these cells. DOT: always empty. */
  colons: number[];
  /** Each logo and its first cell. On DOT only the marks (node, bars, cross) are listed; the rest are glyph cells. */
  logos: Array<{ at: number; logo: StatusLogo }>;
}

/** Text → cells, one string per cell ('' = blank) — the VCR caller passes displayText's toSegCells rules. */
export type StatusToCells = (text: string) => string[];

/** ★ The LOWER-CASE d of the unit "dB" (2026-10-06, Stuart: "the d in dB can be rendered in lowercase on a VFD
 *  display" — the 14-segment capital D read as a 3, so `GAIN 25.4dB` read "25433"). DSEG14 has no lower case of its
 *  own (its 'd' IS its 'D', measured from the TTF), so the cell lights two of its glyphs at once, the way the readout's
 *  "-1" half-digit does: 'J' (segments b c d e) and '-' (the centre bar, g1 g2) — together the classic VFD d, b c d e g.
 *  ★★ The unit ONLY: a case-sensitive "dB" token (dB, dBFS, dBm, dBf) not glued to a letter in front. "DAB", "BBC",
 *  "D" in any word stay the 14-seg alphabet (memory vfd_seg14_glyphs_are_fine). The B stays the 14-seg B. */
export const SEG_LOWER_D = 'J-';
const DB_UNIT_AT = /^dB(?:FS|m|f)?(?![A-Za-z])/;
const isLetterCh = (c: string | undefined) => !!c && c.toLowerCase() !== c.toUpperCase();
/** Is the 'd' at `i` (a code-point index into `chars`) the d of a dB unit? */
export const isDbUnitD = (chars: readonly string[], i: number) =>
  !isLetterCh(chars[i - 1]) && DB_UNIT_AT.test(chars.slice(i, i + 5).join(''));

/** A run in 14-segment cells (VCR). `toCells` folds and upper-cases (ControlsBar segCells). */
export function statusSegSlots(parts: readonly StatusPart[], toCells: StatusToCells): StatusSlots {
  const s: StatusSlots = { cells: [], ghost: [], colons: [], logos: [] };
  const lastIsText = () => s.cells.length > 0 && (s.ghost[s.ghost.length - 1] === '~');
  for (const p of parts) {
    if (typeof p !== 'string') {
      s.logos.push({ at: s.cells.length, logo: p });
      for (let k = 0; k < STATUS_LOGO_CELLS[p.kind]; k++) { s.cells.push(''); s.ghost.push(''); }
      continue;
    }
    // ★ By code point, indexed, so the dB rule can see its neighbours.
    const chars = [...p];
    for (let i = 0; i < chars.length; i++) {
      const ch = chars[i];
      if (ch === 'd' && isDbUnitD(chars, i)) { s.cells.push(SEG_LOWER_D); s.ghost.push('~'); continue; }
      if (ch === ':') {
        // The clock's colon: the electrode in the gap after the cell before it — no cell of its own.
        if (s.cells.length) { s.colons.push(s.cells.length - 1); continue; }
        s.cells.push(''); s.ghost.push('~');
        continue;
      }
      if (ch === '.') {
        if (lastIsText() && !s.cells[s.cells.length - 1].endsWith('.')) {
          s.cells[s.cells.length - 1] += '.';
          s.ghost[s.ghost.length - 1] = '~.';
        } else { s.cells.push('.'); s.ghost.push('~.'); }
        continue;
      }
      if (ch === STATUS_SEP) { s.cells.push(STATUS_SEP); s.ghost.push(STATUS_SEP); continue; }
      if (ch === ' ') { s.cells.push(''); s.ghost.push('~'); continue; }
      const c = toCells(ch)[0] ?? '';
      s.cells.push(c.replace(/\.$/, '')); s.ghost.push('~');
    }
  }
  return s;
}

/** The DOT glyph each non-mark logo is drawn as (dotField DOT_EXTRA_GLYPHS). */
const DOT_LOGO_GLYPH: Readonly<Partial<Record<StatusLogoKind, string>>> = {
  bolt: '⚡', key: '⚿', rec: '●', person: '👤',
};

/** A run in 5 × 7 cells (DOT). `toCell` is dotField's dotChar (a space → ''). */
export function statusDotSlots(parts: readonly StatusPart[], toCell: (ch: string) => string): StatusSlots {
  const s: StatusSlots = { cells: [], ghost: [], colons: [], logos: [] };
  for (const p of parts) {
    if (typeof p !== 'string') {
      const glyph = p.kind === 'arrow' ? (p.dir === 'up' ? '↑' : p.dir === 'down' ? '↓' : '') : DOT_LOGO_GLYPH[p.kind];
      if (glyph !== undefined) { s.cells.push(glyph); s.ghost.push('#'); continue; }
      s.logos.push({ at: s.cells.length, logo: p });
      for (let k = 0; k < STATUS_LOGO_CELLS[p.kind]; k++) { s.cells.push(''); s.ghost.push(''); }
      continue;
    }
    for (const ch of p) { s.cells.push(toCell(ch)); s.ghost.push('#'); }
  }
  return s;
}

// ── Sizes ────────────────────────────────────────────────────────────────────

/** ★ The 14-segment cell height for a status size, em. Doto's ink is 0.7 em tall; the segments get a little more
 *  (0.75) because their strokes are finer — the same visual weight in the same row height. */
export const STATUS_SEG_EM = 0.75;
/** The cell height (pt) for the status display's size, on a half-point. */
export const statusSegFs = (size: number) => Math.max(5, Math.round(size * STATUS_SEG_EM * 2) / 2);
/** DSEG14's cell, em — copies of modeBox SEG14_ADV / SEG14_PITCH (this file has no runtime imports; the test holds
 *  them equal). */
export const STATUS_SEG_ADV = 0.816;
export const STATUS_SEG_PITCH = STATUS_SEG_ADV + 0.08;   // modeBox SEG14_ADV + SEG14_GAP, the same sum
/** A VCR run's width (pt): n cells at the fixed pitch. */
export const statusSegWidth = (n: number, fs: number) => (n > 0 ? ((n - 1) * STATUS_SEG_PITCH + STATUS_SEG_ADV) * fs : 0);
/** DOT: the dot pitch for the status size — Doto's own (0.1 em), so the cells are dot for dot the Doto text they replace. */
export const statusDotPitch = (size: number) => size * 0.1;
/** DOT: a run's width in dot columns (6 a cell, no trailing dead column). */
export const statusDotCols = (n: number) => (n > 0 ? n * 6 - 1 : 0);
/** A run's width (pt) on either Display. */
export function statusRunWidth(face: 'seg' | 'dot', n: number, size: number): number {
  return face === 'seg' ? statusSegWidth(n, statusSegFs(size)) : statusDotCols(n) * statusDotPitch(size);
}
/** ★ The run's box height: the Doto line it replaces, so switching Display never changes the row's height. */
export const statusLineH = (size: number) => Math.ceil(size * 1.2);

// ── DOT marks (5 × 7 grid units; rows top first, '#' lit) ───────────────────

/** The server mark in dots: three 2 × 2 nodes and their links — eight columns, two cells' room. */
export const DOT_NODE = '##.##.## ##....## .#....#. ..#..#.. ..#..#.. ...##... ...##...';
export const DOT_NODE_W = 8;
/** The connection bars: three one-dot bars, 3 / 5 / 7 tall, each its own mark (lit while i < q). */
export const DOT_BARS: readonly string[] = [
  '..... ..... ..... ..... #.... #.... #....',
  '..... ..... ..#.. ..#.. ..#.. ..#.. ..#..',
  '....# ....# ....# ....# ....# ....# ....#',
];
/** Disconnected: the ✕, in the bars' cell (in the link's red). */
export const DOT_CROSS = '..... #...# .#.#. ..#.. .#.#. #...# .....';

/** A DOT mark: its rows and its first dot column within the run, and whether it is lit. */
export interface StatusDotMark { col: number; rows: string; lit: boolean }

/** The marks a DOT run's logos draw. */
export function statusDotMarks(slots: StatusSlots): StatusDotMark[] {
  const out: StatusDotMark[] = [];
  for (const { at, logo } of slots.logos) {
    const col = at * 6;
    if (logo.kind === 'node') {
      out.push({ col: col + Math.floor((statusDotCols(STATUS_LOGO_CELLS.node) - DOT_NODE_W) / 2), rows: DOT_NODE, lit: true });
    } else if (logo.kind === 'bars') {
      DOT_BARS.forEach((rows, i) => out.push({ col, rows, lit: i < logo.q }));
    } else if (logo.kind === 'cross') {
      out.push({ col, rows: DOT_CROSS, lit: true });
    }
  }
  return out;
}

/** The DSP badges (NR / NB / AN) as one run: VCR frames each tag's cells (StatusField draws the frames); DOT
 *  brackets them, a character display's way. Returns the parts and, for VCR, each tag's [first, last] cell. */
export function statusTags(tags: readonly string[], face: 'seg' | 'dot'): { text: string; frames: Array<[number, number]> } {
  if (face === 'dot') return { text: tags.map(t => `[${t}]`).join(''), frames: [] };
  const frames: Array<[number, number]> = [];
  let at = 0;
  for (const t of tags) { frames.push([at, at + t.length - 1]); at += t.length + 1; }
  return { text: tags.join(' '), frames };
}
