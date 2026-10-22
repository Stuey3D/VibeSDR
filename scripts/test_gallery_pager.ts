// test_gallery_pager.ts — the gallery's pages (src/utils/galleryPager.ts, V12 beta 4).
import { galleryPageCount, galleryClampPage, galleryPageQuery, galleryPagerSlots, galleryPageLabel, GALLERY_PAGE }
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
console.log(`gallery pager: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
