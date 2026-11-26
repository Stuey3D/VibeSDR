/**
 * lrptChannels.ts — METEOR LRPT's live preview: WHICH CHANNEL the viewer sees (beta96, 2026-11-26). Shared by the app
 * (DecoderClient → DecoderImageCanvas) and the web page (main.ts) — the rule, the parsing and the pixels are here.
 *
 * ★★★ WHY (Stuart 11-26, NickB's Airspy HF+ on a Meteor-M2-3 pass, "channels 1, 2, 4 · 682 frames"): "can we have the option
 * to change the meteor image being viewed in the preview as one of the channels is this grain but the other channels have
 * images". It was a NIGHT pass (19:43 / 20:00 UTC): channels 1 and 2 see reflected sunlight, so at night they hold only the
 * sensor's noise, and the composite (R = G = ch 2, B = ch 1) stretched that into blue/yellow speckle — while channel 4
 * (infrared, MWIR) of the SAME pass was a clean picture of Italy, Sardinia and Corsica in the gallery.
 *
 * WHAT THE SERVER SENDS (vibe_lrpt_run.h kChannelRowOp): beside each 0x01 preview row (RGB, unchanged — an older app shows
 * it), a 0x09 frame:  0x09 | u32 y | u32 w | u8 rApid | u8 bApid | u8 n | n × (u8 apid | w grey bytes)
 * rApid / bApid say what that 0x01 row carries (R = G = rApid, B = bApid; equal when the row is one grey channel); the n
 * channels are the live ones it does not carry. lrpt_status adds "apids" (live channels) and "dark" (visible channels with
 * no sunlight in them — the server measures it, kDarkHi).
 *
 * WHAT THE VIEWER PICKS (per viewer — never the server's decode, its 0x01 rows or the gallery, which still files every
 * channel): AUTO (the default) | COMPOSITE | CH 1 | CH 2 | CH 4 … (only the channels being received).
 *   AUTO = the composite by day; at night (its channels dark) the infrared channel (4, else 5, 6), said in the key:
 *   "AUTO · CH 4 IR (NIGHT)". A pass with no composite (ch 1 or 2 not received): its first lit channel.
 * The pick is remembered per browser (localStorage) / per app (AsyncStorage) — LRPT_PICK_KEY.
 * Every channel's rows share the composite's geometry (same strips, same gaps — hatched alike by the server), so the gap
 * bands and the MAP OUTLINE line up on any channel.
 */

export type LrptView = 'comp' | number;          // the composite, or a channel's APID (64 = ch 1 … 69 = ch 6)
export type LrptPick = 'auto' | LrptView;

export const LRPT_CHAN_OP = 0x09;
export const LRPT_PICK_KEY = 'vibe.lrptChannel';

/** APID → MSU-MR channel number (64 → 1). */
export const chanNo = (apid: number) => apid - 63;
/** Channels 4–6 are infrared (thermal): they carry a picture day and night. */
export const isIrApid = (apid: number) => apid >= 67 && apid <= 69;
const validApid = (a: unknown): a is number => typeof a === 'number' && Number.isInteger(a) && a >= 64 && a <= 69;

export interface LrptChanRow { y: number; w: number; rA: number; bA: number; extras: Array<[number, Uint8Array]> }

/** A 0x09 frame → its row; null when malformed (never throws). The grey bytes are VIEWS into `u8` — copy to keep. */
export function parseLrptChanFrame(u8: Uint8Array): LrptChanRow | null {
  if (u8.length < 12 || u8[0] !== LRPT_CHAN_OP) return null;
  const u32 = (o: number) => ((u8[o] << 24) | (u8[o + 1] << 16) | (u8[o + 2] << 8) | u8[o + 3]) >>> 0;
  const y = u32(1), w = u32(5), rA = u8[9], bA = u8[10], n = u8[11];
  if (!(w > 0 && w <= 4096) || y > 100000 || !validApid(rA) || !validApid(bA) || n > 6) return null;
  const extras: Array<[number, Uint8Array]> = [];
  let o = 12;
  for (let i = 0; i < n; i++) {
    if (o + 1 + w > u8.length) return null;
    const a = u8[o];
    if (!validApid(a)) return null;
    extras.push([a, u8.subarray(o + 1, o + 1 + w)]);
    o += 1 + w;
  }
  return { y, w, rA, bA, extras };
}

/** lrpt_status's "apids" / "dark" → sorted valid APIDs ([] from an older server). */
export function parseApidList(v: unknown): number[] {
  if (!Array.isArray(v)) return [];
  return [...new Set(v.filter(validApid))].sort((a, b) => a - b).slice(0, 6);
}

/** What AUTO shows, and why: 'day' (the composite), 'night' (its channels dark → an infrared channel), 'only' (no
 *  composite on this pass → the first lit channel), '' (nothing to choose from yet → the composite). */
export interface LrptResolved { view: LrptView; why: 'day' | 'night' | 'only' | '' }
export function resolveLrptView(pick: LrptPick, apids: number[], dark: number[], hasComp: boolean): LrptResolved {
  if (pick !== 'auto') {
    if (pick === 'comp' ? hasComp : apids.includes(pick)) return { view: pick, why: '' };
    // a pick this pass cannot show (CH 4 remembered, a pass without ch 4): AUTO for now, the pick kept
  }
  const lit = apids.filter((a) => !dark.includes(a));
  if (hasComp) {
    if (!dark.includes(64) && !dark.includes(65)) return { view: 'comp', why: apids.length ? 'day' : '' };
    const ir = lit.find(isIrApid) ?? lit[0];
    return ir !== undefined ? { view: ir, why: 'night' } : { view: 'comp', why: 'day' };
  }
  const ir = lit.find(isIrApid);
  const first = dark.length && ir !== undefined ? ir : lit[0] ?? apids[0];
  return first !== undefined ? { view: first, why: dark.length ? 'night' : 'only' } : { view: 'comp', why: '' };
}

/** The choices this pass offers, in order: AUTO, the composite (when received), each received channel. */
export function lrptChoices(apids: number[], hasComp: boolean): LrptPick[] {
  return ['auto', ...(hasComp ? ['comp' as const] : []), ...apids];
}
/** The next choice (the key cycles; a remembered pick this pass cannot show steps on from AUTO). */
export function nextLrptPick(pick: LrptPick, choices: LrptPick[]): LrptPick {
  const i = choices.indexOf(pick);
  return choices[(i + 1) % choices.length] ?? 'auto';
}

const viewWord = (v: LrptView) => (v === 'comp' ? 'COMPOSITE' : `CH ${chanNo(v)}${isIrApid(v) ? ' IR' : ''}`);
/** The key's words: "AUTO · COMPOSITE", "AUTO · CH 4 IR (NIGHT)", "CH 2", "COMPOSITE". */
export function lrptPickLabel(pick: LrptPick, r: LrptResolved): string {
  if (pick !== 'auto' && r.view === pick) return viewWord(pick);
  return `AUTO · ${viewWord(r.view)}${r.why === 'night' ? ' (NIGHT)' : ''}`;
}
/** The key's longer explanation (title / accessibility). */
export function lrptPickHint(pick: LrptPick, r: LrptResolved): string {
  const what = pick === 'auto'
    ? (r.why === 'night' ? `AUTO is showing channel ${typeof r.view === 'number' ? chanNo(r.view) : ''} (infrared): a night pass — channels 1 and 2 see sunlight, and are dark`
      : r.why === 'only' ? 'AUTO is showing the first channel received (no composite on this pass)'
      : 'AUTO is showing the composite (channels 2 and 1) — at night it shows the infrared channel')
    : r.view === pick ? `Showing ${pick === 'comp' ? 'the composite' : `channel ${chanNo(pick as number)}`} — for this browser only`
    : 'That channel is not on this pass — showing AUTO';
  return `${what}. Tap for the next channel; the gallery keeps every channel.`;
}

export const pickToStored = (p: LrptPick) => (p === 'auto' ? 'auto' : p === 'comp' ? 'comp' : `ch${chanNo(p)}`);
export function pickFromStored(v: string | null | undefined): LrptPick {
  if (v === 'comp') return 'comp';
  const m = /^ch([1-6])$/.exec(v ?? '');
  return m ? Number(m[1]) + 63 : 'auto';
}

/** The hatch the server draws where a channel has no picture (vibe_lrpt_run.h gapart::hatch), for a row never received. */
const hatchV = (x: number, y: number) => (((x + y) % 6) < 2 ? 54 : 26);

/**
 * The live preview's rows per channel: the 0x01 rows' two channels (R and B) and the 0x09 rows' extras, kept per row, so
 * the viewer can switch channel and see the WHOLE pass so far in it at once. Bounded: one row per preview line (the
 * server pre-sizes 1 600), ≈ 392 bytes per channel per row (≈ 1.9 MB for a full 1 600-row 3-channel pass, ≈ 0.3 MB for
 * a typical 250-row one).
 */
export class LrptRows {
  w = 0;
  maxY = -1;
  /** a 0x09 has been seen: this server sends the channels (an older one: no picker) */
  supported = false;
  private r: (Uint8Array | undefined)[] = [];
  private b: (Uint8Array | undefined)[] = [];
  private map: (number | undefined)[] = [];       // rA << 8 | bA per row (from its 0x09)
  private ex = new Map<number, (Uint8Array | undefined)[]>();
  private seen = new Set<number>();
  private compRows = 0;

  reset(w = 0) { this.w = w; this.maxY = -1; this.r = []; this.b = []; this.map = []; this.ex.clear(); this.seen.clear(); this.compRows = 0; }
  /** a 0x01 row (RGB) */
  composite(y: number, w: number, rgb: Uint8Array) {
    if (y < 0 || y > 4000 || w <= 0) return;
    if (w !== this.w) this.reset(w);
    const R = new Uint8Array(w), B = new Uint8Array(w);
    const n = Math.min(w, Math.floor(rgb.length / 3));
    for (let x = 0; x < n; x++) { R[x] = rgb[x * 3]; B[x] = rgb[x * 3 + 2]; }
    this.r[y] = R; this.b[y] = B;
    if (y > this.maxY) this.maxY = y;
  }
  /** a 0x09 row */
  channels(c: LrptChanRow) {
    if (c.y > 4000 || c.w !== this.w) return;
    this.supported = true;
    if (this.map[c.y] === undefined && c.rA !== c.bA) this.compRows++;
    this.map[c.y] = (c.rA << 8) | c.bA;
    this.seen.add(c.rA); this.seen.add(c.bA);
    for (const [a, g] of c.extras) {
      let col = this.ex.get(a);
      if (!col) { col = []; this.ex.set(a, col); }
      col[c.y] = g.slice(0, this.w);
      this.seen.add(a);
    }
  }
  /** rows carry the composite (ch 2 + ch 1) */
  hasComp(): boolean { return this.compRows > 0; }
  /** the channels seen in this preview */
  apids(): number[] { return [...this.seen].sort((a, b) => a - b); }
  /** Row y as `view` would show it: RGB (w × 3), grey for a channel; null when row y has not arrived; a channel the row
   *  does not carry is hatched (as the server hatches a block with no picture). */
  row(y: number, view: LrptView, out?: Uint8Array): Uint8Array | null {
    const R = this.r[y], B = this.b[y];
    if (!R || !B) return null;
    const w = this.w, o = out && out.length >= w * 3 ? out : new Uint8Array(w * 3);
    if (view === 'comp') {
      for (let x = 0; x < w; x++) { o[x * 3] = o[x * 3 + 1] = R[x]; o[x * 3 + 2] = B[x]; }
      return o;
    }
    const m = this.map[y];
    const g = m !== undefined && (m >> 8) === view ? R : m !== undefined && (m & 255) === view ? B : this.ex.get(view)?.[y];
    for (let x = 0; x < w; x++) { const v = g ? g[x] : hatchV(x, y); o[x * 3] = o[x * 3 + 1] = v; o[x * 3 + 2] = g ? v : v + 10; }
    return o;
  }
  /** row y's 0x09 has arrived (a channel view can be drawn) */
  ready(y: number): boolean { return this.map[y] !== undefined; }
}
