/**
 * mapChrome.ts — ★★★ beta97 THE APP'S MAP CONTROL CARDS: where everything goes, in pure functions (the app port of the
 * web's TrackView layout — docs/v12/MAP-CONTROL-CARDS.md "What is where", "Short landscape", MAP-TEMPLATE.md rule 6).
 * src/utils/mapCards.ts stays the shared rules (card ids, limits, FIT trim, HELP words); this file is what only the app
 * needs: geometry from the WINDOW and the app's REAL safe-area insets (react-native-safe-area-context, measured by the
 * parent — never a device check, never an orientation guess: memory layout_follows_window_not_device), the LIVE badge
 * rule, the RATE chip's source pick, the gain row per driver (AGENTS.md: a control that only works on one radio is not
 * drawn on the others), the mini waterfall's frame budget and the app's spectrum rule under a map.
 *
 *  ★ Tall windows: the red ✕ is the top-left corner piece, RATE the top-right; SERVERS · TUNE · LAYERS · STATS down the
 *    left under the ✕, and MAP too when the stack fits (`oneSide`), else MAP on the right under CHAT; HEALTH and TIME
 *    (the app's EdgeChip pair) under the right column; LIST bottom-left; ? + scale bottom-right; the pill top-centre.
 *  ★ Short landscape (map height < SHORT_MAP_H and wider than tall): the map edge to edge, ALL chrome inside the safe
 *    rectangle; ONE top row hanging from the top edge — ✕ first, then SERVERS, TUNE, LAYERS, STATS, MAP … CHAT, HEALTH,
 *    TIME, RATE last; cards drop down; LIST / waterfall a left side panel under the row; the pill at the bottom centre.
 * Pure — no React — so scripts/test_map_chrome.ts proves it.
 */
import { CARD_NARROW_W, oneSide as oneSideRule, shortMap } from './mapCards';

export interface Insets { top: number; bottom: number; left: number; right: number }
export interface Box { x: number; y: number; w: number; h: number }

/** The chips the chrome can hold, in the short top row's order (✕ first, RATE last; the gap falls after MAP). */
export const CHIP_ORDER = ['exit', 'servers', 'tune', 'layers', 'stats', 'map', 'chat', 'health', 'time', 'rate'] as const;
export type ChipId = typeof CHIP_ORDER[number];
/** The left family (the gap in the short row falls after it). */
export const LEFT_FAMILY: ReadonlySet<ChipId> = new Set<ChipId>(['exit', 'servers', 'tune', 'layers', 'stats', 'map']);

/** A chip's box (pt). The corner pieces (✕, RATE) are bigger and grow by the inset they pad. */
export const CHIP = 44;
export const CORNER = 52;
export const GAP = 8;
/** The short landscape row's tabs: one height, one width (the web's --trk-tab-h / --trk-tab-w). */
export const TAB_H = 40;
export const TAB_W = 46;
/** A list row's height (pt) — the drawer's height is whole rows. */
export const ROW_H = 44;
export const LIST_MIN_ROWS = 3;
export const LIST_MAX_ROWS = 7;
/** The drawer's normal height is this share of the window, in whole rows (the web's 22 %). */
export const LIST_FRAC = 0.22;
/** The collapsed LIST chip's height. */
export const LIST_CHIP_H = 40;
/** The shared-tuner pill's height. */
export const PILL_H = 30;

/** The LIST drawer's width: a phone's full width, else clamp(420, 30 % of the window, 640) — the web's rule. */
export function listWidthFor(w: number): number {
  if (w < CARD_NARROW_W) return Math.max(0, Math.round(w));
  return Math.round(Math.max(420, Math.min(640, w * 0.3)));
}
/** The short-landscape side panel's width: 300–360 pt, about a third of the safe width. */
export function sidePanelWidthFor(safeW: number): number { return Math.round(Math.max(300, Math.min(360, safeW * 0.36))); }
/** The drawer's normal height in whole rows: 22 % of the window, 3–7. */
export function listRowsFor(h: number): number {
  return Math.max(LIST_MIN_ROWS, Math.min(LIST_MAX_ROWS, Math.floor((h * LIST_FRAC) / ROW_H)));
}

export interface ChromeIn {
  w: number;
  h: number;
  insets: Insets;
  /** The chips offered now (a card with nothing in it is not offered — MAP-TEMPLATE rule 7). ✕ is always there. */
  chips: ChipId[];
  /** LAYERS' width with every family's count, and compact (one count). Default: TAB_W both. */
  layersW?: number;
  layersCompactW?: number;
  /** RATE's width (its words). Default: TAB_W + 30. */
  rateW?: number;
  /** HEALTH's measured height in the tall layout (the EdgeChip pair: TIME sits under it). */
  healthH?: number;
}
export interface ChromeGeom {
  short: boolean;
  /** Tall layout: every chip down the left (MAP included). */
  oneSide: boolean;
  /** Short layout: the row ran out of room, so LAYERS shows one count. */
  famCompact: boolean;
  /** The safe rectangle (screen coordinates): x from l to r, y from t to b. */
  safe: { l: number; r: number; t: number; b: number };
  /** Each offered chip's box (screen coordinates). */
  boxes: Partial<Record<ChipId, Box>>;
  /** Where each chip's card opens: beside it on the left / right edge (L / R), or dropping from the row (D). */
  cardFrom: Partial<Record<ChipId, 'L' | 'R' | 'D'>>;
  /** The LIST drawer: left edge, bottom distance, width, normal rows, the tallest it may grow (maximised), and whether it is
   *  the short layout's side panel (top = under the row). */
  list: { x: number; bottom: number; w: number; rows: number; maxH: number; side: boolean; top: number };
  /** The pill: its centre band (x, w) and its top y. */
  pill: { x: number; w: number; y: number; at: 'top' | 'bottom' };
  /** The bottom-right group (? scale [− +] (i)): distance from the right and the bottom screen edges. */
  br: { right: number; bottom: number; horizontal: boolean };
  /** The map's own zoom control top (tall layout: under the right column). */
  zoomTop: number;
}

const finite = (v: number, d = 0) => (Number.isFinite(v) ? v : d);

/** ★★★ THE LAYOUT, from the window and the real insets. */
export function chromeGeom(p: ChromeIn): ChromeGeom {
  const w = Math.max(0, finite(p.w)), h = Math.max(0, finite(p.h));
  const ins = { top: Math.max(0, finite(p.insets.top)), bottom: Math.max(0, finite(p.insets.bottom)),
                left: Math.max(0, finite(p.insets.left)), right: Math.max(0, finite(p.insets.right)) };
  const offered = new Set<ChipId>(['exit', ...p.chips]);
  const has = (id: ChipId) => offered.has(id);
  const short = shortMap(w, h);
  const safe = { l: ins.left, r: w - ins.right, t: short ? 0 : ins.top, b: h - ins.bottom };
  const rateW = Math.max(TAB_W, finite(p.rateW ?? TAB_W + 30, TAB_W + 30));
  const boxes: ChromeGeom['boxes'] = {};
  const cardFrom: ChromeGeom['cardFrom'] = {};

  if (short) {
    // ── ONE top row hanging from the top edge, inside the safe rectangle; ✕ first, RATE last ──
    const wOf = (id: ChipId, compact: boolean) => id === 'layers' ? Math.max(TAB_W, finite((compact ? p.layersCompactW : p.layersW) ?? TAB_W, TAB_W))
      : id === 'rate' ? rateW : TAB_W;
    const lay = (compact: boolean) => {
      const out: ChromeGeom['boxes'] = {};
      let x = safe.l;
      for (const id of CHIP_ORDER) if (has(id) && LEFT_FAMILY.has(id)) { const bw = wOf(id, compact); out[id] = { x, y: 0, w: bw, h: TAB_H }; x += bw + GAP; }
      const leftEnd = x - GAP;
      let r = safe.r;
      for (const id of [...CHIP_ORDER].reverse()) if (has(id) && !LEFT_FAMILY.has(id)) { const bw = wOf(id, compact); r -= bw; out[id] = { x: r, y: 0, w: bw, h: TAB_H }; r -= GAP; }
      return { out, gap: r + GAP - leftEnd };
    };
    let famCompact = false;
    let row = lay(false);
    if (row.gap < GAP) { famCompact = true; row = lay(true); }
    Object.assign(boxes, row.out);
    for (const id of Object.keys(boxes) as ChipId[]) cardFrom[id] = 'D';
    const top = TAB_H + GAP;
    const listW = Math.min(sidePanelWidthFor(safe.r - safe.l), Math.max(0, safe.r - safe.l));
    const listMaxH = Math.max(LIST_CHIP_H, safe.b - top - GAP);
    // the bottom-right group: from the safe right, above the bottom inset; the pill between the panel and the group
    const brW = 150;
    const pillL = safe.l + listW + GAP, pillR = safe.r - brW - GAP;
    const pw = Math.max(0, pillR - pillL);
    return {
      short, oneSide: false, famCompact, safe, boxes, cardFrom,
      list: { x: safe.l, bottom: ins.bottom + GAP, w: listW, rows: listRowsFor(h), maxH: listMaxH, side: true, top },
      pill: { x: pillL, w: pw, y: safe.b - PILL_H - 6, at: 'bottom' },
      br: { right: ins.right + GAP, bottom: ins.bottom + 6, horizontal: true },
      zoomTop: top,
    };
  }

  // ── TALL: the corner pieces on the physical edges, padding their content in by the inset ──
  boxes.exit = { x: 0, y: 0, w: CORNER + ins.left, h: CORNER + ins.top };
  if (has('rate')) boxes.rate = { x: w - rateW - ins.right, y: 0, w: rateW + ins.right, h: CORNER + ins.top };
  const leftIds: ChipId[] = (['servers', 'tune', 'layers', 'stats'] as ChipId[]).filter(has);
  // the whole stack MAP would join: the ✕, the chips, the LIST chip, their gaps
  const stackH = boxes.exit.h + GAP + (leftIds.length + 1) * (CHIP + GAP) + LIST_CHIP_H;
  const one = has('map') && oneSideRule(h, stackH);
  if (one) leftIds.push('map');
  let y = boxes.exit.h + GAP;
  for (const id of leftIds) { boxes[id] = { x: 0, y, w: CHIP + ins.left, h: CHIP }; cardFrom[id] = 'L'; y += CHIP + GAP; }
  const leftFoot = y;
  // the right column under RATE (or under the top inset when no RATE): CHAT, then MAP when flanking; then HEALTH, TIME
  let ry = (boxes.rate ? boxes.rate.h : ins.top) + GAP;
  for (const id of (['chat', ...(one ? [] : ['map'])] as ChipId[]).filter(has)) { boxes[id] = { x: w - CHIP - ins.right, y: ry, w: CHIP + ins.right, h: CHIP }; cardFrom[id] = 'R'; ry += CHIP + GAP; }
  if (has('health')) { const hh = Math.max(CHIP, finite(p.healthH ?? CHIP, CHIP)); boxes.health = { x: w - CHIP - ins.right, y: ry, w: CHIP + ins.right, h: hh }; cardFrom.health = 'R'; ry += hh + GAP; }
  if (has('time')) { boxes.time = { x: w - CHIP - ins.right, y: ry, w: CHIP + ins.right, h: CHIP }; cardFrom.time = 'R'; ry += CHIP + GAP; }
  if (boxes.rate) cardFrom.rate = 'R';
  cardFrom.exit = 'L';
  const exitR = boxes.exit.w + GAP, rateL = boxes.rate ? boxes.rate.x - GAP : w - ins.right - GAP;
  return {
    short, oneSide: one, famCompact: false, safe, boxes, cardFrom,
    list: { x: 0, bottom: 0, w: listWidthFor(w), rows: listRowsFor(h), maxH: Math.max(LIST_CHIP_H, h - leftFoot - GAP), side: false, top: 0 },
    pill: { x: exitR, w: Math.max(0, rateL - exitR), y: ins.top + 8, at: 'top' },
    br: { right: ins.right + GAP, bottom: ins.bottom + GAP, horizontal: false },
    zoomTop: ry,
  };
}

/** Where a transient card goes: beside its chip (tall) or dropped from the row (short), inside the window, with the most
 *  height it may take. A phone-width window gives it the width left beside the chip. */
export const CARD_MAX_W = 340;
export function cardBox(g: ChromeGeom, id: ChipId, w: number, h: number): { x: number; y: number; w: number; maxH: number } {
  const b = g.boxes[id];
  if (!b) return { x: GAP, y: GAP, w: Math.min(CARD_MAX_W, w - 2 * GAP), maxH: h - 2 * GAP };
  const from = g.cardFrom[id] ?? 'L';
  if (from === 'D') {
    const cw = Math.min(CARD_MAX_W, g.safe.r - g.safe.l - 2 * GAP);
    const x = Math.max(g.safe.l, Math.min(b.x, g.safe.r - cw));
    const y = b.y + b.h + GAP;
    return { x, y, w: cw, maxH: Math.max(80, g.safe.b - y - GAP) };
  }
  const y = b.y;
  const maxH = Math.max(80, g.safe.b - y - GAP - (g.list.side ? 0 : LIST_CHIP_H + GAP));
  if (from === 'L') {
    const x = b.x + b.w + GAP;
    return { x, y, w: Math.max(160, Math.min(CARD_MAX_W, w - x - GAP)), maxH };
  }
  const right = w - b.x + GAP;
  const cw = Math.max(160, Math.min(CARD_MAX_W, w - right - GAP));
  return { x: w - right - cw, y, w: cw, maxH };
}

/** ★★ THE LIVE BADGES in LAYERS (Stuart 15:31 / 15:33, the web's setToggles): filled LIVE = THIS radio decodes it;
 *  outlined = another radio or feed of this server does (its name when one, ≤ 5 letters, else LIVE); none = stored.
 *  Shown ONLY when the rows differ — all alike, there is nothing to tell apart. */
export type LiveBadge = null | { kind: 'mine' | 'other'; text: string; words: string };
export function liveBadges(rows: Array<{ live?: boolean; others?: string[] }>): LiveBadge[] {
  const stateOf = (x: { live?: boolean; others?: string[] }) => (x.live ? 'mine' : x.others?.length ? 'other' : 'stored');
  const mixed = new Set(rows.map(stateOf)).size > 1;
  return rows.map((x) => {
    if (!mixed) return null;
    if (x.live) return { kind: 'mine', text: 'LIVE', words: 'This radio is decoding it now' };
    if (!x.others?.length) return null;
    const one = x.others.length === 1 && x.others[0].length <= 5 ? x.others[0].toUpperCase() : 'LIVE';
    return { kind: 'other', text: one, words: `Live on ${x.others.join(', ')} — another of this server’s sources` };
  });
}

/** ★★★ THE RATE CHIP (Stuart 12:52 / 13:00 / 15:40): this radio's live mode first (marked yours), then each feed with its
 *  own messages a second — never a count; a source without a rate is left out. A viewer (nothing of its own) sees the
 *  busiest first. Hidden when nothing is live. */
export interface RateRow { name: string; rate: string; live: boolean; words?: string; mine?: boolean }
export interface RateIn { mine?: { name: string; perMin?: number; perSec?: number; words?: string } | null;
                          feeds?: Array<{ title: string; label?: string; state?: string; perSec?: number | null }> }
const LIVE_STATE = /connected|listening|live|ok/i;
export function fmtRate(n: number): string { return n >= 100 ? Math.round(n).toLocaleString('en-GB') : n.toFixed(n < 10 ? 1 : 0); }
export function rateRows(x: RateIn): RateRow[] {
  const out: RateRow[] = [];
  const m = x.mine;
  if (m && (typeof m.perSec === 'number' || typeof m.perMin === 'number')) {
    const rate = typeof m.perSec === 'number' ? `${fmtRate(m.perSec)}/s` : `${fmtRate(m.perMin as number)}/min`;
    out.push({ name: m.name, rate, live: true, mine: true, words: m.words });
  }
  const feeds: RateRow[] = [];
  for (const f of x.feeds ?? []) if (typeof f.perSec === 'number' && Number.isFinite(f.perSec)) {
    feeds.push({ name: f.title, rate: `${fmtRate(f.perSec)}/s`, live: LIVE_STATE.test(f.state ?? ''),
                 words: `${(f.label || 'adsb').toUpperCase()} feed · ${f.state || 'unknown'} · messages a second` });
  }
  if (!out.length) feeds.sort((a, b) => (parseFloat(b.rate.replace(/,/g, '')) || 0) - (parseFloat(a.rate.replace(/,/g, '')) || 0));
  return [...out, ...feeds];
}
/** What the chip shows: nothing (no live source), one reading (static — no card), or the first + "+N" ⌄ (a card). */
export function rateChip(rows: RateRow[]): { show: boolean; text: string; more: number } {
  if (!rows.some((r) => r.live)) return { show: false, text: '', more: 0 };
  const first = rows[0];
  return { show: true, text: `${first.name} ${first.rate}`.trim(), more: rows.length - 1 };
}

/** ★★★ THE ONE-ROW GAIN (Stuart 15:35, the web's gainDriver / mapGainRender), per DRIVER (AGENTS.md: never a control that
 *  only works on one radio): an RTL's gain list + its AUTO; an RSP's IF gain reduction + its IF AGC; the HF+'s attenuator
 *  + its AGC (no variable gain); anything else no row — only ⌄ for the radio's own controls. Locked by the owner: 🔒 and
 *  the inline admin password (no slider). */
export interface GainIn {
  driver: string;
  /** RTL: the gain list (tenths of dB), the chosen one, AUTO, the live gain AUTO chose (−1 unknown). */
  gains?: readonly number[]; gainTenthDb?: number; autoGain?: boolean; liveTenthDb?: number;
  /** RSP: IF gain reduction (dB), its IF AGC, the system gain words. */
  rspIfGr?: number; rspIfAgc?: boolean; rspSys?: number; rspGrMin?: number; rspGrMax?: number;
  /** HF+: the attenuator step, its AGC, the step size and count. */
  ahfAtt?: number; ahfAgc?: boolean; attStepDb?: number; attSteps?: number;
  /** The owner fixed the gain, and whether this listener holds the admin password. */
  locked?: boolean; admin?: boolean;
  /** The radio has more controls (the hardware panel) — ⌄. */
  more?: boolean;
}
export type GainRow =
  | { kind: 'none'; more: boolean }
  | { kind: 'auto' | 'manual' | 'locked'; driver: 'rtl' | 'sdrplay' | 'airspyhf'; name: string; value: string;
      min: number; max: number; step: number; at: number; more: boolean; unlocked: boolean };
export function gainRowFor(g: GainIn): GainRow {
  const more = !!g.more;
  const lockedNow = !!g.locked && !g.admin;
  const unlocked = !!g.locked && !!g.admin;
  if (g.driver === 'rtl' && g.gains && g.gains.length) {
    const gains = g.gains;
    const idx = Math.max(0, gains.reduce((bi, v, i) => (Math.abs(v - (g.gainTenthDb ?? 0)) < Math.abs(gains[bi] - (g.gainTenthDb ?? 0)) ? i : bi), 0));
    const t = g.autoGain && (g.liveTenthDb ?? -1) >= 0 ? g.liveTenthDb! : gains[idx] ?? 0;
    const kind = lockedNow ? 'locked' : g.autoGain ? 'auto' : 'manual';
    return { kind, driver: 'rtl', name: 'AUTO', value: `${(t / 10).toFixed(1)} dB`, min: 0, max: gains.length - 1, step: 1, at: idx, more, unlocked };
  }
  if (g.driver === 'sdrplay' && typeof g.rspIfGr === 'number') {
    const lo = g.rspGrMin ?? 20, hi = g.rspGrMax ?? 59;
    const kind = lockedNow ? 'locked' : g.rspIfAgc !== false ? 'auto' : 'manual';
    const value = typeof g.rspSys === 'number' && g.rspSys > 0 ? `${g.rspSys.toFixed(0)} dB` : `GR ${Math.round(g.rspIfGr)} dB`;
    return { kind, driver: 'sdrplay', name: 'IF AGC', value, min: lo, max: hi, step: 1, at: Math.max(lo, Math.min(hi, Math.round(g.rspIfGr))), more, unlocked };
  }
  if (g.driver === 'airspyhf' && typeof g.ahfAtt === 'number') {
    const step = g.attStepDb ?? 6, n = g.attSteps ?? 9;
    const kind = lockedNow ? 'locked' : g.ahfAgc !== false ? 'auto' : 'manual';
    return { kind, driver: 'airspyhf', name: 'AGC', value: g.ahfAgc !== false ? 'ATT by AGC' : `ATT ${Math.round(g.ahfAtt) * step} dB`,
             min: 0, max: Math.max(0, n - 1), step: 1, at: Math.max(0, Math.min(n - 1, Math.round(g.ahfAtt))), more, unlocked };
  }
  return { kind: 'none', more };
}

/** ★★★ THE APP'S SPECTRUM UNDER A MAP (MAP-TEMPLATE rule 4, the app half): the NON-live viewer over the radio keeps the
 *  spectrum flowing (the waterfall's history keeps ingesting, as today); a LIVE map mode stops it — except while the mini
 *  waterfall drawer is on screen. Not over a map: on. */
export function mapSpecWanted(x: { mapUp: boolean; live: boolean; wfOpen: boolean }): boolean {
  if (!x.mapUp) return true;
  if (!x.live) return true;
  return x.wfOpen;
}

/** The mini waterfall's budget (the web's mapWf: light — DPR 1, ≈ 1 column per point, ≤ 10 rows a second; a history of the
 *  screen's longer side, ≤ 1600 rows). */
export const MWF_MAX_FPS = 10;
export const MWF_MAX_ROWS = 1600;
export const MWF_MAX_COLS = 1024;
export function mwfDims(drawerW: number, screenW: number, screenH: number): { cols: number; rows: number } {
  return { cols: Math.max(32, Math.min(MWF_MAX_COLS, Math.round(drawerW))), rows: Math.max(64, Math.min(MWF_MAX_ROWS, Math.round(Math.max(screenW, screenH)))) };
}
/** May a row be drawn now? (≤ MWF_MAX_FPS) */
export function mwfDue(lastMs: number, nowMs: number): boolean { return nowMs - lastMs >= 1000 / MWF_MAX_FPS - 1; }
/** Crop a frame's bins (centre / span Hz) to [lo, hi] and resample to `cols` — the max of the bins under each column
 *  (a narrow carrier never vanishes between two columns). Bins outside the frame read as NaN (drawn as the floor). */
export function mwfCrop(bins: ArrayLike<number>, centreHz: number, spanHz: number, lo: number, hi: number, cols: number): Float32Array {
  const out = new Float32Array(cols).fill(NaN);
  const n = bins.length;
  if (!n || !(spanHz > 0) || !(hi > lo)) return out;
  const f0 = centreHz - spanHz / 2, hzPer = spanHz / n;
  for (let c = 0; c < cols; c++) {
    const a = lo + ((hi - lo) * c) / cols, b = lo + ((hi - lo) * (c + 1)) / cols;
    let i0 = Math.floor((a - f0) / hzPer), i1 = Math.ceil((b - f0) / hzPer);
    if (i1 <= 0 || i0 >= n) continue;
    i0 = Math.max(0, i0); i1 = Math.min(n, Math.max(i0 + 1, i1));
    let m = -Infinity;
    for (let i = i0; i < i1; i++) if (bins[i] > m) m = bins[i];
    out[c] = m;
  }
  return out;
}
/** A marker's x (0…1 of the drawer) — null when it falls outside the span. */
export function mwfX(hz: number, lo: number, hi: number): number | null { return hi > lo && hz >= lo && hz <= hi ? (hz - lo) / (hi - lo) : null; }

/** ★★ THE SHARED-TUNER PILL'S WORDS for the room it has: the whole line, else the short form "Ask · 3/4" — never an
 *  ellipsis through the count (Stuart). `maxChars` is what fits (≈ the pill's width / 6.5 pt). */
export function pillWords(text: string, maxChars: number): string {
  const t = String(text || '').trim();
  if (t.length <= maxChars) return t;
  const m = /(\(You\+\d+\)|\d+\/\d+|\d+)\s*$/.exec(t);
  const count = m ? m[1] : '';
  const locked = /ask|wait|locked/i.test(t);
  const free = /free/i.test(t);
  const head = free ? 'Free' : locked ? 'Ask' : 'Shared';
  const short = count ? `${head} · ${count}` : head;
  return short.length <= maxChars || !count ? short : count;
}

/** The credits' (i) folds itself away this long after the map opens, or at the first touch (Stuart). */
export const CREDITS_FOLD_MS = 5000;
/** The ask-lock pulse on the pill and CHAT: this long, then it stops (2–3 s). */
export const ASK_PULSE_MS = 2600;

/** ★ The pad's reading order across the chrome (controller): the top row / family, then the open card, then LIST's rows,
 *  then the bottom-right group. Only what is offered. */
export function padOrder(g: ChromeGeom, opts: { cardOpen: boolean; listRows: number; br: boolean }): string[] {
  const chips = (Object.keys(g.boxes) as ChipId[]).sort((a, b) => {
    if (g.short) return (g.boxes[a]!.x - g.boxes[b]!.x);
    const side = (id: ChipId) => (g.cardFrom[id] === 'R' || id === 'rate' ? 1 : 0);
    return side(a) - side(b) || g.boxes[a]!.y - g.boxes[b]!.y;
  });
  return [...chips.map((c) => `chip:${c}`), ...(opts.cardOpen ? ['card'] : []), 'list', ...Array.from({ length: Math.max(0, opts.listRows) }, (_, i) => `row:${i}`), ...(opts.br ? ['help'] : [])];
}
