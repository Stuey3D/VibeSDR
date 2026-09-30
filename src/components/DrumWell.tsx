/**
 * DrumWell — the parts of a drum well that DrumWheel and TunerKeys share (faceplates §6.1, §6.2):
 * the face, the edge (border, ring, glow) and the LED pool. Skia components, drawn inside the
 * caller's <Canvas>. The rules and numbers are constants/drumWell.ts; the colours are the chassis
 * tokens and the controls colour.
 *
 * ★★ The well's face, border and glow are ONE implementation so the keys well is the drum well with
 *   the drum swapped out (Stuart, 2026-09-30: the recess and the ring are what he likes) — two
 *   copies would drift apart the first time either was adjusted.
 * ★ Everything here is STATIC: it redraws only when the size, chassis or colour changes. The
 *   callers put it in canvases the drum's rolling notches never touch.
 */

import React from 'react';
import {
  BlurMask, Circle, Group, ImageShader, Line, LinearGradient, RadialGradient, Rect, RoundedRect,
  Skia, vec,
} from '@shopify/react-native-skia';
import { useTexture, TEXTURE_SAMPLING } from './DomeKey';
import { ledA, type ChassisTokens, type LedColour } from '../constants/faceplate';
import { POOL, WELL_GLOW_BLUR, WELL_R, poolEllipse } from '../constants/drumWell';

/**
 * The panel face. Default drum well: TODAY'S machined gradient, drawn exactly as DrumWheel always
 * drew it. Metal: the brushed grain over its base colour (Deck.mockup `.tex-silver` / `.tex-black`,
 * shown at 620 pt wide and centred on the well). `keys`: the tuner-keys face (`tk.bg`).
 */
export function WellFace({ W, H, ct, keys = false }: {
  W: number; H: number; ct: ChassisTokens; keys?: boolean;
}) {
  const img = useTexture(ct.wellTexture);
  if (!ct.wellTexture && !keys) {
    return (
      <RoundedRect x={0} y={0} width={W} height={H} r={WELL_R}>
        <LinearGradient start={vec(0, 0)} end={vec(0, H)} colors={ct.wellFace} positions={[0, 0.4, 1]} />
      </RoundedRect>
    );
  }
  const k = 620 / 1200;
  return (
    <Group clip={Skia.RRectXY(Skia.XYWHRect(0, 0, W, H), WELL_R, WELL_R)}>
      <Rect x={0} y={0} width={W} height={H} color={keys ? ct.keysFace : ct.wellFace[0]} />
      {img && ct.wellTexture && (
        <Rect x={0} y={0} width={W} height={H}>
          <ImageShader image={img} tx="mirror" ty="mirror" fit="none" sampling={TEXTURE_SAMPLING}
            transform={[{ translateX: W / 2 - 600 * k }, { translateY: H / 2 - 450 * k }, { scale: k }]} />
        </Rect>
      )}
    </Group>
  );
}

/**
 * The well's edge, drawn in a canvas `M` larger than the well on every side (wellOutset) so the
 * metal ring and glow can sit OUTSIDE it, as the mockup's box-shadows do.
 *   • default: today's inner glow (G .10, 5 pt, blur 6) and the lit 0.9 pt border (G .70) — as ever.
 *   • metal:   the dark 0.9 pt gap, the 1 pt controls-colour ring outside it, the 8 pt glow beyond
 *              (`0 0 0 1px L(a), 0 0 8px L(a)`), and the face's top lip (`inset 0 1px 0`).
 */
export function WellEdge({ W, H, M, ct, led }: {
  W: number; H: number; M: number; ct: ChassisTokens; led: LedColour;
}) {
  return (
    <Group transform={[{ translateX: M }, { translateY: M }]}>
      {ct.wellGlowA > 0 && (
        // CSS blur radius 8 ≈ sigma 4; `outer` so the glow is only outside the box, as a box-shadow is.
        <RoundedRect x={0} y={0} width={W} height={H} r={WELL_R} color={ledA(led, ct.wellGlowA)}>
          <BlurMask blur={WELL_GLOW_BLUR / 2} style="outer" respectCTM />
        </RoundedRect>
      )}
      {ct.wellRingA > 0 && (
        <RoundedRect x={-0.5} y={-0.5} width={W + 1} height={H + 1} r={WELL_R + 0.5}
          color={ledA(led, ct.wellRingA)} strokeWidth={1} style="stroke" />
      )}
      {ct.wellTopLip && (
        <Line p1={vec(WELL_R * 0.7, 1.4)} p2={vec(W - WELL_R * 0.7, 1.4)} color={ct.wellTopLip} strokeWidth={1} />
      )}
      {ct.wellInnerGlowA > 0 && (
        <RoundedRect x={1} y={1} width={W - 2} height={H - 2} r={WELL_R}
                     color={ledA(led, ct.wellInnerGlowA)} strokeWidth={5} style="stroke">
          <BlurMask blur={6} style="normal" respectCTM />
        </RoundedRect>
      )}
      <RoundedRect x={0.5} y={0.5} width={W - 1} height={H - 1} r={WELL_R}
                   color={ct.wellBorder ?? ledA(led, 0.70)} strokeWidth={0.9} style="stroke" />
    </Group>
  );
}

/**
 * ★★ §6.1: THE LED GLOWING THROUGH FROM BEHIND THE DRUM — what replaces the red index needle on
 * every chassis. A radial pool in the controls colour over the drum body (x, y, w, h), clipped to
 * it. CSS's elliptical radial is a circle squashed about its centre.
 */
export function DrumPool({ x, y, w, h, led }: {
  x: number; y: number; w: number; h: number; led: LedColour;
}) {
  const e = poolEllipse(x, y, w, h);
  if (!(e.rx > 0) || !(e.ry > 0)) return null;
  return (
    <Group clip={Skia.XYWHRect(x, y, w, h)}>
      <Group origin={vec(e.cx, e.cy)} transform={[{ scaleY: e.ry / e.rx }]}>
        <Circle cx={e.cx} cy={e.cy} r={e.rx}>
          <RadialGradient c={vec(e.cx, e.cy)} r={e.rx}
            colors={[ledA(led, POOL.alphas[0]), ledA(led, POOL.alphas[1]), 'rgba(0,0,0,0)']}
            positions={POOL.positions} />
        </Circle>
      </Group>
    </Group>
  );
}
