// galleryPager.ts — THE GALLERY IN PAGES (V12 beta 4, 2026-10-22): 25 items a page, newest first, and a pager at the
// bottom — "‹ Newer · Page 2 of 14 · Older ›" with numbered pages (the first, the last, and those around this one).
// ★ One rule for the web client (web/client/src/main.ts) and the app (src/components/GalleryOverlay.tsx); the server
//   pages with gallery.json?offset=&limit= and says the filter's total (vibe_gallery.h listJson).
// ★ Pure — scripts/test_gallery_pager.ts.

export const GALLERY_PAGE = 25;

/** How many pages `total` items fill (at least one, so "Page 1 of 1" on an empty gallery). */
export function galleryPageCount(total: number, per = GALLERY_PAGE): number {
  return Math.max(1, Math.ceil(Math.max(0, total) / per));
}

/** The page clamped into range. */
export function galleryClampPage(page: number, count: number): number {
  return Math.min(Math.max(1, Math.floor(page) || 1), Math.max(1, count));
}

/** The query for a page: "offset=25&limit=25" (+ the filter). */
export function galleryPageQuery(page: number, kind: string, per = GALLERY_PAGE): string {
  return `offset=${(Math.max(1, page) - 1) * per}&limit=${per}${kind ? `&kind=${encodeURIComponent(kind)}` : ''}`;
}

/** The numbered pages to draw: the first, the last, this one and one either side; 'gap' where pages are skipped.
 *  e.g. page 7 of 14 → [1, 'gap', 6, 7, 8, 'gap', 14]. One page → [] (no pager at all). */
export function galleryPagerSlots(page: number, count: number): Array<number | 'gap'> {
  if (count <= 1) return [];
  const want = new Set([1, count, page - 1, page, page + 1].filter((p) => p >= 1 && p <= count));
  const pages = [...want].sort((a, b) => a - b);
  const out: Array<number | 'gap'> = [];
  for (let i = 0; i < pages.length; i++) {
    if (i && pages[i] - pages[i - 1] === 2) out.push(pages[i] - 1);      // ★ a gap of ONE page is drawn as that page
    else if (i && pages[i] - pages[i - 1] > 2) out.push('gap');
    out.push(pages[i]);
  }
  return out;
}

/** "Page 2 of 14". */
export function galleryPageLabel(page: number, count: number): string { return `Page ${page} of ${count}`; }

// ── The footer: what is SHOWN, counted (2026-11-01) ─────────────────────────────────────────────────────────────────
// ★★ The Lenovo's footer said "5000 items on this server · 9.7 MB" whatever the filter: "I cannot tell you exactly how
//    many NAVTEX messages there are". With a filter, its kind's count and size come first, then the whole gallery:
//    "1,234 NAVTEX items · 2.1 MB — 5,000 items on this server · 9.7 MB. Times are UTC." With ALL, the whole only.
//    gallery.json's "kinds" ({"navtex":{"n":1234,"b":2202009},…}, vibe_gallery.h listJson) carries the counts; an older
//    server without it still says the filter's count (its paged "total"), just without the size.

/** 1234 → "1,234". */
export function galleryCount(n: number): string {
  return String(Math.max(0, Math.round(n || 0))).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
}

export type GalleryKinds = Record<string, { n: number; b: number }>;

/** The gallery's footer line. `kind` '' = ALL; `label` is the filter chip's own word ("NAVTEX"). */
export function galleryFooter(
  j: { count?: number; usedBytes?: number; total?: number; kinds?: GalleryKinds },
  kind: string, label: string, size: (bytes: number) => string, shown = 0,
): string {
  const all = j.count ?? shown;
  const whole = `${galleryCount(all)} item${all === 1 ? '' : 's'} on this server · ${size(j.usedBytes || 0)}`;
  if (!kind) return `${whole}. Times are UTC.`;
  const k = j.kinds?.[kind];
  const n = k ? k.n : j.kinds ? 0 : j.total;
  if (typeof n !== 'number') return `${whole}. Times are UTC.`;
  const part = `${galleryCount(n)} ${label} item${n === 1 ? '' : 's'}${k || j.kinds ? ` · ${size(k ? k.b : 0)}` : ''}`;
  return `${part} — ${whole}. Times are UTC.`;
}

// ── The time range (beta92, Stuart 2026-11-23: "the gallery needs some sorting last hour last day last week last
//    month buttons") ──────────────────────────────────────────────────────────────────────────────────────────────────
// ★ The same keys, words and look as the maps' time filter (ISM_RANGES: 1 H · 24 H · 7 DAYS · 30 DAYS · ALL). The server
//   narrows the page and its "total" (gallery.json?since=<seconds back>, vibe_gallery.h listJson); "count" and "kinds"
//   stay the whole store's. A server that knows since= says "since" in its answer; an older one does not, and then no
//   keys are drawn (keys that change nothing are dead controls). Remembered per browser / app; default ALL.
export const GALLERY_RANGES = [
  { id: '1h', label: '1 H', words: 'Kept in the last hour', since: 3600, phrase: 'in the last hour' },
  { id: '24h', label: '24 H', words: 'Kept in the last 24 hours', since: 86400, phrase: 'in the last 24 hours' },
  { id: '7d', label: '7 DAYS', words: 'Kept in the last 7 days', since: 7 * 86400, phrase: 'in the last 7 days' },
  { id: '30d', label: '30 DAYS', words: 'Kept in the last 30 days', since: 30 * 86400, phrase: 'in the last 30 days' },
  { id: 'all', label: 'ALL', words: 'Everything still in the gallery', since: 0, phrase: '' },
] as const;
export const GALLERY_RANGE_KEY = 'vibesdr.galleryRange';
export const GALLERY_RANGE_HINT = 'ALL is everything still in the gallery — the oldest go when it is full.';

/** A remembered choice, or ALL. */
export function galleryRangeOf(saved: string | null | undefined): string {
  return GALLERY_RANGES.some((r) => r.id === saved) ? (saved as string) : 'all';
}
/** The query's part for a range: "&since=86400"; ALL asks "&since=0" (so the server can say it knows the filter). */
export function galleryRangeQuery(range: string): string {
  const r = GALLERY_RANGES.find((x) => x.id === range);
  return `&since=${r ? r.since : 0}`;
}
/** "in the last 24 hours" ('' for ALL / unknown). */
export function galleryRangePhrase(range: string): string {
  return GALLERY_RANGES.find((x) => x.id === range)?.phrase ?? '';
}
/** Did the server honour the range? (An answer with "since" = it knows the filter.) */
export function galleryKnowsRange(j: { since?: unknown }): boolean { return typeof j.since === 'number'; }

/** The footer under a range: the view's own count, then the whole gallery —
 *  "12 WEFAX items in the last 24 hours — 5,000 items on this server · 9.7 MB. Times are UTC."
 *  ALL (or a server that did not apply it): the plain footer. */
export function galleryRangeFooter(
  j: { count?: number; usedBytes?: number; total?: number; kinds?: GalleryKinds; since?: unknown },
  kind: string, label: string, range: string, size: (bytes: number) => string, shown = 0,
): string {
  const phrase = galleryRangePhrase(range);
  if (!phrase || !j.since || typeof j.total !== 'number') return galleryFooter(j, kind, label, size, shown);
  const all = j.count ?? shown;
  const whole = `${galleryCount(all)} item${all === 1 ? '' : 's'} on this server · ${size(j.usedBytes || 0)}`;
  const n = j.total;
  return `${galleryCount(n)} ${kind ? label + ' ' : ''}item${n === 1 ? '' : 's'} ${phrase} — ${whole}. Times are UTC.`;
}
