// test_gallery_pager.ts — the gallery's pages (src/utils/galleryPager.ts, V12 beta 4).
import { galleryPageCount, galleryClampPage, galleryPageQuery, galleryPagerSlots, galleryPageLabel, GALLERY_PAGE, galleryCount, galleryFooter }
  from '../src/utils/galleryPager.ts';
let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };
const eq = (a: unknown, b: unknown, m: string) => ok(JSON.stringify(a) === JSON.stringify(b), `${m}: ${JSON.stringify(a)}`);

ok(GALLERY_PAGE === 25, '25 a page');
eq([galleryPageCount(0), galleryPageCount(25), galleryPageCount(26), galleryPageCount(340)], [1, 1, 2, 14], 'page counts');
eq([galleryClampPage(0, 14), galleryClampPage(20, 14), galleryClampPage(3, 14), galleryClampPage(5, 0)], [1, 14, 3, 1], 'clamped');
eq(galleryPageQuery(1, ''), 'offset=0&limit=25', 'page 1, all');
eq(galleryPageQuery(3, 'ft8'), 'offset=50&limit=25&kind=ft8', 'page 3, FT8 (the digit kept)');
eq(galleryPagerSlots(1, 1), [], 'one page: no pager');
eq(galleryPagerSlots(1, 2), [1, 2], 'two pages');
eq(galleryPagerSlots(7, 14), [1, 'gap', 6, 7, 8, 'gap', 14], 'the middle: first, around, last');
eq(galleryPagerSlots(1, 14), [1, 2, 'gap', 14], 'the first page');
eq(galleryPagerSlots(14, 14), [1, 'gap', 13, 14], 'the last page');
eq(galleryPagerSlots(3, 14), [1, 2, 3, 4, 'gap', 14], 'a gap of one page is drawn as the page');
eq(galleryPagerSlots(2, 5), [1, 2, 3, 4, 5], 'five pages: all drawn');
ok(galleryPageLabel(2, 14) === 'Page 2 of 14', 'label');
// ★★ The footer counts what is shown (2026-11-01, the Lenovo's "5000 … no matter what I choose")
const mb = (b: number) => b >= 1_048_576 ? `${(b / 1_048_576).toFixed(1)} MB` : b >= 1024 ? `${Math.round(b / 1024)} KB` : `${b} B`;
const J = { count: 5000, usedBytes: 10171187, total: 1234, kinds: { navtex: { n: 1234, b: 2202010 }, wefax: { n: 1, b: 400000 } } };
eq([galleryCount(0), galleryCount(999), galleryCount(1234), galleryCount(1234567)], ['0', '999', '1,234', '1,234,567'], 'thousands');
eq(galleryFooter(J, 'navtex', 'NAVTEX', mb), '1,234 NAVTEX items · 2.1 MB — 5,000 items on this server · 9.7 MB. Times are UTC.', 'a filter: its kind, then the whole');
eq(galleryFooter(J, '', 'ALL', mb), '5,000 items on this server · 9.7 MB. Times are UTC.', 'ALL: the whole only');
eq(galleryFooter(J, 'wefax', 'WEFAX', mb), '1 WEFAX item · 391 KB — 5,000 items on this server · 9.7 MB. Times are UTC.', 'one item: singular');
eq(galleryFooter({ ...J, total: 0 }, 'sstv', 'SSTV', mb), '0 SSTV items · 0 B — 5,000 items on this server · 9.7 MB. Times are UTC.', 'a kind with nothing: 0');
eq(galleryFooter({ count: 5000, usedBytes: 10171187, total: 77 }, 'navtex', 'NAVTEX', mb), '77 NAVTEX items — 5,000 items on this server · 9.7 MB. Times are UTC.',
  'an older server (no "kinds"): the filter\'s total, no size');
eq(galleryFooter({ count: 300, usedBytes: 1024 }, 'navtex', 'NAVTEX', mb), '300 items on this server · 1 KB. Times are UTC.', 'a server with no pages: the whole only');
console.log(`gallery pager: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
