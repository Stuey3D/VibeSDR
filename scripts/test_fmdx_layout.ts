/**
 * The FM-DX screen's big-window layout — src/constants/fmdxLayout.ts.
 *
 * Proves:
 *   • A PHONE-SIZED WINDOW GETS TODAY'S LAYOUT EXACTLY (dial 158, logo 68, scales 1) — SE in Display Zoom,
 *     every iPhone portrait however tall, every landscape phone, a narrow iPad split-view column;
 *   • a big window (Stuart's iPad build full screen on the Mac) grows the dial and the logo substantially;
 *   • the growth never exceeds the window's SPARE height (the cards below still fit), never passes its caps,
 *     and the logo leaves the PI and TP · TA · AF columns room;
 *   • it is CONTINUOUS and monotonic: no jump > a few pt on a 1 pt resize, and a bigger window never shrinks it.
 *
 * Run: node --no-warnings scripts/test_fmdx_layout.ts   (run-tests.sh does)
 */
import {
  fmdxLayout, COMPACT, DIAL_H_COMPACT, DIAL_H_MAX, LOGO_BOX_COMPACT, LOGO_BOX_MAX, ID_CARD_COMPACT,
  BELOW_RESERVE, DIAL_FONT_MAX,
} from '../src/constants/fmdxLayout.ts';

let fails = 0, passes = 0;
function ok(what: string, cond: boolean, detail = '') {
  if (cond) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}${detail ? `\n   ${detail}` : ''}`);
}
const same = (a: object, b: object) => JSON.stringify(a) === JSON.stringify(b);

// The scroll area on the FM-DX screen: window less status bar + 44 pt header, less 14 top padding, less the
// bottom padding (14 + island + VTS strip + 8). Island ≈ 190 + home indicator, strip ≈ 64.
function avail(winH: number, top: number, bottom: number) {
  return winH - top - 44 - 14 - (14 + 190 + bottom + 64 + 8);
}
// content width = window − 28 side padding − horizontal insets
type Case = { name: string; w: number; h: number; top: number; bottom: number; side?: number; grows: boolean | 'some' };
const CASES: Case[] = [
  { name: 'SE Display Zoom 320×568',         w: 320,  h: 568,  top: 20, bottom: 0,  grows: false },
  { name: 'iPhone 17 390×844',               w: 390,  h: 844,  top: 47, bottom: 34, grows: false },
  { name: 'iPhone 17 Pro Max 440×956',       w: 440,  h: 956,  top: 59, bottom: 34, grows: false },
  { name: 'Pro Max landscape 956×440',       w: 956,  h: 440,  top: 0,  bottom: 21, side: 59, grows: false },
  { name: 'SE landscape 568×320',            w: 568,  h: 320,  top: 0,  bottom: 0,  grows: false },
  { name: 'iPad split view 320×1024',        w: 320,  h: 1024, top: 24, bottom: 20, grows: false },
  { name: 'iPad split view 507×1024',        w: 507,  h: 1024, top: 24, bottom: 20, grows: false },
  { name: 'iPad portrait 768×1024',          w: 768,  h: 1024, top: 24, bottom: 20, grows: 'some' },
  { name: 'iPad 11" landscape 1194×834',     w: 1194, h: 834,  top: 24, bottom: 20, grows: 'some' },
  { name: 'iPad 13" landscape 1366×1024',    w: 1366, h: 1024, top: 24, bottom: 20, grows: 'some' },
  { name: 'iPad 13" portrait 1024×1366',     w: 1024, h: 1366, top: 24, bottom: 20, grows: true },
  { name: 'Mac full screen 2000×1100',       w: 2000, h: 1100, top: 28, bottom: 0,  grows: true },
  { name: 'Mac short wide window 2000×600',  w: 2000, h: 600,  top: 28, bottom: 0,  grows: false },
];
for (const c of CASES) {
  const cw = c.w - 28 - 2 * (c.side ?? 0);
  const ah = avail(c.h, c.top, c.bottom);
  const L = fmdxLayout(cw, ah);
  const tag = `${c.name} (content ${cw}×${ah})`;
  console.log(`  ${tag.padEnd(52)} dial ${L.dialH} ×${L.dialScale}  logo ${L.logoBox} ×${L.cardScale}`);
  if (!c.grows) { ok(`${tag}: today's layout exactly`, same(L, COMPACT), JSON.stringify(L)); continue; }
  if (c.grows === true) {
    ok(`${tag}: the dial grows`, L.dialH > DIAL_H_COMPACT + 40, `${L.dialH}`);
    ok(`${tag}: the logo grows`, L.logoBox > LOGO_BOX_COMPACT + 40, `${L.logoBox}`);
  }
  ok(`${tag}: caps hold`, L.dialH <= DIAL_H_MAX && L.logoBox <= LOGO_BOX_MAX && L.dialScale <= DIAL_FONT_MAX && L.cardScale <= 1.6);
  ok(`${tag}: growth fits the spare height`,
     (L.dialH - DIAL_H_COMPACT) + (L.logoBox - LOGO_BOX_COMPACT) <= Math.max(0, ah - DIAL_H_COMPACT - ID_CARD_COMPACT - 24 - BELOW_RESERVE) + 1);
  ok(`${tag}: the logo leaves the side columns ≥ 100 pt each`, (cw - 28 - 20 - L.logoBox) / 2 >= 100);
}
// THE SCREEN STUART SENT (2026-10-04, iPad build full screen on the Mac): ~403 pt of cards (dial 158, band chip,
// station card 96, transmitter) and ~550 pt EMPTY above the deck ⇒ ~950 pt available. That is the case the
// brief is about: it must reach (or near) the caps, and still leave the reserve for the cards below.
{
  const cw = 1512 - 28, ah = 403 + 550;
  const L = fmdxLayout(cw, ah);
  console.log(`  ${"Stuart's screenshot (content " + cw + '×' + ah + ')'}`.padEnd(54) + ` dial ${L.dialH} ×${L.dialScale}  logo ${L.logoBox} ×${L.cardScale}`);
  ok("Stuart's screenshot: dial ≥ 320 and logo ≥ 200", L.dialH >= 320 && L.logoBox >= 200, JSON.stringify(L));
  ok("Stuart's screenshot: the cards still fit", L.dialH + 12 + 28 + 12 + (L.logoBox + 28) + 12 + 85 <= ah, JSON.stringify(L));
}
// Unmeasured (first frame) and nonsense inputs are compact.
ok('unmeasured = compact', same(fmdxLayout(0, 0), COMPACT) && same(fmdxLayout(-5, 900), COMPACT) && same(fmdxLayout(NaN, NaN), COMPACT));

// Continuous + monotonic, sweeping width and height.
{
  let maxJump = 0, mono = true;
  for (let h = 300; h <= 1400; h += 50) {
    let prev = fmdxLayout(200, h);
    for (let w = 201; w <= 2000; w++) {
      const L = fmdxLayout(w, h);
      maxJump = Math.max(maxJump, Math.abs(L.dialH - prev.dialH), Math.abs(L.logoBox - prev.logoBox));
      if (L.dialH < prev.dialH || L.logoBox < prev.logoBox - 1) mono = false;   // −1: the logo's share of a rounded split
      prev = L;
    }
  }
  for (let w = 300; w <= 2000; w += 100) {
    let prev = fmdxLayout(w, 100);
    for (let h = 101; h <= 1400; h++) {
      const L = fmdxLayout(w, h);
      maxJump = Math.max(maxJump, Math.abs(L.dialH - prev.dialH), Math.abs(L.logoBox - prev.logoBox));
      if (L.dialH < prev.dialH || L.logoBox < prev.logoBox) mono = false;
      prev = L;
    }
  }
  ok('a 1 pt resize never moves the dial or logo by more than 3 pt', maxJump <= 3, `max jump ${maxJump}`);
  ok('a bigger window never shrinks the dial or logo', mono);
}

console.log(`fmdxLayout: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
