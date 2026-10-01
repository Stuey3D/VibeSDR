/**
 * Sprite sizes never reach Metal as NaN, zero, negative or absurd (src/constants/spriteSizing.ts).
 *
 * ★★★ WHY: 11 B7/B8 — choosing the VCR display crashed the iPhone instantly (TestFlight report
 * 2026-10-01 02:32: SIGABRT in Metal under RNSkia JsiSkSurfaceFactory::MakeOffscreen). The 7-segment
 * box is unmeasured on its first render (0 × 0); the fit-to-width step divided by a zero cell width and
 * produced NaN, which passed `sh <= 0` and became a NaN-sized offscreen surface. Metal aborts for that;
 * Android's GL returns no surface, so the Android checks never saw it.
 *
 * Run: node --no-warnings scripts/test_sprite_sizing.ts   (run-tests.sh does)
 */
import { segFit, spritePixels, SPRITE_MAX_PX, SEG_CELL_W, SEG_CELL_H } from '../src/constants/spriteSizing.ts';

let fails = 0, passes = 0;
function ok(what: string, cond: boolean) {
  if (cond) { passes++; return; }
  fails++; console.log(`  FAIL ${what}`);
}

// ── The bug, reproduced with the formula as it shipped in 356/357 ──────────────────────────────────
function oldFit(w: number, h: number, n: number, gap: number, designH: number) {
  let sh = Math.max(0, Math.min(designH, h - 4));
  let cw = (SEG_CELL_W * sh) / SEG_CELL_H;
  const need = n * cw + (n - 1) * gap;
  if (need > w && n > 0) { const f = (w - (n - 1) * gap) / (n * cw); sh *= f; cw *= f; }
  return Math.round(sh * 2) / 2;
}
const shOld = oldFit(0, 0, 7, 1, 30);
ok('the shipped formula gives NaN on an unmeasured box (the test can see the bug)', Number.isNaN(shOld));
ok('…and NaN passed the old `sh <= 0` guard', !(shOld <= 0));
ok('the old makeSprite clamp let NaN through: Math.max(1, NaN) is NaN', Number.isNaN(Math.max(1, Math.ceil(NaN * 3))));

// ── segFit: always finite, never negative, 0 when nothing fits ─────────────────────────────────────
const sizes = [0, 0.5, 1, 3, 4, 4.5, 5, 10, 20, 37, 60, 120, 390, 932, NaN, Infinity, -5];
for (const w of sizes) for (const h of sizes) for (const n of [0, 1, 7, 9, 12]) for (const designH of [27, 30]) {
  const { sh, cw } = segFit(w, h, n, 1, designH);
  const tag = `segFit(w=${w}, h=${h}, n=${n}, designH=${designH}) = ${sh}/${cw}`;
  ok(`${tag} finite`, Number.isFinite(sh) && Number.isFinite(cw));
  ok(`${tag} not negative`, sh >= 0 && cw >= 0);
  ok(`${tag} within the design height`, sh <= designH);
  if (sh > 0) {
    // what useSegSprites asks for: the cell plus a 6 pt glow margin — must be a drawable sprite
    const k = sh / SEG_CELL_H;
    ok(`${tag} gives a drawable sprite`, spritePixels(SEG_CELL_W * k + 12, SEG_CELL_H * k + 12, 3) !== null);
  }
}
// a real box still fits as before
const real = segFit(240, 34, 7, 1, 30);
ok('a 240 × 34 box gets the full 30 pt design height', real.sh === 30);
const narrow = segFit(100, 34, 7, 1, 30);
// (rounding the height to the nearest half point can overshoot by up to n × 0.25 × 24/38 pt — as it always has)
ok('a narrow box narrows the digits to fit', narrow.sh > 0 && narrow.sh < 30 && 7 * narrow.cw + 6 <= 100 + 7 * 0.25 * SEG_CELL_W / SEG_CELL_H + 1e-9);

// ── spritePixels: the last line before Metal ───────────────────────────────────────────────────────
for (const bad of [NaN, Infinity, -Infinity, 0, -1]) {
  ok(`spritePixels refuses w=${bad}`, spritePixels(bad, 10, 3) === null);
  ok(`spritePixels refuses h=${bad}`, spritePixels(10, bad, 3) === null);
  ok(`spritePixels refuses pr=${bad}`, spritePixels(10, 10, bad) === null);
}
ok('spritePixels refuses an absurd side', spritePixels(SPRITE_MAX_PX, 10, 3) === null);
const px = spritePixels(36.5, 50.2, 3);
ok('spritePixels rounds a real sprite up at the pixel ratio', px !== null && px.pw === 110 && px.ph === 151);
ok('spritePixels keeps a tiny sprite at least 1 px', JSON.stringify(spritePixels(0.1, 0.1, 1)) === '{"pw":1,"ph":1}');

console.log(`sprite sizing: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
