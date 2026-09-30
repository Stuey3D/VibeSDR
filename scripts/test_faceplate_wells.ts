/**
 * Drum wells and tuner keys (src/constants/drumWell.ts + the well tokens in faceplate.ts) — the
 * chassis parameterisation of today's drum, the needle's removal, the LED pool, the notch-order
 * TRAP, the controls colour's brightness, and the tuner-keys layout.
 *
 * Brief: docs/BRIEF-faceplates.md §6.1, §6.2 (Deck.mockup `W_FACE`, `w.poolA/B`, `tk`).
 *
 * Run: node --no-warnings scripts/test_faceplate_wells.ts   (run-tests.sh does)
 */
import {
  chassisTokens, CHASSIS, CONTROLS, LED, ledA, resolveControlsColour, DEFAULT_CHASSIS,
} from '../src/constants/faceplate.ts';
import {
  POOL, poolEllipse, wellOutset, notchOrder, WELL_GLOW_BLUR, tunerKeysLayout,
  TK_KEY_FRAC, TK_KEY_FRAC_LAND,
} from '../src/constants/drumWell.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);
const near = (what: string, got: number, want: number, tol = 1e-9) =>
  ok(`${what} (got ${got}, want ${want})`, Math.abs(got - want) <= tol);

/** Relative luminance-ish of an rgba()/#hex string, weighted by its alpha over nothing. */
function rgbOf(c: string): [number, number, number, number] {
  const m = c.match(/rgba?\(([^)]+)\)/);
  if (m) { const p = m[1].split(',').map(Number); return [p[0], p[1], p[2], p[3] ?? 1]; }
  const h = c.replace('#', '');
  return [parseInt(h.slice(0, 2), 16), parseInt(h.slice(2, 4), 16), parseInt(h.slice(4, 6), 16), 1];
}
const lum = (c: string) => { const [r, g, b] = rgbOf(c); return 0.2126 * r + 0.7152 * g + 0.0722 * b; };

const D = chassisTokens('default'), S = chassisTokens('silver'), B = chassisTokens('black');

// ── §6.1 The default well IS today's drum (acceptance §13.1) ─────────────────────
eq('default face: today\'s #101410 → #060707 gradient, no grain', [D.wellFace, D.wellTexture],
   [['#101410', '#0a0c0a', '#060706'], null]);
eq('default drum gradient + stops', [D.drumBody, D.drumPos],
   [['#070807', '#191a18', '#232422', '#181917', '#050505'], [0, 0.28, 0.50, 0.74, 1]]);
eq('default ridges, rim, sheen', [D.ridgeShadow, D.ridgeHighlight, D.rimLine, D.sheen],
   ['rgba(0,0,0,0.45)', 'rgba(160,160,150,0.10)', 'rgba(180,185,175,0.14)',
    ['rgba(255,255,255,0)', 'rgba(255,255,255,0.07)', 'rgba(255,255,255,0)']]);
eq('default notches: light on dark, shadow pair +0.9 UNDER them',
   [D.notchMinor, D.notchMed, D.notchMajor, D.notchPair, D.notchPairDx, D.notchPairUnder],
   ['rgba(168,166,158,0.22)', 'rgba(168,166,158,0.36)', 'rgba(168,166,158,0.55)', 'rgba(0,0,0,0.5)', 0.9, true]);
eq('default edge: today\'s lit border (controls colour) + inner glow .10, nothing outside',
   [D.wellBorder, D.wellInnerGlowA, D.wellRingA, D.wellGlowA, D.wellTopLip], [null, 0.10, 0, 0, null]);
eq('default well draws inside its own box (canvas is today\'s size)', wellOutset(D), 0);

// ── §6.1 ★★ the needle is gone on EVERY chassis ──────────────────────────────────
for (const c of CHASSIS) {
  const keys = Object.keys(chassisTokens(c));
  eq(`${c}: no needle tokens left`, keys.filter(k => /needle/i.test(k)), []);
}

// ── §6.1 silver (brief table) ────────────────────────────────────────────────────
eq('silver drum: polished aluminium', S.drumBody, ['#4d4b46', '#a9a69f', '#e4e2dc', '#a3a09a', '#393834']);
eq('silver drum crown at 26 % (Deck.mockup)', S.drumPos, [0, 0.26, 0.50, 0.74, 1]);
eq('silver ridge highlight', S.ridgeHighlight, 'rgba(255,255,255,0.35)');
eq('silver notches: DARK cuts', [S.notchMinor, S.notchMajor], ['rgba(58,56,50,0.40)', 'rgba(38,36,32,0.70)']);
eq('silver face: brushed silver, dark 1 pt gap, ring + glow in the controls colour',
   [S.wellTexture, S.wellBorder, S.wellRingA, S.wellGlowA, S.wellInnerGlowA],
   ['silver', 'rgba(0,0,0,0.55)', 0.35, 0.40, 0]);
// ★ TRAP: on aluminium the pair INVERTS — the cut is dark and its partner is a white highlight, and
//   the highlight sits under the cut (drawn over, it would paint the cut white).
ok('silver: the cut is darker than its pair (inverted)', lum(S.notchMajor) < lum(S.notchPair));
ok('default: the notch is lighter than its pair', lum(D.notchMajor) > lum(D.notchPair));
ok('silver med notch sits between minor and major', rgbOf(S.notchMed)[3] > rgbOf(S.notchMinor)[3]
   && rgbOf(S.notchMed)[3] < rgbOf(S.notchMajor)[3]);
eq('silver pair drawn under the cuts', notchOrder(S), ['pair', 'minor', 'med', 'major']);
eq('notch order is a token: pair over when asked', notchOrder({ notchPairUnder: false }), ['minor', 'med', 'major', 'pair']);

// ── §6.1 black (the mockup wins over the brief's "same as default") ────────────
eq('black drum: neutral grey', B.drumBody, ['#070707', '#181818', '#222222', '#171717', '#050505']);
eq('black face: brushed black, #000 gap, ring .30, glow .38, lip .14',
   [B.wellTexture, B.wellBorder, B.wellRingA, B.wellGlowA, B.wellTopLip],
   ['black', '#000000', 0.30, 0.38, 'rgba(255,255,255,0.14)']);
ok('black notches stay light on dark', lum(B.notchMajor) > lum(B.notchPair));

for (const c of ['silver', 'black'] as const) {
  const t = chassisTokens(c);
  ok(`${c}: the edge canvas reaches past the 8 pt glow`, wellOutset(t) >= WELL_GLOW_BLUR);
  eq(`${c}: face base under the grain is flat (no gradient flash while it loads)`,
     new Set(t.wellFace).size, 1);
}

// ── §6.1 the LED pool: 60 % × 75 % at 50 % 18 %, α .16 → .05 at 55 % → 0 ─────────
eq('pool numbers', [POOL.rx, POOL.ry, POOL.cx, POOL.cy, POOL.alphas, POOL.positions],
   [0.60, 0.75, 0.50, 0.18, [0.16, 0.05], [0, 0.55, 1]]);
{
  const e = poolEllipse(1, 24, 158, 35);
  near('pool centre x = middle of the drum', e.cx, 1 + 79);
  near('pool centre y = 18 % down the drum', e.cy, 24 + 35 * 0.18);
  near('pool radius x = 60 % of the drum width', e.rx, 158 * 0.6);
  near('pool radius y = 75 % of the drum height', e.ry, 35 * 0.75);
}

// ── §6.1 ★★ G() TRAP: the controls colour at the RIGHT brightness, every colour ─
// hsl(GLOW_HUE, 100, 45) could not make white or neon and got blue/amber wrong. Every pool / edge /
// icon alpha now comes from the brief's RGB triplet — only today's default-chassis green keeps its
// exact hsl (so the default drum is unchanged).
for (const chassis of CHASSIS) {
  for (const c of CONTROLS) {
    const led = resolveControlsColour(chassis, c);
    const s = ledA(led, POOL.alphas[0]);
    if (chassis === 'default' && c === 'green') {
      eq('default green keeps today\'s drum hsl', s, 'hsla(120,100%,45%,0.16)');
      continue;
    }
    eq(`${chassis} ${c}: the pool is the brief's triplet`, s, `rgba(${LED[c].rgb},0.16)`);
  }
}
eq('white is white (all three channels high)', rgbOf(ledA(LED.white, 1)).slice(0, 3), [215, 228, 255]);
eq('neon is the neon orange', rgbOf(ledA(LED.neon, 1)).slice(0, 3), [255, 106, 20]);
eq('amber is full-brightness amber', rgbOf(ledA(LED.amber, 1)).slice(0, 3), [255, 174, 26]);
eq('blue is the brief\'s blue', rgbOf(ledA(LED.blue, 1)).slice(0, 3), [61, 155, 255]);

// ── §6.2 tuner-keys mode ─────────────────────────────────────────────────────────
eq('keys well faces (Deck.mockup tk.bg)', [D.keysFace, S.keysFace, B.keysFace], ['#0b0a08', '#c9c6bf', '#1b1c1e']);
eq('only the default key needs its own dark slot', [D.keysSlot, S.keysSlot, B.keysSlot], ['#050403', null, null]);
eq('the keys well keeps the drum well\'s edge exactly (same tokens, one component)',
   [S.wellRingA, S.wellGlowA, S.wellBorder], [0.35, 0.40, 'rgba(0,0,0,0.55)']);
for (const [W, H, land] of [[160, 60, false], [183, 60, false], [140, 45, false],
                            [80, 32, true], [120, 44, true], [220, 51, true]] as const) {
  const L = tunerKeysLayout(W, H, land);
  const frac = land ? TK_KEY_FRAC_LAND : TK_KEY_FRAC;
  const tag = `${land ? 'landscape' : 'portrait'} ${W}×${H}`;
  near(`${tag}: keys are ${frac * 100} % of the well's inner width`, L.keyW, (W - 2 * L.pad) * frac);
  eq(`${tag}: padding`, L.pad, land ? 6 : 8);
  eq(`${tag}: full height inside the padding`, L.keyH, H - 2 * L.pad);
  near(`${tag}: right key flush to the padding`, L.rightX + L.keyW, W - L.pad);
  ok(`${tag}: the keys never overlap`, L.leftX + L.keyW < L.rightX);
  ok(`${tag}: the glyph fits between the keys`,
     L.leftX + L.keyW <= L.glyphCx - L.glyphSz / 2 && L.glyphCx + L.glyphSz / 2 <= L.rightX);
  ok(`${tag}: glyph no larger than the mockup's 24 pt`, L.glyphSz <= 24);
}
{
  const r = (n: number) => Math.round(n * 0.75);
  eq('padding scales with the deck (useUiScale r)', tunerKeysLayout(160, 45, false, r).pad, 6);
}
// The DEFAULT_CHASSIS object must still carry every well token the components read.
for (const k of ['wellFace', 'drumBody', 'drumPos', 'notchPair', 'notchPairDx', 'notchPairUnder',
                 'keysFace', 'keysSlot', 'wellRingA', 'wellGlowA'] as const) {
  ok(`DEFAULT_CHASSIS has ${k}`, (DEFAULT_CHASSIS as any)[k] !== undefined);
}

console.log(`faceplate wells: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
