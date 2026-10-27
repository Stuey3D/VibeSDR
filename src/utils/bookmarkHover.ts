/**
 * bookmarkHover.ts — "which bookmark is under the pointer?" for the frequency-at-pointer label.
 *
 * ★★ GitHub #60 (Gesiima asked for bookmark markers on the frequency scale). Stuart, 2026-10-27: "we can have a
 *    little bookmark on the mouse tooltip alongside the frequency like we added for Kiko but I do not want to crowd
 *    the waterfall with labels". So nothing is DRAWN on the scale or the waterfall — the pointer label names the
 *    bookmark it is over, and only while it is over one.
 *
 * "Over" = inside the bookmark's passband: the one it was saved with (bandwidth_low/high, offsets from the carrier,
 * UberSDR's own fields), else its mode's default (the same widths the client tunes with). The nearest carrier wins
 * when several overlap; the rest are counted ("+2 more").
 *
 * ★ CHEAP. Built once per list change (sorted by carrier, plus the widest reach either side), then every lookup is
 *   one binary search and a short scan of the entries close enough to reach the pointer. A lookup allocates nothing:
 *   it fills the caller's own result object. The pointer label runs this once per animation frame.
 *
 * Pure: no DOM, no React Native — the web client uses it today; the app can when it has a pointer label.
 */

/** Anything bookmark-shaped we hold: the listener's own (UserBookmark), the receiver's (/bookmarks). */
export interface HoverBookmarkIn {
  name: string;
  frequency: number;
  mode?: string | null;
  bandwidth_low?: number | null;
  bandwidth_high?: number | null;
}

/** A mode's default passband about its carrier (Hz) — mirrors the client's MODE_BANDWIDTHS, plus DAB's block. */
const MODE_SPAN: Record<string, [number, number]> = {
  usb: [50, 2700], lsb: [-2700, -50],
  am: [-5000, 5000], sam: [-5000, 5000],
  cw: [-200, 200], cwu: [-200, 200], cwl: [-200, 200],
  fm: [-6000, 6000], nfm: [-5000, 5000],
  wfm: [-100000, 100000],
  drm: [-5000, 5000],
  dab: [-768000, 768000],     // a DAB block is 1.536 MHz wide
};
/** No mode, or one we do not know: a sensible half-channel. */
const FALLBACK_SPAN: [number, number] = [-2500, 2500];

/** The bookmark's span about its carrier: its own saved passband, else the mode default. */
export function hoverSpan(b: HoverBookmarkIn): [number, number] {
  const lo = b.bandwidth_low, hi = b.bandwidth_high;
  if (typeof lo === 'number' && typeof hi === 'number' && Number.isFinite(lo) && Number.isFinite(hi) && hi > lo) {
    return [lo, hi];
  }
  const m = (b.mode || '').toLowerCase();
  return MODE_SPAN[m] ?? FALLBACK_SPAN;
}

/** One reach class: carriers ascending, absolute passband edges parallel to them. */
interface HoverTier {
  freq: Float64Array;
  lo: Float64Array;
  hi: Float64Array;
  name: string[];
  /** The furthest any passband in this tier reaches below / above its own carrier — bounds the scan. */
  reachDown: number;
  reachUp: number;
}

/** ★ TIERED BY WIDTH. One wide bookmark (a WFM station, a DAB block) would otherwise widen the scan for every
 *  lookup — and a receiver holding an airband import has hundreds of narrow channels inside one DAB block's reach.
 *  Narrow, medium and wide bookmarks each get their own sorted array, so each scan covers only what can reach. */
export interface BookmarkHoverIndex {
  tiers: HoverTier[];
  size: number;
}

/** Tier limits on a bookmark's furthest reach from its carrier (Hz): voice/CW channels, FM, the rest. */
const TIER_REACH = [8_000, 120_000, Infinity];

/** Filled by bookmarkAt — one object, reused every frame. */
export interface BookmarkHit {
  /** '' when the pointer is over no bookmark. */
  name: string;
  frequency: number;
  /** How many OTHER bookmarks also cover the pointer. */
  more: number;
}

export function newBookmarkHit(): BookmarkHit { return { name: '', frequency: 0, more: 0 }; }

/** Build the index from one or more lists (the listener's own first). The same name on the same carrier — a
 *  bookmark kept both locally and on the receiver — is counted once. */
export function buildBookmarkHoverIndex(...lists: ReadonlyArray<ReadonlyArray<HoverBookmarkIn> | null | undefined>): BookmarkHoverIndex {
  const rows: { f: number; lo: number; hi: number; name: string }[] = [];
  const seen = new Set<string>();
  for (const list of lists) {
    if (!list) continue;
    for (const b of list) {
      if (!b || typeof b.frequency !== 'number' || !Number.isFinite(b.frequency) || b.frequency <= 0) continue;
      const name = typeof b.name === 'string' ? b.name.trim() : '';
      if (!name) continue;
      const key = Math.round(b.frequency) + '\u0000' + name.toLowerCase();
      if (seen.has(key)) continue;
      seen.add(key);
      const [l, h] = hoverSpan(b);
      rows.push({ f: b.frequency, lo: b.frequency + l, hi: b.frequency + h, name });
    }
  }
  rows.sort((a, b) => a.f - b.f);
  const groups: (typeof rows)[] = TIER_REACH.map(() => []);
  for (const r of rows) {
    const reach = Math.max(r.f - r.lo, r.hi - r.f);
    let t = 0;
    while (reach > TIER_REACH[t]) t++;
    groups[t].push(r);
  }
  const tiers: HoverTier[] = [];
  for (const g of groups) {
    if (!g.length) continue;
    const n = g.length;
    const t: HoverTier = { freq: new Float64Array(n), lo: new Float64Array(n), hi: new Float64Array(n), name: new Array(n), reachDown: 0, reachUp: 0 };
    for (let i = 0; i < n; i++) {
      const r = g[i];
      t.freq[i] = r.f; t.lo[i] = r.lo; t.hi[i] = r.hi; t.name[i] = r.name;
      if (r.f - r.lo > t.reachDown) t.reachDown = r.f - r.lo;
      if (r.hi - r.f > t.reachUp) t.reachUp = r.hi - r.f;
    }
    tiers.push(t);
  }
  return { tiers, size: rows.length };
}

/** First index whose carrier is >= hz. */
function lowerBound(a: Float64Array, hz: number): number {
  let lo = 0, hi = a.length;
  while (lo < hi) { const mid = (lo + hi) >>> 1; if (a[mid] < hz) lo = mid + 1; else hi = mid; }
  return lo;
}

/** The bookmark under `hz` into `out` (name '' when none). Returns true when there is one. No allocation. */
export function bookmarkAt(idx: BookmarkHoverIndex | null, hz: number, out: BookmarkHit): boolean {
  out.name = ''; out.frequency = 0; out.more = 0;
  if (!idx || !idx.size || !Number.isFinite(hz)) return false;
  let bestT: HoverTier | null = null, best = -1, bestD = Infinity, count = 0;
  for (let k = 0; k < idx.tiers.length; k++) {
    const t = idx.tiers[k], f = t.freq;
    // Only carriers within [hz - reachUp, hz + reachDown] can have a passband that covers hz.
    const end = hz + t.reachDown;
    for (let i = lowerBound(f, hz - t.reachUp); i < f.length && f[i] <= end; i++) {
      if (hz < t.lo[i] || hz > t.hi[i]) continue;
      count++;
      const d = Math.abs(f[i] - hz);
      if (d < bestD) { bestD = d; best = i; bestT = t; }
    }
  }
  if (!bestT) return false;
  out.name = bestT.name[best]; out.frequency = bestT.freq[best]; out.more = count - 1;
  return true;
}
