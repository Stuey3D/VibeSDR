/**
 * DabMark — the DAB+ mark in the VTS strip, in the RDS mark's slot while a DAB service is on the strip
 * (Stuart, 2026-10-02: "same thing as the RDS logo … when a DAB+ station is playing the whole thing shows,
 * but standard DAB the +] gets turned off, same VFD styling as the RDS logo"). It replaces the green "DAB"
 * text pill, which was the one element of the strip that belonged to no display style.
 *
 *   plain   (hyper, nixie, default) the mark in the strip's mark colour with its glow — as RdsMark plain.
 *   picto   (dot, seg) a VFD annunciator in the glass: lit in the text colour (drop-shadow 2) and seen
 *           through the same 60° mesh as RdsMark's picto mark.
 *
 * ★★ TWO ELECTRODE GROUPS. MAIN = the radio body, its handle and the "dab" letters. PLUS = the "+]": the
 *    right-hand part of the box frame and the plus inside it, split from MAIN by a thin non-emissive cut
 *    through the box's left wall (as RdsMark's electrodes are cut). DAB+ (AAC) lights both. Standard DAB
 *    (MP2) or an unknown codec lights MAIN and leaves the "+]" as an UNLIT electrode — drawn faintly in the
 *    ghost colour, like the dark segments and annunciators beside lit ones (Stuart's final call, 2026-10-02,
 *    after briefly asking for it to vanish). The width never changes, so the text never moves.
 *
 * ★★★ THIRD-PARTY ARTWORK — WORLDDAB'S TERMS. assets/branding/dabplus/README.md records the toolkit's
 *    rules: "presentations other than those in the guide are prohibited — so it is the official artwork,
 *    unaltered, never redrawn", in its colours (or the secondary black/white version), and never under
 *    32 px wide on screen. This mark is drawn from the official PATH (dabLogoPaths.ts, generated from the
 *    unmodified SVG) — not redrawn — but it IS recoloured to the display's colour, cut into two
 *    electrodes, meshed, shown without its "+]" on MP2, and drawn narrower than 32 px. RdsMark makes the
 *    same kind of alterations to the RDS Forum's mark behind RDS_ALTERATIONS_ALLOWED; this mirrors it with
 *    DAB_ALTERATIONS_ALLOWED. ▶ Set it to false if WorldDAB's terms are to be followed to the letter: the
 *    mark is then drawn ONLY for DAB+, in the logo's own gradient, uncut and unmeshed, with nothing on MP2
 *    (its room still kept).
 *
 * ★ Drawn in Skia from the path data, like RdsMark: the glow is rasterised once into a sprite and the
 *   cut and mesh are path operations done once at load.
 */

import React, { useMemo } from 'react';
import { View } from 'react-native';
import { Canvas, Image as SkImageNode, LinearGradient, Path, PathOp, Skia, vec, type SkPath } from '@shopify/react-native-skia';
import { DAB_LOGO_PATH, DAB_VIEWBOX } from './dabLogoPaths';
import { glowPaint, makeSprite } from './glowSprite';

/** ★ See the header: false = WorldDAB's toolkit to the letter (official colours, DAB+ only, unaltered). */
export const DAB_ALTERATIONS_ALLOWED = true;

const VB = DAB_VIEWBOX;

/** The whole official mark, viewBox units (0..100 × 0..59), non-zero fill as the SVG draws it. */
const FULL: SkPath = Skia.Path.MakeFromSVGString(DAB_LOGO_PATH) ?? Skia.Path.Make();

/** The "+]" region: right of the box's left wall, below the body's top edge (the handle stays in MAIN).
 *  The 1-unit gap between the two rectangles is the non-emissive cut between the electrodes. */
const PLUS_RECT = Skia.XYWHRect(67.4, 16.5, 40, 50);
const MAIN_CUT  = Skia.XYWHRect(66.4, 16.5, 40, 50);
const rectPath = (r: ReturnType<typeof Skia.XYWHRect>) => { const p = Skia.Path.Make(); p.addRect(r); return p; };

const PLUS: SkPath = Skia.Path.MakeFromOp(FULL, rectPath(PLUS_RECT), PathOp.Intersect) ?? Skia.Path.Make();
const MAIN: SkPath = Skia.Path.MakeFromOp(FULL, rectPath(MAIN_CUT), PathOp.Difference) ?? FULL;

/** The VFD mesh at RdsMark's proportions (42 units of pitch, 8 wide, on a 260-unit-tall mark). */
function meshOver(p: SkPath): [SkPath, SkPath] {
  const pitch = (42 / 260) * VB.h, bar = (8 / 260) * VB.h;
  const a = Skia.Path.Make(), b = Skia.Path.Make();
  for (let k = -40; k <= 40; k++) {
    a.addRect(Skia.XYWHRect(-200, pitch * k, 400, bar));
    b.addRect(Skia.XYWHRect(pitch * k, -200, bar, 400));
  }
  const m = Skia.Matrix();
  m.rotate((60 * Math.PI) / 180);
  a.transform(m); b.transform(m);
  return [Skia.Path.MakeFromOp(a, p, PathOp.Intersect) ?? a, Skia.Path.MakeFromOp(b, p, PathOp.Intersect) ?? b];
}
const MESH_MAIN = meshOver(MAIN);
const MESH_PLUS = meshOver(PLUS);

/** Mark width for a height — the FULL mark's, whichever groups are drawn. */
export const dabMarkWidth = (h: number) => (h * VB.w) / VB.h;

const MARGIN = 8;

function scaled(p: SkPath, k: number): SkPath {
  const c = p.copy();
  const m = Skia.Matrix();
  m.translate(MARGIN, MARGIN);
  m.scale(k, k);
  c.transform(m);
  return c;
}

export default function DabMark({ height = 15, kind, color, glow, ghost, plus }: {
  height?: number;
  kind: 'plain' | 'picto';
  /** The lit colour (the strip's mark colour plain, its text colour picto; neon under Nixie). */
  color: string;
  glow: string | null;
  /** The unlit electrode colour — the "+]" on MP2 / an unknown codec. */
  ghost: string;
  /** DAB+ (AAC): the "+]" group lit. False for MP2 or an unknown codec — drawn as a ghost. */
  plus: boolean;
}) {
  const k = height / VB.h;
  const w = dabMarkWidth(height);
  const W = w + 2 * MARGIN, H = height + 2 * MARGIN;
  const altered = DAB_ALTERATIONS_ALLOWED;
  // The lit groups and their glow, rasterised once per colour × size × codec.
  const sprite = useMemo(() => {
    if (!altered) return null;
    const p = scaled(MAIN, k);
    if (plus) p.addPath(scaled(PLUS, k));
    return makeSprite(W, H, (c) => {
      if (glow) c.drawPath(p, glowPaint(glow, kind === 'picto' ? 2 : 5));
      c.drawPath(p, glowPaint(color));
    });
  }, [k, W, H, color, glow, kind, plus, altered]);
  const statics = useMemo(() => ({
    full: scaled(FULL, k),
    plusGhost: scaled(PLUS, k),
    meshMain: kind === 'picto' && altered ? [scaled(MESH_MAIN[0], k), scaled(MESH_MAIN[1], k)] : null,
    meshPlus: kind === 'picto' && altered ? [scaled(MESH_PLUS[0], k), scaled(MESH_PLUS[1], k)] : null,
  }), [k, kind, altered]);

  // ★ Unaltered mode: the official artwork in its own gradient, DAB+ only (the README's rule).
  if (!altered) {
    return (
      <View style={{ width: w, height }} pointerEvents="none" accessibilityRole="image" accessibilityLabel={plus ? 'DAB+' : 'DAB'}>
        {plus && (
          <Canvas style={{ position: 'absolute', left: -MARGIN, top: -MARGIN, width: W, height: H }}>
            <Path path={statics.full}>
              <LinearGradient start={vec(MARGIN, 0)} end={vec(MARGIN + w, 0)} colors={['#00BDD2', '#86DD25']} />
            </Path>
          </Canvas>
        )}
      </View>
    );
  }
  return (
    <View style={{ width: w, height }} pointerEvents="none" accessibilityRole="image" accessibilityLabel={plus ? 'DAB+' : 'DAB'}>
      <Canvas style={{ position: 'absolute', left: -MARGIN, top: -MARGIN, width: W, height: H }}>
        {!plus && <Path path={statics.plusGhost} color={ghost} />}
        {sprite && <SkImageNode image={sprite} x={0} y={0} width={W} height={H} />}
        {statics.meshMain && <Path path={statics.meshMain[0]} color="rgba(0,0,0,0.55)" />}
        {statics.meshMain && <Path path={statics.meshMain[1]} color="rgba(0,0,0,0.35)" />}
        {statics.meshPlus && <Path path={statics.meshPlus[0]} color="rgba(0,0,0,0.55)" />}
        {statics.meshPlus && <Path path={statics.meshPlus[1]} color="rgba(0,0,0,0.35)" />}
      </Canvas>
    </View>
  );
}
