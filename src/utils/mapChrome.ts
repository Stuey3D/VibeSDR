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
/** The LIST drawer's folder tabs (≋ swap, ⌃ maximise): this tall, standing above the drawer's top edge. */
export const FTAB_H = 26;
/** The shared-tuner pill's height. */
export const PILL_H = 30;
/** The short layout's bottom-right row (scale bar ≤ 80 · [− +] 78 · ? 36 · (i) 36 + its margins and gaps): what the pill
 *  keeps clear of. */
export const BR_ROW_W = 258;

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
  /** ★★★ the LAYERS chip in the tall layout: its families stacked, icon over count — it GROWS to hold them (Stuart, build 586:
   *  "the chip doesn't grow to accommodate it"), never smaller than a chip. */
  layersTallW?: number;
  layersTallH?: number;
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
  /** The pill: its centre band (x, w), its top y, its whole height and the inset it pads its words in by. ★★★ ATTACHED as the
   *  web's (coordinator, beta97): tall = flush to the TOP edge (flat top, rounded bottom), short = flush to the BOTTOM edge
   *  (flat bottom, rounded top) — the deck's material, never a floating capsule. */
  pill: { x: number; w: number; y: number; at: 'top' | 'bottom'; h: number; pad: number };
  /** The bottom-right group (? scale [− +] (i)): distance from the right and the bottom screen edges. */
  br: { right: number; bottom: number; horizontal: boolean };
  /** The map's own zoom control top (tall layout: under the right column). */
  zoomTop: number;
}

const finite = (v: number, d = 0) => (Number.isFinite(v) ? v : d);

/** ★★★ beta97 hotfix THE PILL IS ALWAYS EXACTLY CENTRED ON THE WINDOW (Stuart, Mac, 2380 px: "still off centre and on a
 *  notched mac looks especially stupid" — it sat 270 px left). It was SLID to stay inside its band, and the band's right
 *  edge is the RATE corner, whose width follows its words ("VDL2 36/min +2"): a wider RATE pushed the pill left. Now it is
 *  never moved: it NARROWS symmetrically about the centre to the room its nearer corner leaves (and its words shorten to
 *  fit — pillWords' short forms). On a notched Mac / an iPhone island it hangs centred under the notch. */
export function pillBox(band: { x: number; w: number }, wantW: number, winW: number): { left: number; w: number; room: number } {
  const cx = winW / 2;
  const room = Math.max(0, 2 * Math.min(cx - band.x, band.x + band.w - cx));   // the widest centred box inside the band
  const w = Math.max(0, Math.min(wantW, room));
  return { left: cx - w / 2, w, room };
}

/** ★★★ beta97 a map's NOTICE (the feed lost, the time limit, the antenna) in the cards' own layout — never the page's banner
 *  under the pill (Stuart, build 587): its heading from the words (the page's old rule), or null for nothing to say. */
export function feedNotice(w: string | null | undefined): { title: string; text: string } | null {
  const t = String(w ?? '').trim();
  if (!t) return null;
  return { title: /on the map|Map time/.test(t) ? 'TIME LIMIT' : /reconnecting|lost/.test(t) ? 'AIRCRAFT FEED' : 'CHECK THE ANTENNA', text: t };
}

/** ★★★ beta97 hotfix (Stuart, 11-28: a MeshCore node's card stuck UNDER the chips, its × out of reach) THE ROOM FOR A MAP
 *  ITEM'S OWN CARD (the track layer's .vtl-card — AIS, APRS, DSC, ISM / mesh, NAVTEX, sondes, datalink): inside the safe
 *  area, below the top corners (and a top pill), clear of BOTH chip columns, above the LIST and the bottom-right group —
 *  as the aircraft card already sits. Distances in screen points from each window edge; the page reads them as CSS. */
export function itemCardArea(g: ChromeGeom, o: { winW: number; winH: number; insets: Insets; k: number; listH: number; listW: number; listShown: boolean; brTop: number }): { top: number; right: number; bottom: number; left: number } {
  const gap = GAP * o.k;
  const boxes = Object.values(g.boxes).filter((b): b is Box => !!b);
  if (g.short) {
    const top = Math.max(o.insets.top, ...boxes.map((b) => b.y + b.h)) + gap;
    const left = o.listShown && g.list.side ? g.list.x + o.listW + gap : o.insets.left + gap;
    return { top, right: o.insets.right + gap, bottom: Math.max(o.insets.bottom, g.pill.h, o.brTop) + gap, left };
  }
  const leftCol = boxes.filter((b) => b.x < o.winW / 2), rightCol = boxes.filter((b) => b.x >= o.winW / 2);
  const corner = Math.max(o.insets.top, ...boxes.filter((b) => b.y <= 1).map((b) => b.y + b.h), g.pill.at === 'top' ? g.pill.y + g.pill.h : 0);
  const left = Math.max(o.insets.left, ...leftCol.map((b) => b.x + b.w)) + gap;
  const right = Math.max(o.insets.right, ...rightCol.map((b) => o.winW - b.x)) + gap;
  // ★ the LIST at the foot: a full-width drawer, or one under the card's column, holds the card above it
  const listUnder = o.listShown && !g.list.side && (o.listW >= o.winW - right - 40);
  const bottom = Math.max(o.insets.bottom, o.brTop, listUnder ? o.listH : 0) + gap;
  return { top: corner + gap, right, bottom, left };
}

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
    // the bottom-right group: from the safe right, above the bottom inset; the pill between the panel and the group —
    // ★★ the group's REAL width (scale ≤ 80 + zoom 78 + ? 42 + gaps), so the pill never runs over the scale bar
    const brW = BR_ROW_W;
    const pillL = safe.l + listW + GAP, pillR = safe.r - brW - GAP;
    const pw = Math.max(0, pillR - pillL);
    return {
      short, oneSide: false, famCompact, safe, boxes, cardFrom,
      list: { x: safe.l, bottom: ins.bottom + GAP, w: listW, rows: listRowsFor(h), maxH: listMaxH, side: true, top },
      pill: { x: pillL, w: pw, y: h - PILL_H - ins.bottom, at: 'bottom', h: PILL_H + ins.bottom, pad: ins.bottom },
      br: { right: ins.right + GAP, bottom: ins.bottom + 6, horizontal: true },
      zoomTop: top,
    };
  }

  // ── TALL: the corner pieces on the physical edges, padding their content in by the inset ──
  boxes.exit = { x: 0, y: 0, w: CORNER + ins.left, h: CORNER + ins.top };
  if (has('rate')) boxes.rate = { x: w - rateW - ins.right, y: 0, w: rateW + ins.right, h: CORNER + ins.top };
  const leftIds: ChipId[] = (['servers', 'tune', 'layers', 'stats'] as ChipId[]).filter(has);
  // the whole stack MAP would join: the ✕, the chips, the LIST chip, their gaps
  const lyH = Math.max(CHIP, finite(p.layersTallH ?? CHIP, CHIP)), lyW = Math.max(CHIP, finite(p.layersTallW ?? CHIP, CHIP));
  const hOf = (id: ChipId) => (id === 'layers' ? lyH : CHIP);
  const stackH = boxes.exit.h + GAP + (leftIds.length + 1) * (CHIP + GAP) + (leftIds.includes('layers') ? lyH - CHIP : 0) + LIST_CHIP_H;
  const one = has('map') && oneSideRule(h, stackH);
  if (one) leftIds.push('map');
  let y = boxes.exit.h + GAP;
  for (const id of leftIds) { const hh = hOf(id); boxes[id] = { x: 0, y, w: (id === 'layers' ? lyW : CHIP) + ins.left, h: hh }; cardFrom[id] = 'L'; y += hh + GAP; }
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
    /* ★★★ beta97 hotfix (Stuart, TestFlight 589: a tall LIST's swap tab sat on the MAP CONTROLS chip's bottom edge) the drawer
     *  AND its folder tabs (FTAB_H above its top edge) stop below the lowest chip of every column the drawer reaches — the left
     *  one always, the right one where the drawer runs under it — at any list height */
    list: { x: 0, bottom: 0, w: listWidthFor(w), rows: listRowsFor(h), side: false, top: 0,
            maxH: Math.max(LIST_CHIP_H, h - Math.max(leftFoot, listWidthFor(w) > w - (CHIP + ins.right) - GAP ? ry : 0) - GAP - FTAB_H) },
    pill: { x: exitR, w: Math.max(0, rateL - exitR), y: 0, at: 'top', h: PILL_H + ins.top, pad: ins.top },
    br: { right: ins.right + GAP, bottom: ins.bottom + GAP, horizontal: false },
    zoomTop: ry,
  };
}

/** Where a transient card goes: beside its chip (tall) or dropped from the row (short), inside the window, with the most
 *  height it may take. A phone-width window gives it the width left beside the chip. */
export const CARD_MAX_W = 340;
/** ★★ yMin: how high a side card may RISE when its content is taller than the room below its chip (the web's: a long card
 *  slides up beside its chip column rather than opening a short window at the screen's foot) — under the ✕ corner. */
export function cardBox(g: ChromeGeom, id: ChipId, w: number, h: number): { x: number; y: number; w: number; maxH: number; yMin: number } {
  const b = g.boxes[id];
  if (!b) return { x: GAP, y: GAP, w: Math.min(CARD_MAX_W, w - 2 * GAP), maxH: h - 2 * GAP, yMin: GAP };
  const from = g.cardFrom[id] ?? 'L';
  if (from === 'D') {
    const cw = Math.min(CARD_MAX_W, g.safe.r - g.safe.l - 2 * GAP);
    const x = Math.max(g.safe.l, Math.min(b.x, g.safe.r - cw));
    const y = b.y + b.h + GAP;
    return { x, y, w: cw, maxH: Math.max(80, g.safe.b - y - GAP), yMin: y };
  }
  const y = b.y;
  const maxH = Math.max(80, g.safe.b - y - GAP - (g.list.side ? 0 : LIST_CHIP_H + GAP));
  const yMin = Math.min(y, (g.boxes.exit ? g.boxes.exit.y + g.boxes.exit.h : g.safe.t) + GAP);
  if (from === 'L') {
    const x = b.x + b.w + GAP;
    return { x, y, w: Math.max(160, Math.min(CARD_MAX_W, w - x - GAP)), maxH, yMin };
  }
  const right = w - b.x + GAP;
  const cw = Math.max(160, Math.min(CARD_MAX_W, w - right - GAP));
  return { x: w - right - cw, y, w: cw, maxH, yMin: Math.min(y, (g.boxes.rate ? g.boxes.rate.y + g.boxes.rate.h : g.safe.t) + GAP) };
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

/** ★★★ THE ONE-ROW GAIN (Stuart 15:35, the web's gainDriver / mapGainRender), for EVERY driver the app's hardware panel
 *  (LocalHardwarePanel) knows, each with ITS OWN controls (AGENTS.md: never a control built for one radio drawn on another):
 *    rtl       the gain list + AUTO (VibeAGC); a relayed rtl_tcp stream (noHwGain) has no gain → no row
 *    sdrplay   IF gain reduction + its IF AGC
 *    airspyhf  the attenuator + its AGC (no variable gain)
 *    usrp      its gain list + the AD9361's own AGC — the AGC key only where the radio says it has one (hwAgc)
 *    hackrf    the LNA (0–40 dB, 8 dB steps) — NO AGC key (libhackrf has none); VGA and the owner-only amp under ⌄
 *    airspy    the preset curve (0–21, the owner's limit) in SENSITIVE / LINEAR — no AGC key; FREE mode's three stages
 *              and the per-stage AGCs under ⌄ (no row: there is no single gain to move)
 *    icom, anything else: no row — ⌄ only when the radio has controls (the hardware panel).
 *  Locked by the owner: 🔒 and the inline admin password (no slider). `set` says which setter the slider drives. */
export interface GainIn {
  driver: string;
  /** rtl / usrp: the gain list (tenths of dB), the chosen one, AUTO / AGC on, the live gain AUTO chose (−1 unknown). */
  gains?: readonly number[]; gainTenthDb?: number; autoGain?: boolean; liveTenthDb?: number;
  /** rtl: a relayed rtl_tcp stream with no tuner (radio.noHwGain). usrp: the radio has its own AGC (radio.hwAgc). */
  noHwGain?: boolean; hwAgc?: boolean;
  /** sdrplay: IF gain reduction (dB), its IF AGC, the system gain, the owner's floor; ★★★ beta97 the LNA state (the RF
   *  side, shown with the IF on the row) and the NAME of the loop holding the IF while it is automatic — the server's own
   *  report (rspstat vagc → "VibeAGC", else "SDRplay"; vagcIfOwner), never a hardcoded one. */
  rspIfGr?: number; rspIfAgc?: boolean; rspSys?: number; rspGrMin?: number; rspGrMax?: number; rspLna?: number; agcName?: string;
  /** airspyhf: the attenuator step, its AGC, the step size and count. */
  ahfAtt?: number; ahfAgc?: boolean; attStepDb?: number; attSteps?: number;
  /** hackrf: the LNA (dB). */
  hrfLna?: number;
  /** airspy: the gain mode (0 sensitive, 1 linear, 2 free) and the owner's limit on the preset (max position, locked). */
  aspGainMode?: number; aspLimit?: { max: number; locked?: boolean } | null;
  /** The owner fixed the gain, and whether this listener holds the admin password. */
  locked?: boolean; admin?: boolean;
  /** The radio has more controls (the hardware panel) — ⌄. */
  more?: boolean;
}
export type GainSet = 'gain-index' | 'rsp-gr' | 'ahf-att' | 'hrf-lna' | 'asp-preset';
export type GainRow =
  | { kind: 'none'; more: boolean }
  | { kind: 'auto' | 'manual' | 'locked'; driver: string;
      /** the AGC key's name ('' = this radio has no automatic gain: no key) */
      name: string;
      /** the slider's own label where it is not plain gain (LNA, SENSITIVE, LINEAR, GR, ATT) */
      label: string;
      value: string; min: number; max: number; step: number; at: number; set: GainSet; more: boolean; unlocked: boolean };
export function gainRowFor(g: GainIn): GainRow {
  const more = !!g.more;
  const lockedNow = !!g.locked && !g.admin;
  const unlocked = !!g.locked && !!g.admin;
  const kindOf = (auto: boolean) => (lockedNow ? 'locked' : auto ? 'auto' : 'manual') as 'locked' | 'auto' | 'manual';
  const listRow = (driver: string, name: string, auto: boolean): GainRow => {
    const gains = g.gains!;
    const want = g.gainTenthDb ?? 0;
    const idx = gains.reduce((bi, v, i) => (Math.abs(v - want) < Math.abs(gains[bi] - want) ? i : bi), 0);
    const t = auto && (g.liveTenthDb ?? -1) >= 0 ? g.liveTenthDb! : gains[idx] ?? 0;
    return { kind: kindOf(auto), driver, name, label: '', value: `${(t / 10).toFixed(1)} dB`, min: 0, max: gains.length - 1, step: 1, at: idx,
             set: 'gain-index', more, unlocked };
  };
  switch (g.driver) {
    case 'rtl':
      if (g.noHwGain || !g.gains?.length) return { kind: 'none', more };
      return listRow('rtl', 'AUTO', !!g.autoGain);
    case 'usrp':
      if (!g.gains?.length) return { kind: 'none', more };
      // ★ the AGC key only where the radio has one; without it the slider alone (never a dead AUTO)
      return listRow('usrp', g.hwAgc ? 'AGC' : '', !!g.hwAgc && !!g.autoGain);
    case 'sdrplay': {
      if (typeof g.rspIfGr !== 'number') return { kind: 'none', more };
      const lo = g.rspGrMin ?? 20, hi = g.rspGrMax ?? 59;
      /* ★★★ beta97 (Stuart, build 587) the RF AND the IF on the one row: "RF LNA 3 · IF 42 dB" (the LNA state and the IF gain
       *  reduction the server reports); the loop's own name on the AGC key ("VibeAGC") */
      const ifText = `IF ${Math.round(g.rspIfGr)} dB`;
      const value = typeof g.rspLna === 'number' ? `RF LNA ${Math.round(g.rspLna)} \u00b7 ${ifText}`
        : typeof g.rspSys === 'number' && g.rspSys > 0 ? `${g.rspSys.toFixed(0)} dB` : `GR ${Math.round(g.rspIfGr)} dB`;
      return { kind: kindOf(g.rspIfAgc !== false), driver: 'sdrplay', name: g.agcName || 'IF AGC', label: 'GR', value, min: lo, max: hi, step: 1,
               at: Math.max(lo, Math.min(hi, Math.round(g.rspIfGr))), set: 'rsp-gr', more, unlocked };
    }
    case 'airspyhf': {
      if (typeof g.ahfAtt !== 'number') return { kind: 'none', more };
      const step = g.attStepDb ?? 6, n = g.attSteps ?? 9;
      const agc = g.ahfAgc !== false;
      return { kind: kindOf(agc), driver: 'airspyhf', name: 'AGC', label: 'ATT', value: agc ? 'ATT by AGC' : `ATT ${Math.round(g.ahfAtt) * step} dB`,
               min: 0, max: Math.max(0, n - 1), step: 1, at: Math.max(0, Math.min(n - 1, Math.round(g.ahfAtt))), set: 'ahf-att', more, unlocked };
    }
    case 'hackrf': {
      // ★ no AGC of any kind on this radio: the LNA slider alone (VGA and the owner-only amp under ⌄)
      const lna = Math.max(0, Math.min(40, Math.round((g.hrfLna ?? 0) / 8) * 8));
      return { kind: kindOf(false), driver: 'hackrf', name: '', label: 'LNA', value: `LNA ${lna} dB`, min: 0, max: 40, step: 8, at: lna, set: 'hrf-lna',
               more: true, unlocked };
    }
    case 'airspy': {
      const mode = g.aspGainMode ?? 1;
      if (mode === 2) return { kind: 'none', more: true };   // ★ FREE: three stages, no single gain — under ⌄
      const max = Math.max(0, Math.min(21, g.aspLimit?.max ?? 21));
      const at = Math.min(max, Math.max(0, Math.round((g.gainTenthDb ?? 0) / 10)));
      const label = mode === 0 ? 'SENSITIVE' : 'LINEAR';
      const kind = lockedNow || g.aspLimit?.locked ? 'locked' : 'manual';
      return { kind, driver: 'airspy', name: '', label, value: `${label} ${at}`, min: 0, max, step: 1, at, set: 'asp-preset', more: true, unlocked };
    }
    default:
      return { kind: 'none', more };
  }
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
  /* ★★★ beta97 (Stuart, build 587: hard-edged vertical bands of noise) a column NARROWER than a bin (a narrow span on a wide
   *  capture): the bins are interpolated, never one bin's value smeared across a block of columns */
  if ((hi - lo) / cols < hzPer) {
    for (let c = 0; c < cols; c++) {
      const x = (lo + ((hi - lo) * (c + 0.5)) / cols - f0) / hzPer - 0.5;
      if (x < -0.5 || x > n - 0.5) continue;
      const i = Math.max(0, Math.min(n - 1, Math.floor(x))), j = Math.min(n - 1, i + 1), t = Math.max(0, Math.min(1, x - i));
      out[c] = bins[i] * (1 - t) + bins[j] * t;
    }
    return out;
  }
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
/** ★★★ beta97 hotfix THE MINIMUM SPAN (the web's rule too): the drawer never shows fewer than this many of the frame's
 *  bins — a narrower view on a wide capture is widened about its centre, so there is always signal structure to see. */
export const MWF_MIN_BINS = 16;
export function mwfMinSpan(lo: number, hi: number, binHz: number, minBins = MWF_MIN_BINS): { lo: number; hi: number } {
  const want = binHz > 0 && Number.isFinite(binHz) ? binHz * minBins : 0;
  if (!(hi > lo) || !(hi - lo < want)) return { lo, hi };
  const c = (lo + hi) / 2;
  return { lo: c - want / 2, hi: c + want / 2 };
}
/** A marker's x (0…1 of the drawer) — null when it falls outside the span. */
export function mwfX(hz: number, lo: number, hi: number): number | null { return hi > lo && hz >= lo && hz <= hi ? (hz - lo) / (hi - lo) : null; }

/** ★★ THE SHARED-TUNER PILL'S WORDS for the room it has: the whole line, else the short form "Ask · 3/4" — never an
 *  ellipsis through the count (Stuart). `maxChars` is what fits (≈ the pill's width / 7.2 pt). */
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

/** ★★ ADS-B LIVE: its gain is the server's ramp (AUTO, training) over the radio's list — the web lends its #adsbGain into
 *  TUNE while ADS-B is on; here it is the same one row: [AUTO] value (the ramp) · slider over the list (manual), the
 *  owner's ceiling the slider's end. null view (no gain list, owner-fixed, older server) = no row. */
export function adsbGainRow(view: { auto: boolean; now: string; idx: number; maxIdx: number } | null, more = false): GainRow {
  if (!view) return { kind: 'none', more };
  return { kind: view.auto ? 'auto' : 'manual', driver: 'adsb', name: 'AUTO', label: '', value: view.now, min: 0, max: Math.max(1, view.maxIdx), step: 1,
           at: Math.max(0, Math.min(view.maxIdx, view.idx)), set: 'gain-index', more, unlocked: false };
}

/** ★★★ THE SERVERS CARD (the web's mapServersSync — Stuart: "must ALWAYS be reachable on every map screen"): LEAVE first
 *  (back to this server's radios when there is a choice, else the server list), then a row per OTHER radio on this
 *  server with its listeners against its limit (live while the card is open, else the limit alone). A radio behind a PIN
 *  this visit has not unlocked goes to the radio list (where its PIN is typed), never a dead switch.
 *  ids: 'back' · 'r:<id>' (switch) · 'pin:<id>' (to the radio list). */
export interface RadioLite { id: string; label: string; driver?: string; users?: number; pinLocked?: boolean }
export function serversRows(o: { radios: RadioLite[]; mine: string | null; viaDoor: boolean; host: string;
                                 live?: Record<string, { listeners: number; max: number }>; unlocked?: Record<string, boolean> }): Array<{ id: string; label: string; sub: string }> {
  const others = o.viaDoor ? o.radios.filter((r) => r.id !== o.mine) : [];
  const rows = [{ id: 'back', label: others.length ? 'Leave — back to this server’s radios' : 'Leave server — back to the server list', sub: o.host }];
  for (const r of others) {
    const lv = o.live?.[r.id], max = lv?.max || r.users || 0;
    const count = lv ? `${lv.listeners}/${max || '?'} listeners` : max ? `?/${max} listeners` : '';
    const pin = r.pinLocked === true && !o.unlocked?.[r.id];
    rows.push({ id: `${pin ? 'pin' : 'r'}:${r.id}`, label: `Switch to ${r.label}`,
                sub: [pin ? 'PIN needed — opens the radio list' : '', count, r.driver ?? ''].filter(Boolean).join(' · ') || 'another radio on this server' });
  }
  return rows;
}

/** ★★★ SOURCES (the MAP card — the web's asCardsSync): each radio of this server and each feed, with what it holds, as
 *  this viewer's own show / hide (never sent). Offered only with more than one row (MAP-TEMPLATE rule 7). Every feed is
 *  tagged "feed" in the table, so the feed rows show and hide together. */
export interface SourceRow { id: string; name: string; what: string; on: boolean; down?: boolean }
export function sourceRows(o: { aircraft: ReadonlyArray<{ radios?: string[] }>; vessels: ReadonlyArray<{ radios?: string[] }>;
                                feeds: ReadonlyArray<{ title: string; label?: string; state?: string; aircraft?: number }>;
                                mine: string; onRadio: boolean; names: Record<string, string>; hidden: ReadonlySet<string>;
                                liveText?: string; mineHolds?: string }): SourceRow[] | null {
  const per = new Map<string, { ac: number; ships: number }>();
  const bump = (ids: string[] | undefined, k: 'ac' | 'ships') => { for (const id of ids ?? []) { const e = per.get(id) ?? { ac: 0, ships: 0 }; e[k]++; per.set(id, e); } };
  for (const a of o.aircraft) bump(a.radios, 'ac');
  for (const v of o.vessels) bump(v.radios, 'ships');
  const rows: SourceRow[] = [];
  const holds = (e: { ac: number; ships: number }) => [e.ac ? `${e.ac} aircraft` : '', e.ships ? `${e.ships} ships` : ''].filter(Boolean).join(' · ') || '—';
  for (const [id, e] of per) {
    if (id === 'feed') continue;
    const mine = o.onRadio && id === o.mine;
    rows.push({ id, on: !o.hidden.has(id), name: o.names[id] ?? (mine ? 'This radio' : `Radio ${id}`),
                what: `${mine ? 'this radio' : 'a radio on this server'}${mine && o.liveText ? ` · live: ${o.liveText}` : ''} · ${holds(e)}` });
  }
  if (o.onRadio && o.mine && !per.has(o.mine) && o.mineHolds) {
    rows.push({ id: o.mine, on: !o.hidden.has(o.mine), name: o.names[o.mine] ?? 'This radio', what: `this radio${o.liveText ? ` · live: ${o.liveText}` : ''} · ${o.mineHolds}` });
  }
  if (per.has('feed') && !o.feeds.length) rows.push({ id: 'feed', on: !o.hidden.has('feed'), name: 'Aircraft feeds', what: `the server’s feeds · ${per.get('feed')!.ac} aircraft` });
  for (const f of o.feeds) {
    const up = /connected|listening|live|ok/i.test(f.state ?? '');
    rows.push({ id: 'feed', on: !o.hidden.has('feed'), name: f.title, what: `${(f.label || 'adsb').toUpperCase()} feed · ${f.state || 'unknown'} · ${f.aircraft ?? 0} aircraft`, down: !up });
  }
  return rows.length > 1 ? rows : null;
}

/** ★★ LIVE ELSEWHERE (Stuart 15:31, the web's `others`): which layers another of this server's radios — or its feeds —
 *  heard an aircraft on in the last minute (the same table SOURCES and RATE read; no fetch). layer id → source names. */
const SRC_LAYER: Record<string, string> = { 'ADS-B': 'adsb', VDL2: 'vdl2', ACARS: 'acars', HFDL: 'hfdl', 'ADS-C': 'adsc' };
/** ★★★ beta97 (Stuart, build 587: ADS-B said "LIVE 0" while PiAware put 33 on the map, and the LAYERS chip lost its aircraft)
 *  how many each layer's OTHER sources hold — this server's other radios and its feeds, heard in the last minute (the same
 *  table and rule as layerOthers). A row with both this radio and others shows BOTH counts (layerSplit). */
export function layerOtherCounts(aircraft: ReadonlyArray<{ age: number; src: string[]; radios?: string[] }>, o: { mine: string; onRadio: boolean }): Record<string, number> {
  const out: Record<string, number> = {};
  for (const p of aircraft) {
    if (!(p.age < 60)) continue;
    if (!(p.radios ?? []).some((r) => !(o.onRadio && r === o.mine))) continue;
    const seen = new Set<string>();
    for (const s of p.src) { const id = SRC_LAYER[s]; if (!id || seen.has(id)) continue; seen.add(id); out[id] = (out[id] ?? 0) + 1; }
  }
  return out;
}
/** ★★★ A LAYERS row's badges and counts: this radio live AND other sources → BOTH — the filled LIVE with this radio's count
 *  (green) and the outlined LIVE with the others' count; one source → one badge / count as before (liveBadges). The row's
 *  TOTAL (the LAYERS chip's family count) is never less than either: the sources overlap (one aircraft, two receivers), so the
 *  larger of the two is what is on the map at least — a family never vanishes because this radio has heard 0 so far. */
export function layerSplit(r: { n?: number; nOther?: number; live?: boolean; others?: string[] }): { split: boolean; mine?: number; other?: number; total?: number } {
  const split = !!r.live && !!r.others?.length && r.nOther != null;
  if (!split) return { split: false, total: r.n };
  return { split: true, mine: r.n ?? 0, other: r.nOther, total: Math.max(r.n ?? 0, r.nOther ?? 0) };
}
export function layerOthers(aircraft: ReadonlyArray<{ age: number; src: string[]; radios?: string[] }>, o: { mine: string; onRadio: boolean;
                            names: Record<string, string>; feedTitle?: string }): Record<string, string[]> {
  const out = new Map<string, Set<string>>();
  for (const p of aircraft) {
    if (!(p.age < 60)) continue;
    for (const r of p.radios ?? []) {
      if (o.onRadio && r === o.mine) continue;
      const name = r === 'feed' ? o.feedTitle || 'FEED' : o.names[r] ?? 'SDR';
      for (const s of p.src) { const id = SRC_LAYER[s]; if (!id) continue; (out.get(id) ?? out.set(id, new Set()).get(id)!).add(name); }
    }
  }
  return Object.fromEntries([...out].map(([k, v]) => [k, [...v]]));
}

/** ★★★ FIT TO ONE LAYER (Stuart 11-27: "with this much data the fit button doesnt work"): on the AIR & SEA map the shared
 *  auto fit owns the camera and frames ONE layer — the viewer's pick, else mapCards.fitDefault (ADS-B where there is ADS-B,
 *  else the live mode's, else whatever moves). Each item's layer, from its own fields (the web tags `_fg`; the app's keys
 *  say it): adsb · datalink · ships · dsc · navst · warnings · weather · vor · sondes · other; null = never framed. */
export const FIT_ORDER = ['adsb', 'datalink', 'ships', 'dsc', 'navst', 'warnings', 'weather', 'vor', 'sondes', 'other'] as const;
export const FIT_LABEL: Record<string, string> = { adsb: 'ADS-B', datalink: 'DATALINK', ships: 'SHIPS', dsc: 'DSC', navst: 'NAVTEX STATIONS',
  warnings: 'WARNINGS', weather: 'WEATHER', vor: 'VOR', sondes: 'SONDES', other: 'OTHER' };
export function fitGroupOf(it: { key: string; _fg?: unknown; _adsb?: unknown; _nofit?: unknown; lat?: unknown; lon?: unknown }): string | null {
  if (it._nofit || it.lat == null || it.lon == null || !Number.isFinite(Number(it.lat)) || !Number.isFinite(Number(it.lon))) return null;
  if (typeof it._fg === 'string') return it._fg;
  const k = String(it.key);
  if (k.startsWith('adsb:')) return 'adsb';
  if (it._adsb) return (it._adsb as { sparse?: boolean }).sparse ? 'datalink' : 'adsb';
  if (k.startsWith('s:')) return 'ships';
  if (k.startsWith('d:')) return 'dsc';
  if (k.startsWith('w:')) return 'warnings';
  if (k.startsWith('m:')) return 'weather';
  if (k.startsWith('vor:') || k.startsWith('vos:')) return 'vor';
  if (k.startsWith('sd:') || k.startsWith('sds:')) return 'sondes';
  if (k.includes('|')) return 'navst';
  return 'other';
}
/** The FIT TO rows (the layers drawn now with how many placed items each) and the layer FIT frames. */
export function fitGroups(items: ReadonlyArray<Parameters<typeof fitGroupOf>[0]>, pick: string, main: string,
                          fitDefault: (counts: Record<string, number>, main: string) => string): { groups: Array<{ id: string; label: string; n: number }>; use: string } {
  const n: Record<string, number> = {};
  for (const it of items) { const g = fitGroupOf(it); if (g) n[g] = (n[g] ?? 0) + 1; }
  const groups = FIT_ORDER.filter((g) => g !== 'other' && (n[g] ?? 0) > 0).map((g) => ({ id: g, label: FIT_LABEL[g], n: n[g] }));
  const use = pick && (n[pick] ?? 0) > 0 ? pick : fitDefault(n, main);
  return { groups, use };
}
/** The live mode → its own layer (fitDefault's `main`). */
export function fitMainOf(mode: string): string {
  return mode === 'datalink' || mode === 'hfdl' ? 'datalink' : mode === 'ais' ? 'ships' : mode === 'sondes' ? 'sondes' : mode === 'vormap' ? 'vor'
    : mode === 'navtex' ? 'navst' : mode === 'dsc' ? 'dsc' : mode === 'adsb' ? 'adsb' : '';
}

/** The layers' families for the LAYERS chip (icon over count): aircraft, ships, NAVTEX stations, sondes, beacons — each
 *  the sum of its shown layers' counts (the web's setLayerSummary families). */
export const LAYER_FAMILY: Record<string, 'aircraft' | 'ship' | 'navtex' | 'balloon' | 'beacon'> = {
  adsb: 'aircraft', acars: 'aircraft', vdl2: 'aircraft', hfdl: 'aircraft', adsc: 'aircraft',
  ships: 'ship', dsc: 'ship', navst: 'navtex', sondes: 'balloon', vor: 'beacon', ndb: 'beacon',
};
export function layerFamilies(rows: Array<{ id: string; n?: number; nOther?: number; nPos?: number; live?: boolean; others?: string[]; colour?: string }>, toggled: Record<string, boolean>): Array<{ glyph: 'aircraft' | 'ship' | 'navtex' | 'balloon' | 'beacon'; n: number; colour: string; pos?: number }> {
  const out: Array<{ glyph: 'aircraft' | 'ship' | 'navtex' | 'balloon' | 'beacon'; n: number; colour: string; pos?: number }> = [];
  for (const r of rows) {
    const f = LAYER_FAMILY[r.id];
    const n = layerSplit(r).total;   // ★★★ beta97 the row's TOTAL (both sources), never only this radio's
    if (!f || toggled[r.id] === false || !(n && n > 0)) continue;
    const pos = r.nPos != null ? Math.min(n, r.nPos) : n;   // ★★★ beta97 hotfix how many of them the map can draw
    const have = out.find((x) => x.glyph === f);
    if (have) { have.n += n; have.pos = (have.pos ?? 0) + pos; } else out.push({ glyph: f, n, colour: r.colour || '#9fb0c2', pos });
  }
  return out;
}

/** ★★★ beta97 hotfix (Stuart: "25 aircraft 0 position messages") A COUNT THAT SAYS HOW MANY ARE ON THE MAP: "0/25" (on the map /
 *  heard) when they differ, else the one number — the LAYERS chip and its rows. */
export function layerCountText(n: number, pos?: number | null): string {
  return pos != null && pos !== n ? `${Math.max(0, Math.min(n, pos))}/${n}` : String(n);
}
/** …and in words, for the LIST's line and a screen reader: "25 aircraft · 0 with position" (one number when all are placed). */
export function layerPosWords(n: number, pos: number | null | undefined, noun: string, plural = noun + 's'): string {
  const what = `${n} ${n === 1 ? noun : plural}`;
  return pos != null && pos !== n ? `${what} \u00b7 ${Math.max(0, Math.min(n, pos))} with position` : what;
}

/** The credits' (i) folds itself away this long after the map opens, or at the first touch (Stuart). */
export const CREDITS_FOLD_MS = 5000;
/** The ask-lock pulse on the pill and CHAT: this long, then it stops (2–3 s). */
export const ASK_PULSE_MS = 2600;

/** ★★★ THE PAD'S WALK (the web's rules, Stuart on build 586: "moving the arrow down on the list goes to the lower lines but
 *  doesn't scroll; also scrolling a super long list to get to the buttons underneath"): the chips in reading order, the
 *  pill, then the LIST as ONE stop, then the bottom-right group — never through the rows. ENTER on the LIST steps INTO it
 *  (its rows, then its bar's keys; BACK steps out to the LIST stop). An open card is its own scope (ENTER on a chip opens
 *  it and the highlight goes inside; BACK shuts it and the highlight is back on its chip). */
/** ★★★ beta97 THE LIST'S OWN CONTROLS AS STOPS (Stuart, build 587: "the enlarge button and the sort controls at the bottom
 *  need to be SEPARATE elements"), in the drawer's visual order: above the LIST stop the folder tabs (≋ swap, ⌃ maximise — the
 *  side panel has no maximise; its mode tabs head it), × ALL and ROUTE; below it the foot bar (☰ hide, the mode tabs, ‖ +N,
 *  sort). What MapChrome's ListDrawer renders, in the same order. */
export function listStops(o: { side: boolean; swap: boolean; tabs: string[]; filter?: boolean; route?: boolean; pause?: boolean; sort?: boolean; wf?: boolean }): { top: string[]; bar: string[] } {
  const tabs = o.wf ? [] : o.tabs.length >= 2 ? o.tabs.map((t) => `tab:${t}`) : [];
  const top = [...(o.swap ? ['swap'] : []), ...(o.side ? [] : ['max']), ...(o.side ? tabs : []),
               ...(!o.wf && o.filter ? ['all'] : []), ...(!o.wf && o.route ? ['route'] : [])];
  const bar = ['hide', ...(o.side ? [] : tabs), ...(!o.wf && o.pause ? ['pause'] : []), ...(!o.wf && o.sort ? ['sort'] : [])];
  return { top, bar };
}
export function padOrder(g: ChromeGeom, opts: { cardOpen?: boolean; listInside?: boolean; listRows?: number; br: boolean; pill?: boolean; listTop?: string[]; listBar?: string[] }): string[] {
  if (opts.cardOpen) return ['card'];
  if (opts.listInside) return [...(opts.listTop ?? []), ...Array.from({ length: Math.max(0, opts.listRows ?? 0) }, (_, i) => `row:${i}`), ...(opts.listBar ?? ['bar'])];
  const chips = (Object.keys(g.boxes) as ChipId[]).sort((a, b) => {
    if (g.short) return (g.boxes[a]!.x - g.boxes[b]!.x);
    const side = (id: ChipId) => (g.cardFrom[id] === 'R' || id === 'rate' ? 1 : 0);
    return side(a) - side(b) || g.boxes[a]!.y - g.boxes[b]!.y;
  });
  return [...chips.map((c) => `chip:${c}`), ...(opts.pill ? ['pill'] : []), ...(opts.listTop ?? []), 'list', ...(opts.listBar ?? []), ...(opts.br ? ['help'] : [])];
}

/** ★★★ STUART'S LIST FLOW (as the R36S handheld UI — the same spec the web follows), as a pure step function the LIST drawer
 *  runs on ENTER / BACK (Up / Down are the scope's own walk, row by row, the list scrolling to keep the row in view):
 *    stop  + ENTER → rows   (the first row with a place on the map)
 *    rows  + ENTER → item   (that row selected: centred, its card opened, the highlight INTO the card)
 *    item  + BACK  → rows   (the card shut, the highlight on the SAME row)
 *    rows  + BACK  → stop   (out of the list; free to move round the chips and the bottom-right group)
 *    stop  + BACK  → out    (passed on: the chrome's own BACK — a card, then the map's exit) */
export type ListFlow = { at: 'stop' | 'rows' | 'item'; row: number };
export function listFlowStep(st: ListFlow, key: 'enter' | 'back' | 'up' | 'down', o: { rows: number; firstPlaced: number }): ListFlow | 'out' {
  if (key === 'back') return st.at === 'item' ? { at: 'rows', row: st.row } : st.at === 'rows' ? { at: 'stop', row: st.row } : 'out';
  if (key === 'enter') {
    if (st.at === 'stop') return o.rows > 0 ? { at: 'rows', row: Math.max(0, Math.min(o.rows - 1, o.firstPlaced)) } : st;
    if (st.at === 'rows') return { at: 'item', row: st.row };
    return st;
  }
  if (st.at !== 'rows' || o.rows <= 0) return st;
  return { at: 'rows', row: Math.max(0, Math.min(o.rows - 1, st.row + (key === 'down' ? 1 : -1))) };
}

/** ★★★ THE CHROME'S SIZE (Stuart, build 586 on the Mac: "on the iPad/Mac build they are super tiny"): a phone's window is the
 *  reference (1); a TABLET-SIZED window (shorter side ≥ 600 — every iPad, a Mac window) scales by its long side the way the
 *  rest of the app does (useUiScale: max(W, H) / 926), 1 … 1.45 — so a 1440-point Mac window, drawn at the Mac's 77 %, shows
 *  a 64-point chip as ≈ 49 px: the web's 48. The window, never the device. */
export const CHROME_SCALE_MAX = 1.45;
export function chromeScale(w: number, h: number): number {
  if (!(Math.min(w, h) >= 600)) return 1;
  return Math.max(1, Math.min(CHROME_SCALE_MAX, Math.max(w, h) / 926));
}
/** The layout at scale k: worked out on the window divided by k (so the same rules decide), then every distance times k. */
export function chromeGeomAt(p: ChromeIn, k: number): { g: ChromeGeom; g0: ChromeGeom; k: number } {
  const kk = Number.isFinite(k) && k > 0 ? k : 1;
  const g0 = chromeGeom({ ...p, w: p.w / kk, h: p.h / kk, insets: { top: p.insets.top / kk, bottom: p.insets.bottom / kk, left: p.insets.left / kk, right: p.insets.right / kk } });
  if (kk === 1) return { g: g0, g0, k: 1 };
  const m = (v: number) => v * kk;
  const bx = (b: Box): Box => ({ x: m(b.x), y: m(b.y), w: m(b.w), h: m(b.h) });
  const boxes: ChromeGeom['boxes'] = {};
  for (const id of Object.keys(g0.boxes) as ChipId[]) boxes[id] = bx(g0.boxes[id]!);
  const g: ChromeGeom = { ...g0, boxes, safe: { l: m(g0.safe.l), r: m(g0.safe.r), t: m(g0.safe.t), b: m(g0.safe.b) },
    list: { ...g0.list, x: m(g0.list.x), bottom: m(g0.list.bottom), w: m(g0.list.w), maxH: m(g0.list.maxH), top: m(g0.list.top) },
    pill: { ...g0.pill, x: m(g0.pill.x), w: m(g0.pill.w), y: m(g0.pill.y), h: m(g0.pill.h), pad: m(g0.pill.pad) }, br: { ...g0.br, right: m(g0.br.right), bottom: m(g0.br.bottom) }, zoomTop: m(g0.zoomTop) };
  return { g, g0, k: kk };
}
/** A card's box at scale k (cardBox on the unscaled layout, times k). */
export function cardBoxAt(at: { g0: ChromeGeom; k: number }, id: ChipId, w: number, h: number): { x: number; y: number; w: number; maxH: number; yMin: number } {
  const c = cardBox(at.g0, id, w / at.k, h / at.k);
  return { x: c.x * at.k, y: c.y * at.k, w: c.w * at.k, maxH: c.maxH * at.k, yMin: c.yMin * at.k };
}
/** ★★ The card's top for its content: beside its chip when it fits below, else risen (never above yMin) so the most shows;
 *  its bottom stays where it was allowed to reach. */
export function cardRise(at: { y: number; maxH: number; yMin?: number }, wantH: number): { top: number; maxH: number } {
  const bottom = at.y + at.maxH, yMin = Math.min(at.y, at.yMin ?? at.y);
  if (!(wantH > at.maxH)) return { top: at.y, maxH: at.maxH };
  const top = Math.max(yMin, bottom - wantH);
  return { top, maxH: bottom - top };
}
