/**
 * dabBlockView.ts — DAB BLOCK LABELS AND THE BLOCK SWAPPER on a wide spectrum. ONE pure module, shared by the app
 * (WaterfallView.tsx, fed by SDRScreen) and the web client (web/client/src/waterfall.ts, fed by main.ts). Tested by
 * scripts/test_dab_block_view.ts.
 *
 * ★★★ STUART'S BRIEF (2026-11-15, the Almere USRP B210 capturing 56 MS/s — all of Band III on one screen, DAB on 7D):
 *     "For DAB on a super wide radio like this add the spectrum labels to show the blocks and, if in the memory, show the
 *     MUX name next to it — DAB 7D – MTVNL, DAB 8A – 8A regio Alphen. And allow clicking the waterfall to tune to the
 *     specific DAB block (no option to unlock waterfall click from step rate; this is simply a block swapper without
 *     having to walk up all blocks). Maybe on radios with sample rates of 4 MS/s and above which can show 2 or more
 *     multiplexes side by side."
 *
 *  ★ WHEN: in DAB, and only while the span ACTUALLY SHOWN holds two whole blocks side by side (DAB_WIDE_MIN_SPAN_HZ) —
 *    so a narrow radio never changes, and zooming in on a wide one drops the labels and the swapper together.
 *  ★ NAMES: only what the receiver has told us — the live ensemble's own label for the block it is on, the server's
 *    block memory (DabState.blocks), the last band scan's list (MULTIPLEXES), a DAB bookmark that carries an ensemble
 *    name. Never invented: a block nobody has named is plain "DAB 5A".
 *  ★ THE CLICK: inside a block's bandwidth (±DAB_BLOCK_HALF_HZ of its centre) = go to that multiplex, by the SAME
 *    function the MULTIPLEXES list calls (dabGoTo in both clients), so ask-before-tuning, the shared dial, the server's
 *    block change and its service auto-select all apply. Outside every block: nothing — DAB has no dial.
 *  ★ LAYOUT: the labels are KnownSignal rows, laid out by knownSignals.ts labelLayout with everything else on those two
 *    rows (LISTEN, RF CENTRE, the signal labels), so they never overlap. Named blocks outrank bare ones; the block being
 *    received outranks both and is drawn in the tuned colour.
 *  ★★★ EVERY BLOCK, AND THE TUNED ONE ABOVE THEM (Stuart, 2026-11-16, the Almere B210 again: "we are missing all of the DAB
 *    block labels and the known multiplex name for all known MUX's", "raise the VFO coloured labels above so they dont
 *    foul the next block along too"). The ordinary layout's cap (12 labels on 2 rows) dropped most of Band III, and the
 *    LISTEN box on the bottom row hid the block beside the needle. On a wide DAB view dabLabelLayout replaces it: every
 *    block in view on DAB_BLOCK_ROWS rows (bare "DAB 5A" first, so none is ever dropped, then each known name widened in
 *    where it fits), and the VFO-coloured pair — the tuned block's name, then LISTEN — on two rows of their own above.
 */
import { DAB_BLOCKS } from '../services/dabBlocks.ts';
import { signalsInView, LABEL_GAP, LABEL_MAX, type KnownSignal, type PlacedLabel, type BlockedBox } from './knownSignals.ts';

/** Half a DAB ensemble's occupied bandwidth (1.536 MHz / 2). */
export const DAB_BLOCK_HALF_HZ = 768_000;
/** ★ Two whole blocks side by side need one block spacing (1.712 MHz) plus one block's width (1.536 MHz) = 3.248 MHz.
 *  A 3.2 MS/s RTL never reaches it (one multiplex at a time); a 4 MS/s RSP and up does. */
export const DAB_WIDE_MIN_SPAN_HZ = 3_300_000;

/** Collision priority (knownSignals.ts PRIO_*: safety is 7) — the block labels are the reason for the view in DAB. */
export const PRIO_DAB_CURRENT = 10;
export const PRIO_DAB_NAMED = 9;
export const PRIO_DAB_BLOCK = 8;

/** The longest ensemble name drawn beside a block (characters, an ellipsis included). */
export const DAB_MUX_NAME_MAX = 18;

/** True when the span shown can hold two whole multiplexes side by side. */
export function dabWideView(spanHz: number): boolean {
  return Number.isFinite(spanHz) && spanHz >= DAB_WIDE_MIN_SPAN_HZ;
}

/** Where a block's name can come from — each optional, all what the server has said. */
export interface DabNameSources {
  /** The ensemble being received: its block name ("7D") and its own label. Outranks everything. */
  live?: { channel: string; label: string } | null;
  /** The server's block memory (DabState.blocks: block name → ensemble label). */
  memory?: Readonly<Record<string, string>> | null;
  /** The last band scan's hits (dab_scan `found`). */
  scan?: readonly { block: string; ensemble: string }[] | null;
  /** Bookmarks — only a DAB one that carries an ensemble name counts (most carry only the service's). */
  bookmarks?: readonly { frequency: number; mode?: string | null; ensemble?: string | null }[] | null;
}

const clean = (s: unknown) => (typeof s === 'string' ? s.replace(/\s+/g, ' ').trim() : '');

/** Block name → ensemble name, from what the receiver has told us; highest-ranked source wins. */
export function dabMuxNames(src: DabNameSources): Map<string, string> {
  const out = new Map<string, string>();
  const put = (block: string, name: unknown, force = false) => {
    const n = clean(name);
    if (!block || !n) return;
    if (force || !out.has(block)) out.set(block, n);
  };
  // lowest first, each later source overwriting: bookmarks < scan < memory < live
  for (const b of src.bookmarks ?? []) {
    if (String(b.mode ?? '').toLowerCase() !== 'dab') continue;
    const i = DAB_BLOCKS.findIndex((k) => Math.abs(k.hz - b.frequency) < 50_000);
    if (i >= 0) put(DAB_BLOCKS[i].name, b.ensemble, true);
  }
  for (const h of src.scan ?? []) put(h.block, h.ensemble, true);
  for (const [k, v] of Object.entries(src.memory ?? {})) put(k, v, true);
  if (src.live) put(src.live.channel, src.live.label, true);
  return out;
}

/** "DAB 7D — MTVNL", or "DAB 5A" with no name. A long name is shortened with an ellipsis. */
export function dabBlockLabel(block: string, mux?: string): string {
  let n = clean(mux);
  if (n.length > DAB_MUX_NAME_MAX) n = n.slice(0, DAB_MUX_NAME_MAX - 1).trimEnd() + '…';
  return n ? `DAB ${block} — ${n}` : `DAB ${block}`;
}

/** ★★ The tuned box's first line — the needle's frequency and its block: "194.064M: DAB 7D". */
export function dabTunedHead(hz: number, block: string): string {
  return `${(hz / 1e6).toFixed(3)}M: DAB ${block}`;
}

/** The ensemble's name as drawn under its block: whitespace tidied, at most DAB_MUX_NAME_MAX characters (an ellipsis). */
export function dabMuxShort(mux?: string): string {
  const n = clean(mux);
  return n.length > DAB_MUX_NAME_MAX ? n.slice(0, DAB_MUX_NAME_MAX - 1).trimEnd() + '…' : n;
}

/** ★ The fewest characters a shortened name keeps before its ellipsis. At a whole Band III span on a 1190-px window a block
 *  is ~33 px wide — three characters and the ellipsis ("MTV…", "Gel…"); the hint over a block still gives the whole name
 *  (dabBlockHint). Fewer than three says nothing, and the whole name is lifted to a free pair of rows instead. */
export const DAB_MUX_KEEP_MIN = 3;

/** ★★ The name shortened (ellipsis) until its box is no wider than `maxW` (the block's width), keeping at least
 *  DAB_MUX_KEEP_MIN characters; the whole name when it already fits; null when even the shortest will not fit. */
export function dabFitName(name: string, maxW: number, measure: (t: string) => number): string | null {
  if (!name) return null;
  if (measure(name) <= maxW) return name;
  const base = name.endsWith('…') ? name.slice(0, -1) : name;
  for (let n = base.length - 1; n >= DAB_MUX_KEEP_MIN; n--) {
    const head = base.slice(0, n).trimEnd();
    if (head.length < DAB_MUX_KEEP_MIN) break;
    const t = head + '…';
    if (measure(t) <= maxW) return t;
  }
  return null;
}

/** A block label row for the shared layout: which block, and whether it is the one being received. */
export interface DabBlockSignal extends KnownSignal {
  dabIdx: number; dabCurrent: boolean;
  /** The block's bare label ("DAB 8A") — the one-line text for accessibility and hints. */
  bare: string;
  /** ★★ The two-line box's parts (Stuart, 2026-11-16: "taller instead of wider"): the block ("8A") on top, the ensemble's
   *  name ('' = none known) underneath — no "DAB " and no " — " (the band bar already says DAB). */
  block: string; mux: string;
}

/** Every Band III block as a label (sorted by frequency — labelLayout takes the ones in view). `current` = index into
 *  DAB_BLOCKS of the block being received, −1 = none. */
export function dabBlockSignals(names: ReadonlyMap<string, string>, current: number): DabBlockSignal[] {
  return DAB_BLOCKS.map((b, i) => {
    const mux = names.get(b.name);
    return {
      hz: b.hz, lo: b.hz - DAB_BLOCK_HALF_HZ, hi: b.hz + DAB_BLOCK_HALF_HZ,
      label: dabBlockLabel(b.name, mux), dec: '', dialHz: b.hz, mode: 'dab',
      prio: i === current ? PRIO_DAB_CURRENT : mux ? PRIO_DAB_NAMED : PRIO_DAB_BLOCK,
      src: 'dab', dabIdx: i, dabCurrent: i === current, bare: dabBlockLabel(b.name),
      block: b.name, mux: dabMuxShort(mux),
    } as DabBlockSignal;
  });
}

/** True for a label row made by dabBlockSignals. */
export function isDabBlockSignal(s: KnownSignal): s is DabBlockSignal {
  return (s as DabBlockSignal).dabIdx !== undefined;
}

/** The blocks whose centre is in [lo, hi] (indices into DAB_BLOCKS). */
export function dabBlocksInView(lo: number, hi: number): number[] {
  const out: number[] = [];
  for (let i = 0; i < DAB_BLOCKS.length; i++) if (DAB_BLOCKS[i].hz >= lo && DAB_BLOCKS[i].hz <= hi) out.push(i);
  return out;
}

/** The labels to lay out in DAB on this view: the signal labels already in force with every block merged in (sorted),
 *  or the signal labels alone when the view cannot hold two blocks. */
export function withDabBlocks(known: readonly KnownSignal[] | null | undefined, blocks: readonly DabBlockSignal[] | null | undefined,
                              spanHz: number): readonly KnownSignal[] | null {
  if (!blocks || !blocks.length || !dabWideView(spanHz)) return known && known.length ? known : null;
  if (!known || !known.length) return blocks;
  return [...known, ...blocks].sort((a, b) => a.hz - b.hz);
}

/** ★★ THE CLICK: the block a click at `hz` lands in (index into DAB_BLOCKS), or −1 — outside every block, or a view too
 *  narrow to swap blocks on. Where two blocks overlap (10A/10N, 11A/11N, 12A/12N sit 160 kHz apart) the nearer centre. */
export function dabBlockHit(hz: number, spanHz: number): number {
  if (!Number.isFinite(hz) || !dabWideView(spanHz)) return -1;
  let best = -1, bestD = Infinity;
  for (let i = 0; i < DAB_BLOCKS.length; i++) {
    const d = Math.abs(DAB_BLOCKS[i].hz - hz);
    if (d <= DAB_BLOCK_HALF_HZ && d < bestD) { bestD = d; best = i; }
  }
  return best;
}

/** The hover hint on the web: "go to 8A — 8A regio Alphen", "go to 5A", "on 7D — MTVNL" for the block already playing. */
export function dabBlockHint(idx: number, names: ReadonlyMap<string, string>, current: number): string {
  const b = DAB_BLOCKS[idx];
  if (!b) return '';
  const n = clean(names.get(b.name));
  return `${idx === current ? 'on' : 'go to'} ${b.name}${n ? ' — ' + n : ''}`;
}

// ── The wide view's own layout ────────────────────────────────────────────────────────────────────────────────────────

/** Rows the block labels may take on a wide DAB view — as FEW as hold every block and every known name (from 2), at most
 *  DAB_BLOCK_ROWS. At a whole Band III span (56 MHz) a bare "DAB 5A" is a little wider than one block spacing (two rows
 *  hold them all) and a named block about three spacings; six rows hold nearly every name, and one that fits nowhere
 *  stays bare. A narrower view needs fewer rows and so covers less of the spectrum. */
export const DAB_BLOCK_ROWS = 6;
export const DAB_BLOCK_ROWS_MIN = 2;
/** No cap that could drop a block: every Band III block, plus the ordinary cap for the other labels in view. */
export const DAB_LABEL_MAX = DAB_BLOCKS.length + LABEL_MAX;

/** True when a label list is the wide DAB view's (withDabBlocks merged the blocks in). */
export function isDabWideList(list: readonly KnownSignal[] | null | undefined): boolean {
  return !!list && list.some(isDabBlockSignal);
}

/** ★★ A block label's box: CENTRED on the block's centre `x` (so it sits over its own block, not over it and the next one
 *  to the right), moved only as far as it must to stay on screen (0..width). */
export function dabBlockBoxX(x: number, w: number, width: number): number {
  return Math.max(0, Math.min(Math.max(0, width - w), x - w / 2));
}

/** The wide view's layout: the boxes, how many block rows it used, the tuned block's (lowest) row (= rows) and LISTEN's
 *  (above the tuned box: rows + 2 for a named two-line box, rows + 1 for a bare one). */
export interface DabLabelPlan {
  placed: PlacedLabel[]; rows: number; tunedRow: number; liveRow: number;
  /** ★★ The tuned box carries the needle's readout — the caller draws NO separate LISTEN/readout label (false = no block
   *  being received is in view: the caller draws its readout as usual, on `liveRow`). */
  merged: boolean;
}

/**
 * ★★★ THE WIDE DAB VIEW'S LAYOUT (same units and conventions as knownSignals.ts labelLayout; `blocked` boxes on the block
 * rows — RF CENTRE, the DC spike, the dB axis). Tried with DAB_BLOCK_ROWS_MIN rows, then one more at a time up to `maxRows`,
 * until every block in view is placed and every known name fits.
 * ★★ TALLER, NOT WIDER (Stuart, 2026-11-16 — the Almere B210: "DAB 8B — 8B N-H / Flevo" covered the multiplexes beside
 *    it): a named block is a TWO-LINE box (PlacedLabel.span 2, .lines), the block on top and its name underneath, both
 *    centred, CENTRED ON ITS OWN BLOCK and no wider than the block where it can be. In order:
 *   1. the block being received, alone on the row(s) above the block rows, CENTRED ON THE NEEDLE (`dialHz`) and kept on
 *      screen: ONE box with the needle's readout and its whole name ("194.064M: DAB 7D" over "MTVNL") — `merged`, so the
 *      caller draws no separate readout;
 *   2. every other block in view as a one-line box ("8A"), left to right, on the lowest block row it fits — none dropped;
 *   3. each named block (nearest the dial first) grown into the two-line box: its name as it is, else shortened with an
 *      ellipsis to the block's width (dabFitName) — in place (its row and the one above) or on the lowest free pair of
 *      rows; ONLY a name that cannot be shortened to the block's width is drawn whole and wider, on a pair of rows where it
 *      overlaps nothing; where nothing fits the block stays one line;
 *   4. the other labels in view (decoders, bookmarks…) in what is left of the block rows, at most LABEL_MAX of them.
 * With no received block in view the needle's readout is the caller's, on `liveRow`, CENTRED ON THE NEEDLE (dabBlockBoxX).
 */
export function dabLabelLayout(list: readonly KnownSignal[], lo: number, span: number, width: number,
                               measure: (label: string) => number, dialHz: number,
                               blocked: readonly BlockedBox[] = [], pad = 3, gap = LABEL_GAP,
                               maxRows = DAB_BLOCK_ROWS): DabLabelPlan {
  let plan: DabLabelPlan | null = null;
  // ★ never fewer block rows than the highest blocked box needs (the DC spike's label reaches row 3): the tuned pair on
  //   top must clear it too. The dB axis's boxes (x < 30) name rows 0 and 1 only.
  const floor = blocked.reduce((m, b) => Math.max(m, b.row + 1), DAB_BLOCK_ROWS_MIN);
  for (let rows = Math.min(floor, maxRows); rows <= maxRows; rows++) {
    const r = layoutRows(list, lo, span, width, measure, dialHz, blocked, pad, gap, rows);
    plan = { placed: r.placed, rows, tunedRow: rows, liveRow: rows + r.tunedSpan, merged: r.placed.some((p) => isDabBlockSignal(p.sig) && p.sig.dabCurrent) };
    if (r.complete) break;
  }
  return plan ?? { placed: [], rows: 0, tunedRow: 0, liveRow: 1, merged: false };
}

function layoutRows(list: readonly KnownSignal[], lo: number, span: number, width: number,
                    measure: (label: string) => number, dialHz: number,
                    blocked: readonly BlockedBox[], pad: number, gap: number,
                    rows: number): { placed: PlacedLabel[]; complete: boolean; tunedSpan: number } {
  const placed: PlacedLabel[] = [];
  if (!(span > 0 && width > 0)) return { placed, complete: true, tunedSpan: 1 };
  const vis = signalsInView(list, lo, lo + span);
  if (!vis.length) return { placed, complete: true, tunedSpan: 1 };
  let complete = true;
  const d = Number.isFinite(dialHz) ? dialHz : lo + span / 2;
  const taken: Array<Array<[number, number]>> = Array.from({ length: rows + 3 }, () => []);
  for (const b of blocked) if (b.row >= 0 && b.row < rows) taken[b.row].push([b.x0, b.x1]);
  const free = (r: number, x0: number, x1: number) => r >= 0 && r < taken.length && taken[r].every(([a, b]) => x1 + gap <= a || x0 >= b + gap);
  const take = (r: number, x0: number, x1: number) => taken[r].push([x0, x1]);
  const drop = (r: number, x0: number, x1: number) => { const k = taken[r].findIndex(([a, c]) => a === x0 && c === x1); if (k >= 0) taken[r].splice(k, 1); };
  const boxAt = (x: number, w: number) => (x + pad + w > width ? Math.max(0, x - pad - w) : x + pad);
  // ★★ a BLOCK's box is centred on its own block (Stuart, 2026-11-16: the labels "straddle 2 blocks, the actual block and
  //    the one to the right of it") — dabBlockBoxX; every other label keeps the marker-and-box-to-the-right look
  const blockAt = (x: number, w: number) => dabBlockBoxX(x, w, width);
  const xOf = (hz: number) => (hz - lo) / span * width;
  const blockW = (2 * DAB_BLOCK_HALF_HZ) / span * width;   // ★ the block's own width on screen: the box's budget
  const blocks = vis.filter(isDabBlockSignal);
  const current = blocks.find((b) => b.dabCurrent) ?? null;
  const two = (b: DabBlockSignal, name: string, x: number, lx: number, w: number, row: number): PlacedLabel =>
    ({ sig: b, x, lx, w, row, span: 2, lines: [b.block, name] });

  // 1. the tuned block, on its own row(s), centred on the NEEDLE — ONE box: the needle's readout and the ensemble's name
  //    (Stuart, 2026-11-16: "7D" shown once — "194.064M: DAB 7D" over "MTVNL"; the separate readout is not drawn here)
  let tunedSpan = 1;
  if (current) {
    const needleHz = Number.isFinite(dialHz) ? dialHz : current.hz;
    const x = xOf(needleHz), head = dabTunedHead(needleHz, current.block);
    const lines = current.mux ? [head, current.mux] : [head];
    const w = Math.max(...lines.map(measure)), lx = dabBlockBoxX(x, w, width);
    for (let r = 0; r < lines.length; r++) take(rows + r, lx, lx + w);
    placed.push({ sig: current, x, lx, w, row: rows, span: lines.length, lines });
    tunedSpan = lines.length;
  }
  // 2. every other block, one line, left to right
  const slot = new Map<DabBlockSignal, PlacedLabel>();
  for (const b of blocks) {
    if (b === current) continue;
    const x = xOf(b.hz), w = measure(b.block), lx = blockAt(x, w);
    for (let r = 0; r < rows; r++) {
      if (!free(r, lx, lx + w)) continue;
      take(r, lx, lx + w);
      const p: PlacedLabel = { sig: b, x, lx, w, row: r, span: 1, lines: [b.block] };
      placed.push(p); slot.set(b, p);
      break;
    }
    if (!slot.has(b)) complete = false;
  }
  // 3. the names, nearest the dial first: two lines, as narrow as the block
  const named = blocks.filter((b) => b !== current && slot.has(b) && b.mux)
                      .sort((a, b) => Math.abs(a.hz - d) - Math.abs(b.hz - d) || a.hz - b.hz);
  for (const b of named) {
    const p = slot.get(b)!;
    drop(p.row, p.lx, p.lx + p.w);
    const fit = dabFitName(b.mux, blockW, measure);
    const name = fit ?? b.mux;
    const w = Math.max(measure(b.block), measure(name)), lx = blockAt(p.x, w);
    const pairFree = (r: number) => r + 1 < rows && free(r, lx, lx + w) && free(r + 1, lx, lx + w);
    let row = pairFree(p.row) ? p.row : -1;
    for (let r = 0; row < 0 && r + 1 < rows; r++) if (pairFree(r)) row = r;
    if (row < 0) { take(p.row, p.lx, p.lx + p.w); complete = false; continue; }
    take(row, lx, lx + w); take(row + 1, lx, lx + w);
    Object.assign(p, two(b, name, p.x, lx, w, row));
  }
  // 4. everything else in view, in what is left
  const others = vis.filter((s) => !isDabBlockSignal(s))
                    .sort((a, b) => (b.prio - a.prio) || (Math.abs(a.hz - d) - Math.abs(b.hz - d)) || (a.hz - b.hz));
  let n = 0;
  for (const s of others) {
    if (n >= LABEL_MAX) break;
    const x = xOf(s.hz), w = measure(s.label), lx = boxAt(x, w);
    for (let r = 0; r < rows; r++) {
      if (!free(r, lx, lx + w)) continue;
      take(r, lx, lx + w);
      placed.push({ sig: s, x, lx, w, row: r }); n++;
      break;
    }
  }
  return { placed, complete, tunedSpan };
}
