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
