/**
 * RdsMark — the RDS mark in the VTS strip (faceplates brief §7.1; refs docs/faceplates/rds/R1, R2;
 * Deck.mockup `rdsMark`, `rdsCut`, `vfdMesh`). Replaces the fixed black-on-white `rds-logo.png`.
 *
 *   plain   (hyper, nixie) the mark in the resolved text colour — neon under Nixie — with that
 *           colour's glow (none on the default chassis); shown only on an RDS signal.
 *   picto   (dot, seg) a VFD ANNUNCIATOR: a fixed electrode built into the glass. Always there — a
 *           ghost at text colour α .10 — lit in the text colour (drop-shadow 2) only while RDS
 *           decodes; cut into its electrodes by two non-emissive gaps; seen through the 60° grid mesh.
 * ★ Never the "ADVANCED" wording: that belongs to the Advanced RDS mode button and the directory.
 *
 * ★★★ THIRD-PARTY ARTWORK — OPEN LICENSING QUESTION. The mark is the RDS Forum's. The repo carries
 *   no logo-use terms for it (assets/branding/rds/ has only the SVG), unlike DAB+, whose toolkit
 *   forbids alterations (assets/branding/dabplus/README.md). Recolouring it and cutting it (picto)
 *   are alterations. If the RDS Forum's terms forbid them, set RDS_ALTERATIONS_ALLOWED = false: the
 *   mark then stays ONE colour (the text colour, no glow) and uncut, on every display.
 *
 * ★ Drawn in Skia from the SVG's own path data (rdsLogoPaths.ts), not react-native-svg: the glow is
 *   rasterised ONCE into a sprite (glowSprite.ts) and the cut and mesh are path operations done once
 *   at load — react-native-svg would re-run a filter on every paint.
 */

import React, { useMemo } from 'react';
import { View } from 'react-native';
import { Canvas, Image as SkImageNode, Path, Skia, type SkPath } from '@shopify/react-native-skia';
import { RDS_LOGO_PATHS, RDS_GROUP_DX, RDS_GROUP_DY, RDS_VIEWBOX } from './rdsLogoPaths';
import { glowPaint, imageBuild, makeSprite, useSharedSprite } from './glowSprite';
import { MESH_SHADES, placePath, vfdCut, vfdMesh } from './vfdMesh';

/** ★ Flip to false if the RDS Forum's logo terms forbid recolouring / cutting (see the header). */
export const RDS_ALTERATIONS_ALLOWED = true;

const VB = RDS_VIEWBOX;

/** The mark in viewBox-local units (0..1063 × 0..260.3). */
const MARK: SkPath = (() => {
  const p = Skia.Path.Make();
  for (const [d, dx, dy] of RDS_LOGO_PATHS) {
    const sp = Skia.Path.MakeFromSVGString(d);
    if (!sp) continue;
    sp.offset(dx + RDS_GROUP_DX - VB.x, dy + RDS_GROUP_DY - VB.y);
    p.addPath(sp);
  }
  return p;
})();

/** §7.1 electrode cuts, logo units: (210,30)→(238,300) 16 wide; (30,193)→(440,193) 13 wide. */
const CUT_MARK: SkPath = vfdCut(MARK, [[210, 30, 238, 300, 16], [30, 193, 440, 193, 13]], -VB.x, -VB.y);

/** §7.1 VFD mesh: a 42-unit pattern at 60°, bars 8 wide — α .55 one way, .35 the other — over the mark. */
const MESH: [SkPath, SkPath] = vfdMesh(CUT_MARK, 42, 8, { count: 40, extent: 2000, dx: -VB.x, dy: -VB.y });

/** Mark width for a height (≈ 53 pt at 13). */
export const rdsMarkWidth = (h: number) => (h * VB.w) / VB.h;

const MARGIN = 8;

const scaled = placePath;

export default function RdsMark({ height = 13, kind, color, glow, ghost, lit = true }: {
  height?: number;
  kind: 'plain' | 'picto';
  /** The lit colour (the resolved text colour; neon under Nixie). */
  color: string;
  /** Its glow colour, or null for none. */
  glow: string | null;
  /** picto: the unlit electrode colour (text colour α .10). */
  ghost?: string;
  /** picto: lit while RDS decodes. */
  lit?: boolean;
}) {
  const k = height / VB.h;
  const w = rdsMarkWidth(height);
  const altered = RDS_ALTERATIONS_ALLOWED;
  const src = kind === 'picto' && altered ? CUT_MARK : MARK;
  // The lit mark and its glow, rasterised once per colour × size.
  // ★ Shared across instances and freed when unused — see useSharedSprite. `src` follows kind + altered.
  const sprite = useSharedSprite(`rds|${kind}|${altered}|${k}|${w}|${height}|${color}|${glow}`, () => {
    const W = w + 2 * MARGIN, H = height + 2 * MARGIN;
    const p = scaled(src, k, MARGIN, MARGIN);
    const g = altered ? glow : null;
    return imageBuild(makeSprite(W, H, (c) => {
      if (g) c.drawPath(p, glowPaint(g, kind === 'picto' ? 2 : 5));
      c.drawPath(p, glowPaint(color));
    }));
  });
  const statics = useMemo(() => kind === 'picto' ? {
    ghost: scaled(src, k, MARGIN, MARGIN),
    meshA: altered ? scaled(MESH[0], k, MARGIN, MARGIN) : null,
    meshB: altered ? scaled(MESH[1], k, MARGIN, MARGIN) : null,
  } : null, [kind, src, k, altered]);

  return (
    <View style={{ width: w, height }} pointerEvents="none" accessibilityRole="image" accessibilityLabel="RDS">
      <Canvas style={{ position: 'absolute', left: -MARGIN, top: -MARGIN, width: w + 2 * MARGIN, height: height + 2 * MARGIN }}>
        {statics && ghost && <Path path={statics.ghost} color={ghost} />}
        {sprite && (kind === 'plain' || lit) && (
          <SkImageNode image={sprite} x={0} y={0} width={w + 2 * MARGIN} height={height + 2 * MARGIN} />
        )}
        {statics?.meshA && <Path path={statics.meshA} color={MESH_SHADES[0]} />}
        {statics?.meshB && <Path path={statics.meshB} color={MESH_SHADES[1]} />}
      </Canvas>
    </View>
  );
}
