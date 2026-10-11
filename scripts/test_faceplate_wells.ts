/**
 * Drum wells and tuner keys (src/constants/drumWell.ts + the well tokens in faceplate.ts) — the
 * chassis parameterisation of today's drum, the needle's removal, the LED pool, the notch-order
 * TRAP, the controls colour's brightness, and the tuner-keys layout. And (2026-10-01) the ring round
 * the wells is GONE, and every front-panel key is lit by the light coming up out of its panel gap
 * (src/constants/keyLight.ts): its reach, its fit in the deck's tightest gaps, its colour, the pressed
 * flood, the etched glyph's glow — and that unlit means NONE.
 *
 * Brief: docs/BRIEF-faceplates.md §6.1, §6.2 (Deck.mockup `W_FACE`, `w.poolA/B`, `tk`).
 *
 * Run: node --no-warnings scripts/test_faceplate_wells.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import {
  chassisTokens, CHASSIS, CONTROLS, LED, ledA, hotA, resolveControlsColour, DEFAULT_CHASSIS,
} from '../src/constants/faceplate.ts';
import * as drumWell from '../src/constants/drumWell.ts';
import {
  POOL, poolEllipse, notchOrder, tunerKeysLayout, TK_KEY_FRAC, TK_KEY_FRAC_LAND,
} from '../src/constants/drumWell.ts';
import {
  KEY_LIGHT, KEY_PRESS_LIGHT, ETCH_LIGHT, DECK_MIN_GAP, keyLightReach, keyLightExtent, keyLightAt,
  keyLightSprite, keyLightLayers, domeCap, drawKeyLight, drawKeyWash, type RR,
} from '../src/constants/keyLight.ts';
import { landscapeDeck, portraitDeck, type MeterKind } from '../src/constants/meters.ts';

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
eq('default edge: none — the lit border and its inner glow went with the ring (2026-10-01)',
   [D.wellBorder, D.wellTopLip], [null, null]);

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
eq('silver face: brushed silver and the drum\'s dark 1 pt cut (no ring, no glow)',
   [S.wellTexture, S.wellBorder], ['silver', 'rgba(0,0,0,0.55)']);
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
eq('black face: brushed black, #000 cut, lip .14 (no ring, no glow)',
   [B.wellTexture, B.wellBorder, B.wellTopLip], ['black', '#000000', 'rgba(255,255,255,0.14)']);
ok('black notches stay light on dark', lum(B.notchMajor) > lum(B.notchPair));

for (const c of ['silver', 'black'] as const) {
  const t = chassisTokens(c);
  eq(`${c}: face base under the grain is flat (no gradient flash while it loads)`,
     new Set(t.wellFace).size, 1);
}

// ── ★★★ THE RING IS GONE (Stuart, 2026-10-01: "try removing the outline ring that surrounds both buttons
//     and replace it with a glow coming up in the panel gap around each button") — on every chassis, lit
//     or not: the tokens and the constants went with it, so nothing can draw it back ──────────────────
for (const c of CHASSIS) {
  const keys = Object.keys(chassisTokens(c));
  eq(`${c}: no ring / glow / inner-glow tokens left`, keys.filter(k => /^well(Ring|Glow|InnerGlow)/.test(k)), []);
}
eq('drumWell.ts: no ring light, ring rect, glow blur or outset left',
   ['RING_LIGHT', 'ringRect', 'WELL_GLOW_BLUR', 'wellOutset'].filter(k => k in drumWell), []);
const SRC = (f: string) => readFileSync(new URL(`../src/${f}`, import.meta.url), 'utf8');
const code = (f: string) => SRC(f).replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*$/gm, '');
{
  const dw = code('components/DrumWell.tsx');
  ok('DrumWell: the well edge draws no blur (no ring glow, no live BlurMask)', !/BlurMask/.test(dw));
  ok('DrumWell: WellEdge takes no colour — nothing in it is lit', /export function WellEdge\(\{ W, H, ct \}/.test(dw));
  const tk = code('components/TunerKeys.tsx');
  ok('TunerKeys: no WellEdge round the keys well', !/WellEdge/.test(tk));
  ok('DrumWheel: the well canvas is its own size again (no outset)', !/wellOutset|left: -M/.test(code('components/DrumWheel.tsx')));
}

// ── ★★★ THE PANEL-GAP LIGHT (constants/keyLight.ts) ────────────────────────────────────────────────
// The light at its full reach: soft, warm, and a FALLOFF — never a line, never a box.
{
  const L = keyLightLayers(KEY_LIGHT.reach);
  ok('light: subtle — no outside layer above α .35 (not neon)', L.edge.a <= 0.35 && L.spill.a <= 0.35 && L.bottom.a <= 0.35);
  ok('light: the spill is wider, softer and fainter than the edge (a falloff, not a second line)',
     L.spill.w > L.edge.w && L.spill.blur > L.edge.blur && L.spill.a < L.edge.a);
  ok('light: the bottom\'s extra is fainter than the spill (subtle)', L.bottom.a < L.spill.a);
  ok('light: brightest AT the edge, dying away outward',
     keyLightAt(KEY_LIGHT.reach, 0) > keyLightAt(KEY_LIGHT.reach, 1)
     && keyLightAt(KEY_LIGHT.reach, 1) > keyLightAt(KEY_LIGHT.reach, 2)
     && keyLightAt(KEY_LIGHT.reach, 2) > keyLightAt(KEY_LIGHT.reach, 3));
  ok('light: in a slot, brightest in the GAP against the cap (it comes from under the key)',
     keyLightAt(KEY_LIGHT.reach, -1.6, 2) > keyLightAt(KEY_LIGHT.reach, 0, 2));
  ok('light: gone (< 1 %) by the end of its reach', keyLightAt(KEY_LIGHT.reach, KEY_LIGHT.reach) < 0.01);
}
// ★★ The sprite reaches past the light, at every reach, at rest and pressed — the light is never clipped.
for (let r = KEY_LIGHT.minReach; r <= KEY_LIGHT.reach; r += 0.25) {
  ok(`reach ${r}: every layer ends inside the sprite (extent ${keyLightExtent(r).toFixed(2)})`, keyLightExtent(r) <= r + 1e-9);
  ok(`reach ${r}: so does the pressed flood (extent ${keyLightExtent(r, true).toFixed(2)})`, keyLightExtent(r, true) <= r + 1e-9);
}
{
  const cut = { x: -2, y: -1.5, w: 60, h: 40, r: 8 };
  eq('the sprite is the cut-out plus the reach on every side', keyLightSprite(cut, 3), { x: -5, y: -4.5, w: 66, h: 46 });
}
// ★ Never NaN, never a sprite for nothing (spriteSizing refuses NaN / 0 — the light must never ask).
for (const v of [NaN, Infinity, -1, 0, 1, 1.9]) eq(`keyLightReach(${v}) = 0 (no light, no sprite)`, keyLightReach(v), 0);
ok('keyLightReach caps at the full reach', keyLightReach(100) === KEY_LIGHT.reach);

// ★★★ IT FITS THE DECK'S GAPS — portrait and landscape, every chassis × meter, from the SE in Display
//     Zoom (320 / 568 × 320) up. Two neighbours' lights meet in the middle of the gap: there the light must
//     stay well under the light at either key's edge (two lit edges, never one glowing bar), and it must be
//     gone before it reaches the neighbour.
{
  const scaleP = (W: number) => Math.max(0.75, Math.min(1.45, W / 390));
  const scaleL = (W: number) => Math.max(0.58, Math.min(1.45, W / 926));
  const gaps: Array<[string, number, number, { w: number; h: number }[], { W: number; H: number; land: boolean; r: (n: number) => number }[]]> = [];
  for (const W of [320, 375, 390, 430, 768]) {
    const sc = scaleP(W), r = (n: number) => Math.round(n * sc);
    const ROW = r(7), COL = r(8);
    for (const c of CHASSIS) for (const meter of ['bar', 'vu', 'edge'] as MeterKind[]) {
      const t = chassisTokens(c);
      const dl = portraitDeck({ cap: t.dome.look === 'cap', meter, shared: false, tablet: W >= 768, rowGap: ROW, r });
      const inner = W - 16 - 2 * r(12);
      const kw = (inner - 3 * COL) / 4;
      gaps.push([`portrait ${W} ${c} ${meter}`, ROW, COL, [{ w: kw, h: dl.keySlot }],
                 [{ W: (inner - COL) / 2, H: r(60), land: false, r }]]);
    }
  }
  for (const [W, H] of [[568, 320], [667, 375], [844, 390], [926, 428], [1366, 1024]]) {
    const sc = scaleL(W), r = (n: number) => Math.round(n * sc);
    for (const c of CHASSIS) for (const meter of ['bar', 'vu', 'edge'] as MeterKind[]) {
      const t = chassisTokens(c);
      const d = landscapeDeck({ plate: t.plate ? { screws: t.plate.screws, gloss: t.plate.gloss } : null, meter,
                                tablet: Math.min(W, H) >= 768, W, H, scale: sc, r });
      gaps.push([`landscape ${W}×${H} ${c} ${meter}`, d.rowGap, d.colGap, [{ w: d.keyW, h: d.keyH }],
                 [{ W: d.drumW, H: d.bandH, land: true, r }]]);
    }
  }
  let tightest = Infinity;
  for (const [tag, row, col, keys, wells] of gaps) {
    const gap = Math.min(row, col);
    tightest = Math.min(tightest, gap);
    ok(`${tag}: the deck's gap (${gap}) is never under DECK_MIN_GAP`, gap >= DECK_MIN_GAP);
    const reach = keyLightReach(gap);   // ControlsBar: keyLightReach(min(ROW_GAP, COL_GAP))
    ok(`${tag}: the main keys are lit (reach ${reach})`, reach >= KEY_LIGHT.minReach);
    ok(`${tag}: the light stays inside the gap`, keyLightExtent(reach, true) <= gap);
    for (const slotGap of [0, 2]) {
      const edge = keyLightAt(reach, 0, slotGap), mid = keyLightAt(reach, gap / 2, slotGap);
      const meet = 1 - (1 - mid) * (1 - mid);
      ok(`${tag} (slot ${slotGap}): where two lights meet it is under half the edge's (${(meet / edge).toFixed(2)})`, meet <= 0.5 * edge);
      ok(`${tag} (slot ${slotGap}): gone before the neighbour (${keyLightAt(reach, gap, slotGap).toFixed(4)})`, keyLightAt(reach, gap, slotGap) < 0.01);
    }
    // The pressed wash: the key's middle stays the key — the band and its blur end before the centre.
    for (const k of keys) {
      const cap = domeCap(k.w, k.h, 8, false);
      for (const face of [cap, { w: k.w - 2, h: k.h - 2 }]) {
        const m = Math.min(face.w, face.h);
        const blur = Math.min(KEY_PRESS_LIGHT.wash.blurMax, m * KEY_PRESS_LIGHT.wash.blurFrac);
        const band = Math.min(KEY_PRESS_LIGHT.wash.w, m * KEY_PRESS_LIGHT.wash.wFrac);
        ok(`${tag}: the pressed wash leaves the key's middle dark (${m.toFixed(1)} pt face)`, band + blur <= m / 2);
      }
    }
    // The tuner keys: TunerKeys takes keyLightReach(pad + DECK_MIN_GAP); the two keys' lights never meet
    // across the glyph, and the etched glyph's glow stays inside the well.
    for (const w of wells) {
      const L = tunerKeysLayout(w.W, w.H, w.land, w.r);
      const tr = keyLightReach(L.pad + DECK_MIN_GAP);
      ok(`${tag} tuner: lit (reach ${tr})`, tr >= KEY_LIGHT.minReach);
      ok(`${tag} tuner: the light ends before the deck's neighbour (pad ${L.pad} + gap ${gap})`, keyLightExtent(tr, true) <= L.pad + gap);
      ok(`${tag} tuner: the two keys' lights never meet across the glyph`, L.rightX - (L.leftX + L.keyW) >= 2 * tr);
      ok(`${tag} tuner: the etched glyph's glow stays inside the well`,
         L.glyphCy - L.glyphSz / 2 - ETCH_LIGHT.reach >= 0 && L.glyphCx - L.glyphSz / 2 - ETCH_LIGHT.reach >= 0);
      // ★★ DAB (2026-10-05): "MUX 12B" in a row ABOVE the keys. The well keeps its size (the zoom control and
      //    the deck do not move); the keys give up the row's height and nothing else — same x, same width, same
      //    bottom padding (so their light still ends before the neighbour) — and nothing overlaps.
      const D = tunerKeysLayout(w.W, w.H, w.land, w.r, true);
      const dt = `${tag} tuner (DAB label ${w.W.toFixed(0)}×${w.H})`;
      ok(`${dt}: the label row is inside the well, above the keys`,
         D.labelY >= 0 && D.labelH >= drumWell.TK_LABEL_MIN && D.labelY + D.labelH <= D.keyY);
      ok(`${dt}: the keys keep their x, width and bottom padding`,
         D.leftX === L.leftX && D.rightX === L.rightX && D.keyW === L.keyW && D.keyY + D.keyH === L.pad + L.keyH);
      ok(`${dt}: the keys stay keys (${D.keyH} of ${L.keyH} pt, ≥ 70 % and ≥ 20 pt)`,
         D.keyH >= 0.7 * L.keyH && D.keyH >= 20);
      ok(`${dt}: the glyph is back between the keys, inside their height`,
         L.leftX + L.keyW <= D.glyphCx - D.glyphSz / 2 && D.glyphCx + D.glyphSz / 2 <= L.rightX
         && D.glyphCy - D.glyphSz / 2 >= D.keyY && D.glyphCy + D.glyphSz / 2 <= D.keyY + D.keyH);
      ok(`${dt}: the etched glyph's glow stays inside the well`,
         D.glyphCy + D.glyphSz / 2 + ETCH_LIGHT.reach <= w.H && D.glyphCx - D.glyphSz / 2 - ETCH_LIGHT.reach >= 0);
    }
  }
  eq('the tightest deck gap is the SE\'s landscape row (4 pt) — DECK_MIN_GAP', tightest, DECK_MIN_GAP);
}

// ★★ The etched glyph between the tuner keys: TODAY'S backlit look, kept exactly (Stuart: "must stay
//    BACKLIT exactly as they are now"), plus a gentle bleed onto the case — wider and fainter.
eq('etched glyph: today\'s halo (2.6 wide, blur 3, α .55)', [ETCH_LIGHT.glow.width, ETCH_LIGHT.glow.blur, ETCH_LIGHT.glow.a], [2.6, 3, 0.55]);
ok('etched glyph: the bleed is wider, softer and fainter than the halo',
   ETCH_LIGHT.bleed.width > ETCH_LIGHT.glow.width && ETCH_LIGHT.bleed.blur > ETCH_LIGHT.glow.blur && ETCH_LIGHT.bleed.a < ETCH_LIGHT.glow.a);
eq('etched glyph: its reach is the bleed\'s half-width + blur', ETCH_LIGHT.reach, ETCH_LIGHT.bleed.width / 2 + ETCH_LIGHT.bleed.blur);

// ── The draw itself, through a recording Skia: the colour, the cap kept dark, the pressed geometry ──
type Op = { op: string; rr?: any; color?: string; clipOp?: number; sigma?: number; stroke?: number };
function recorder() {
  const ops: Op[] = [];
  const Sk: any = {
    Paint: () => { const p: any = { color: '', sigma: 0, stroke: undefined };
      p.setAntiAlias = () => {}; p.setColor = (c: string) => { p.color = c; };
      p.setMaskFilter = (m: any) => { p.sigma = m.sigma; }; p.setStyle = () => {}; p.setStrokeWidth = (w: number) => { p.stroke = w; };
      return p; },
    Color: (c: string) => c,
    MaskFilter: { MakeBlur: (_s: number, sigma: number) => ({ sigma }) },
    XYWHRect: (x: number, y: number, width: number, height: number) => ({ x, y, width, height }),
    RRectXY: (rect: any, rx: number) => ({ ...rect, rx }),
  };
  const c: any = {
    save: () => ops.push({ op: 'save' }), restore: () => ops.push({ op: 'restore' }),
    clipRRect: (rr: any, clipOp: number) => ops.push({ op: 'clipRRect', rr, clipOp }),
    clipRect: (rr: any, clipOp: number) => ops.push({ op: 'clipRect', rr, clipOp }),
    drawRRect: (rr: any, p: any) => ops.push({ op: 'drawRRect', rr, color: p.color, sigma: p.sigma, stroke: p.stroke }),
  };
  return { Sk, c, ops };
}
{
  const cut: RR = { x: 0, y: 0, w: 70, h: 48, r: 10 };
  const cap = domeCap(70, 48, 10, false);
  eq('a cap key\'s cap: inset 2 at the sides, 1.5 at the top, 4 pt shorter (DomeKey)', cap, { x: 2, y: 1.5, w: 66, h: 44, r: 8 });
  eq('an outline key IS its cap (no slot)', domeCap(70, 36, 4, true), { x: 0, y: 0, w: 70, h: 36, r: 4 });
  for (const chassis of CHASSIS) for (const cc of CONTROLS) {
    const led = resolveControlsColour(chassis, cc);
    const { Sk, c, ops } = recorder();
    drawKeyLight(Sk, c, cut, cap, 4, a => ledA(led, a), a => hotA(led, a));
    const draws = ops.filter(o => o.op === 'drawRRect');
    const okColour = (s: string) => {
      const m = s.match(/^(rgba|hsla)\((.*),([\d.]+)\)$/);
      if (!m) return false;
      const pre = m[2];
      return [led.rgb, led.hotRgb, led.hsl && `${led.hsl[0]},${led.hsl[1]}%,${led.hsl[2]}%`,
              led.hotHsl && `${led.hotHsl[0]},${led.hotHsl[1]}%,${led.hotHsl[2]}%`].includes(pre);
    };
    ok(`${chassis} ${cc}: every layer is the controls colour (or its hot centre)`, draws.length >= 4 && draws.every(d => okColour(d.color!)));
  }
  // ★★ never on the cap: the first thing the light does is clip the cap OUT, at its sprite position.
  const { Sk, c, ops } = recorder();
  drawKeyLight(Sk, c, cut, cap, 4, a => `rgba(1,2,3,${a})`, a => `rgba(4,5,6,${a})`);
  const clip = ops.find(o => o.op === 'clipRRect')!;
  eq('rest: the cap is clipped OUT (Difference) where it sits in the sprite', [clip.clipOp, clip.rr.x, clip.rr.y, clip.rr.width, clip.rr.height],
     [0, 4 + 2, 4 + 1.5, 66, 44]);
  ok('rest: the clip comes before any light', ops.indexOf(clip) < ops.findIndex(o => o.op === 'drawRRect'));
  // ★★★ PRESSED: the light floods out of the WIDER gap the snap opens — it hugs the SUNK cap.
  const P = recorder();
  drawKeyLight(P.Sk, P.c, cut, cap, 4, a => `rgba(1,2,3,${a})`, a => `rgba(4,5,6,${a})`, true, 2);
  const pclip = P.ops.find(o => o.op === 'clipRRect')!;
  eq('pressed: the SUNK cap (2 pt down, DOME_TRAVEL) is clipped out', [pclip.clipOp, pclip.rr.y], [0, 4 + 1.5 + 2]);
  const hug = P.ops.filter(o => o.op === 'drawRRect')[0];
  ok('pressed: the gap light hugs the sunk cap', Math.abs(hug.rr.y - (4 + 1.5 + 2 - KEY_LIGHT.cap.o)) < 1e-9);
  const restEdge = Math.max(...ops.filter(o => o.op === 'drawRRect').map(o => +o.color!.match(/,([\d.]+)\)$/)![1]));
  ok('pressed: brighter than at rest (the gap opens)', KEY_PRESS_LIGHT.open.a > keyLightLayers(4).edge.a && restEdge > 0);
  // The wash: clipped TO the cap (Intersect), a soft stroke round its edge.
  const Wsh = recorder();
  drawKeyWash(Wsh.Sk, Wsh.c, { x: 0, y: 0, w: 66, h: 44, r: 8 }, a => `rgba(1,2,3,${a})`);
  const wclip = Wsh.ops.find(o => o.op === 'clipRRect')!;
  eq('wash: clipped TO the cap (Intersect), the cap\'s own size', [wclip.clipOp, wclip.rr.width, wclip.rr.height], [1, 66, 44]);
  ok('wash: soft and warm, not a fill', Wsh.ops.some(o => o.op === 'drawRRect' && o.stroke! > 0 && o.sigma! > 0));
}

// ── ★★★ UNLIT = NONE, and the perf rules (source checks: the components need a device to run) ─────────
{
  const dk = code('components/DomeKey.tsx');
  ok('DomeKey: lit only with a reach AND Transparency effects on',
     /const lit = lightReach > 0 && fp\.settings\.transparency === 'on'/.test(dk));
  ok('DomeKey: unlit, the outline key clips as it always did', /overflow: lit \? 'visible' : 'hidden'/.test(dk));
  ok('DomeKey: no reach given = no light (popup and decoder keys)', /lightReach = 0/.test(dk));
  ok('DomeKey: the press fades the ready-made images in on its own `dim` style',
     /pressStyle=\{dim\}/.test(dk) && /<KeyWash[\s\S]*?pressStyle=\{dim\}/.test(dk));
  const cb = code('components/ControlsBar.tsx');
  eq('ControlsBar: all four portrait keys take keyProps (which carries lightReach)', (cb.match(/\{\.\.\.keyProps\}/g) ?? []).length, 4);
  ok('ControlsBar: keyProps carries the light', /height: KEY_SLOT, radius: s\.r\(8\), lightReach/.test(cb) && /minHeight: true, lightReach/.test(cb));
  eq('ControlsBar: all four landscape keys are lit', (cb.match(/radius=\{6\} lightReach=\{(?:chatOff \? 0 : )?lightReach\}/g) ?? []).length, 4);
  // ★ B10: CHAT's lamp goes OFF (no light, unlit legend) on a server without chat — never a grey-out or a dead key.
  ok('ControlsBar: chat with no chat = lamp off, still pressable (portrait + landscape)',
     (cb.match(/lightReach=\{chatOff \? 0 : (?:keyProps\.)?lightReach\}/g) ?? []).length === 2
     && (cb.match(/onPress=\{chatOff \? NOOP : onChat\}/g) ?? []).length === 2
     && !/opacity: chatOff \? 0\.4/.test(cb) && !/disabled=\{chatOff\}/.test(cb));
  const tk = code('components/TunerKeys.tsx');
  ok('TunerKeys: both keys lit through DomeKey (one implementation)', /<DomeKey\s+lightReach=\{lampOff \? 0 : lightReach\}/.test(tk));
  ok('TunerKeys: unlit, the glyph is drawn exactly as today (α .55, 2.6, blur 3, under α .95 1.4)',
     /: <Path path=\{glyphPath\} color=\{G\(0\.55 \* dim\)\} strokeWidth=\{2\.6\}[\s\S]*?<BlurMask blur=\{3\} style="normal" respectCTM \/>/.test(tk)
     && /color=\{lampOff \? LAMP_OFF_INK : G\(0\.95 \* dim\)\} strokeWidth=\{1\.4\}/.test(tk));
  // ★ 2026-10-07: ZOOM's lamp goes OFF in DAB (drum and keys, portrait and landscape) — unlit, never disabled:
  //   it still turns / presses and clicks (disabled_control_reads_as_absent).
  ok('ControlsBar: zoom lamp off in DAB — both drums and both key pairs',
     (cb.match(/<DrumWheel type="zoom"[^>]*lampOff=\{!!dab\}/g) ?? []).length === 2
     && (cb.match(/<TunerKeys type="zoom"[^>]*lampOff=\{!!dab\}/g) ?? []).length === 2
     && !/<(?:DrumWheel|TunerKeys) type="zoom"[^>]*disabled/.test(cb));
  ok('TunerKeys: lamp off = unlit legend + no glyph glow, keys never disabled by it',
     /lampOff \? LAMP_OFF_LEGEND/.test(tk) && /lampOff \? null : lit/.test(tk) && !/disabled[^\n]*lampOff/.test(tk));
  const dw = code('components/DrumWheel.tsx');
  ok('DrumWheel: lamp off = no pool, light transparent, ink unlit, no colour left on the raw LED',
     /\{!lampOff && <DrumPool/.test(dw) && /lampOff \? 'rgba\(0,0,0,0\)' : G\(a\)/.test(dw)
     && /lampOff \? LAMP_OFF_INK : G\(a\)/.test(dw) && !/G\(0\.\d+\)[},]/.test(dw));
  for (const f of ['components/KeyLight.tsx', 'constants/keyLight.ts']) {
    const src = code(f);
    ok(`${f}: no frame callback, no derived value, no live blur`, !/useFrameCallback|useDerivedValue|BlurMask|withRepeat/.test(src));
  }
  ok('KeyLight: images come from makeSprite (spriteSizing vets every size)', /makeSprite\(/.test(code('components/KeyLight.tsx')));
  ok('KeyLight: a failed surface is never cached', /if \(!img\) return null;/.test(code('components/KeyLight.tsx')));
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
// ★ Black is the mockup's #1b1c1e darkened a touch (build 356) — the keys face follows the plate.
eq('keys well faces (Deck.mockup tk.bg; black darkened with its plate)', [D.keysFace, S.keysFace, B.keysFace],
   ['#0b0a08', '#c9c6bf', '#161719']);
eq('black keys face = its plate base', B.keysFace, B.plate!.base);
eq('only the default key needs its own dark slot', [D.keysSlot, S.keysSlot, B.keysSlot], ['#050403', null, null]);
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
                 'keysFace', 'keysSlot', 'wellBorder', 'wellTopLip'] as const) {
  ok(`DEFAULT_CHASSIS has ${k}`, (DEFAULT_CHASSIS as any)[k] !== undefined);
}

console.log(`faceplate wells: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
